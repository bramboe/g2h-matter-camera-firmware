"""Install a qualified candidate into a separate slot; retain the working app.

No in-place deletion, radio flashing, fabric edits, monitor or vendor stop.
An independent on-camera timer rolls back an unconfirmed candidate. A reboot
with a pending transaction selects the previous app before launching Matter.
"""
import argparse, getpass, hashlib, io, json, os, re, shlex, struct, tarfile, time, uuid
from pathlib import Path
from g2h_identification_probe import Telnet
from g2h_upload_telnet_bulk import upload
from g2h_backup_live import create_backup, identity, safe_status, private_json, app_identity
Q=shlex.quote
HERE=Path(__file__).resolve().parent
CFG='/mnt/config/g2h-matter-camera'
BASE='/customer/g2h-matter-camera'
INIT='/etc/init.d/S96g2hmattercamera'
BROKER=BASE+'/g2h-media-broker'
PENDING=CFG+'/update-pending'
GUARD='''#!/bin/sh
trap '' HUP
pending=/mnt/config/g2h-matter-camera/update-pending
printf '%s\n' "$$" > "$pending/guard.pid"
awk '{print $22}' "/proc/$$/stat" > "$pending/guard.birth"
printf '%s\n' "$1" > "$pending/guard.ready"
sleep 180
[ -r "$pending/candidate" ] || exit 0
read candidate < "$pending/candidate"
[ "$candidate" = "$1" ] || exit 0
/etc/init.d/S96g2hmattercamera rollback
'''

def image_bytes(path):
 if path.is_symlink() or not 65536<=path.stat().st_size<=12*1024*1024:
  raise ValueError('candidate must be a bounded regular ARM ELF')
 data=path.read_bytes()
 if data[:7]!=b'\x7fELF\x01\x01\x01':raise ValueError('32-bit little-endian ELF required')
 kind,machine,version,entry,phoff,_,flags,ehsize,phsize,phnum,*_=struct.unpack_from('<HHIIIIIHHHHHH',data,16)
 if (kind,machine,version,ehsize,phsize)!=(2,40,1,52,32) or flags>>24!=5 or not 1<=phnum<=64 or phoff<52 or phoff+phsize*phnum>len(data):
  raise ValueError('static ARM EABI5 executable headers required')
 executable_entry=False;loads=0
 for i in range(phnum):
  kind,offset,address,_,filesz,memsz,permissions,align=struct.unpack_from('<8I',data,phoff+i*phsize)
  if kind==3:raise ValueError('dynamic interpreter is unsupported')
  if kind!=1:continue
  loads+=1
  if filesz>memsz or offset+filesz>len(data) or address+memsz>2**32 or (align>1 and (align&(align-1) or offset%align!=address%align)):
   raise ValueError('invalid executable load segment')
  if permissions&1 and address<=entry<address+filesz:executable_entry=True
 if not loads or not executable_entry:raise ValueError('entry point outside executable file-backed segment')
 return data

def packed_image(image):
 output=io.BytesIO()
 with tarfile.open(fileobj=output,mode='w:gz') as archive:
  info=tarfile.TarInfo('camera');info.size=len(image);info.mode=0o700;info.mtime=0
  archive.addfile(info,io.BytesIO(image))
 return output.getvalue()

def selected_path(text):
 path=text.strip() or BASE+'/camera'
 if path!=BASE+'/camera' and not re.fullmatch(r'/(?:customer|mnt/sdcard)/g2h-matter-camera/releases/[0-9a-f]{64}/camera',path):
  raise ValueError('unrecognized active app path')
 return path

def free_kb(session,path):
 text=session.run('df -k '+Q(path)+' | awk \'NR==2 {print $4}\'').strip()
 if not text.isdigit():raise ValueError('disk capacity unavailable')
 return int(text)

def checksum(session,path):
 parts=session.run('md5sum '+Q(path),timeout=30).split()
 if len(parts)!=2 or parts[1]!=path or not re.fullmatch('[0-9a-f]{32}',parts[0]):raise ValueError('invalid checksum')
 return parts[0]

def install_small(session,stage,name,destination,data):
 upload(session,stage,name,data,budget_seconds=30)
 session.run('set -e\ncp '+Q(stage+'/'+name)+' '+Q(destination+'.next')+'\nchmod 700 '+Q(destination+'.next')+'\nmv '+Q(destination+'.next')+' '+Q(destination)+'\nsync',timeout=30)
 if checksum(session,destination)!=hashlib.md5(data).hexdigest():raise RuntimeError('installed helper mismatch')
 session.run('rm -f '+Q(stage+'/'+name))

def native_streams(session,pid):
    if type(pid) is not int or pid<2:raise ValueError('invalid app PID')
    fds=session.run('ls -l /proc/'+str(pid)+'/fd',timeout=10)
    owned=set(re.findall(r'socket:\[([0-9]+)\]',fds))
    rows=session.run('cat /proc/net/tcp').splitlines()[1:]
    return any(len(f:=line.split())>=10 and f[2]=='0100007F:022A' and f[3]=='01' and f[9] in owned for line in rows)

def guardian_ready(session,candidate):
    command='set -e\n[ "$(cat '+PENDING+'/guard.ready)" = '+Q(candidate)+' ]\np=$(cat '+PENDING+'/guard.pid)\nb=$(cat '+PENDING+'/guard.birth)\n[ "$(awk \'{print $22}\' /proc/$p/stat)" = "$b" ]'
    session.run(command)

def verify_backup(directory,old,before,status):
    if directory.is_symlink() or time.time()-directory.stat().st_mtime>600:
        raise ValueError('fresh private backup directory required')
    manifest=json.loads((directory/'manifest.json').read_text())
    if manifest.get('complete') is not True or manifest.get('before')!=before or manifest.get('after')!=before:
        raise ValueError('backup belongs to another camera lifetime or is incomplete')
    for snapshot in ('status_before','status_after'):
        if any(manifest.get(snapshot,{}).get(k)!=status.get(k) for k in ('pid','fabrics')):
            raise ValueError('backup app identity differs')
    row=next((r for r in manifest['files'] if r['local_file']=='matter-camera-active' and r['remote_path']==old),None)
    if not row:raise ValueError('selected executable was not backed up')
    local=directory/row['local_file']
    if local.is_symlink() or local.stat().st_size!=row['bytes'] or hashlib.sha256(local.read_bytes()).hexdigest()!=row['sha256']:
        raise ValueError('private backup data changed')
    return directory

def upgrade(session,image,broker,backup_root,storage='customer',qualify_seconds=30,prepared_backup=None):
 if storage not in ('customer','sd') or not 15<=qualify_seconds<=90:raise ValueError('invalid bounded update settings')
 digest=hashlib.sha256(image).hexdigest()
 payload=packed_image(image)
 old=selected_path(session.run('if [ -r '+CFG+'/app-path ]; then cat '+CFG+'/app-path; fi'))
 old_md5=checksum(session,old)
 if old_md5==hashlib.md5(image).hexdigest():return {'unchanged':True,'sha256':digest}
 session.run('[ ! -e '+PENDING+' ]')
 before=identity(session);status=safe_status(session)
 app_identity(session,status,old)
 previous_broker=session.run('if [ -f '+CFG+'/media-broker-enabled ]; then echo 1; else echo 0; fi').strip()
 if previous_broker not in ('0','1'):raise RuntimeError('broker state unknown')
 if previous_broker=='0' and native_streams(session,status['pid']):raise RuntimeError('first broker migration requires a clean boot with no Matter streams opened')
 if status.get('video_session'):raise RuntimeError('close the current Matter live view before upgrading')
 backup=create_backup(session,backup_root) if prepared_backup is None else verify_backup(prepared_backup,old,before,status)
 # All mutation starts only after a fresh exact download has been verified.
 root=BASE if storage=='customer' else '/mnt/sdcard/g2h-matter-camera'
 if storage=='sd':session.run("mount | grep -q ' on /mnt/sdcard '")
 capacity='/customer' if storage=='customer' else '/mnt/sdcard'
 if free_kb(session,capacity)<(len(image)+len(broker)+1023)//1024+1024:
  raise RuntimeError('insufficient space for separate candidate and 1MiB margin; original retained')
 if free_kb(session,'/tmp')<(len(payload)+1023)//1024+4096:
  raise RuntimeError('insufficient staging memory; original retained')
 slot=root+'/releases/'+digest
 candidate=slot+'/camera'
 stage='/tmp/g2h_upgrade_'+uuid.uuid4().hex
 record={'format':1,'backup':str(backup),'candidate_sha256':digest,'transport_bytes':len(payload),'stage':stage,'old_md5':old_md5,'previous':old,'candidate':candidate,'before':before,'previous_broker':previous_broker,'verified':False}
 switched=False
 try:
  session.run('set -e\numask 077\nmkdir '+Q(stage)+'\nmkdir -p '+Q(slot))
  present_slot=session.run('if [ -e '+Q(candidate)+' ]; then echo yes; fi').strip()
  if not present_slot:
   upload(session,stage,'candidate-archive',payload,budget_seconds=480)
   session.run('set -e\ntar -xzf '+Q(stage+'/candidate-archive')+' -C '+Q(slot)+'\nchmod 700 '+Q(candidate)+'\nsync\nrm -f '+Q(stage+'/candidate-archive'),timeout=150)
  if checksum(session,candidate)!=hashlib.md5(image).hexdigest():raise RuntimeError('candidate slot mismatch')
  present=session.run('if [ -e '+Q(BROKER)+' ]; then echo yes; fi').strip()
  if present:
   if checksum(session,BROKER)!=hashlib.md5(broker).hexdigest():raise RuntimeError('existing broker differs; separate broker migration required')
  else:install_small(session,stage,'broker',BROKER,broker)
  for name in ('S95g2hmediabroker','S96g2hmattercamera'):
   install_small(session,stage,name,'/etc/init.d/'+name,(HERE/'services'/name).read_bytes())
  install_small(session,stage,'guard',CFG+'/update-guard.sh',GUARD.encode())
  # A pending directory appears atomically, never a partially filled transaction.
  transaction=CFG+'/update-'+uuid.uuid4().hex
  session.run('set -e\nmkdir '+Q(transaction)+'\nprintf \'%s\\n\' '+Q(old)+' > '+Q(transaction+'/previous')+'\nprintf \'%s\\n\' '+Q(candidate)+' > '+Q(transaction+'/candidate')+'\nprintf \'%s\\n\' '+Q(previous_broker)+' > '+Q(transaction+'/previous-broker')+'\ncat /proc/sys/kernel/random/boot_id > '+Q(transaction+'/boot')+'\nsync\nmv '+Q(transaction)+' '+Q(PENDING)+'\nsync')
  switched=True
  session.run('sh '+Q(CFG+'/update-guard.sh')+' '+Q(candidate)+' </dev/null >/dev/null 2>&1 &')
  time.sleep(1);guardian_ready(session,candidate)
  if identity(session)!=before or checksum(session,old)!=old_md5:raise RuntimeError('host or previous app changed before switch')
  fresh_status=safe_status(session)
  app_identity(session,fresh_status,old)
  if previous_broker=='0' and native_streams(session,fresh_status['pid']):raise RuntimeError('legacy native stream opened during staging; clean boot required')
  session.run('set -e\nprintf \'%s\\n\' '+Q(candidate)+' > '+Q(CFG+'/app-path.next')+'\nmv '+Q(CFG+'/app-path.next')+' '+Q(CFG+'/app-path')+'\n: > '+Q(CFG+'/media-broker-enabled')+'\nsync')
  session.run(INIT+' stop',timeout=20)
  session.run(INIT+' start',timeout=12)
  deadline=time.monotonic()+qualify_seconds+20
  stable_since=None
  while time.monotonic()<deadline:
   time.sleep(2)
   now=identity(session)
   if now!=before:raise RuntimeError('host or vendor camera restarted; candidate not accepted')
   try:current=safe_status(session)
   except Exception:stable_since=None;continue
   app_identity(session,current,candidate)
   if current['fabrics']!=status['fabrics']:raise RuntimeError('fabric count changed')
   if status.get('thread_fresh') and not current.get('thread_fresh'):stable_since=None;continue
   if current.get('source_fault'):raise RuntimeError('candidate media source fault')
   if current.get('media_broker') is not True:raise RuntimeError('candidate is not using the independent media broker')
   if stable_since is None:stable_since=time.monotonic()
   if time.monotonic()-stable_since>=qualify_seconds:break
  else:raise RuntimeError('candidate did not remain ready for qualification window')
  # Atomic exclusion from the independent timeout action. Commit preserves a
  # durable previous pointer for explicit rollback after the test window.
  if checksum(session,candidate)!=hashlib.md5(image).hexdigest():raise RuntimeError('candidate changed during qualification')
  session.run('set -e\nmkdir '+Q(PENDING+'/action.lock')+'\ntrap '+Q('rmdir '+Q(PENDING+'/action.lock')+' 2>/dev/null || :')+' EXIT\ncp '+Q(PENDING+'/previous')+' '+Q(CFG+'/app-path.previous.next')+'\nmv '+Q(CFG+'/app-path.previous.next')+' '+Q(CFG+'/app-path.previous')+'\nmv '+Q(PENDING)+' '+Q(CFG+'/update-complete-'+digest)+'\nsync')
  switched=False
  record.update(verified=True,status=current,after=identity(session))
  private_json(backup/'upgrade.json',record)
  return record
 except BaseException:
  if switched:
   try:session.run(INIT+' rollback',timeout=30);record['rollback_requested']=True
   except Exception:record['rollback_waiting_for_guardian_or_reboot']=True
  private_json(backup/'upgrade.json',record)
  raise
 finally:
  try:session.run('rm -f '+Q(stage+'/candidate')+' '+Q(stage+'/candidate-archive')+'\nrmdir '+Q(stage)+' 2>/dev/null || :')
  except Exception:pass

def main(argv=None):
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--host',default=os.environ.get('G2H_IP') or os.environ.get('CAMERA_IP'))
 p.add_argument('--image',type=Path,required=True);p.add_argument('--broker',type=Path,required=True)
 p.add_argument('--storage',choices=('customer','sd'),default='customer')
 p.add_argument('--backup-root',type=Path,default=HERE.parents[1]/'private-g2h-backups')
 p.add_argument('--qualify-seconds',type=int,default=30)
 p.add_argument('--verified-backup',type=Path,help='Reuse a verified backup from this exact process lifetime, at most ten minutes old')
 a=p.parse_args(argv)
 if not a.host:p.error('provide --host or G2H_IP')
 image=image_bytes(a.image);broker=image_bytes(a.broker)
 pw=getpass.getpass('Camera root Telnet password: ');s=Telnet(a.host)
 try:
  s.login(pw);pw=None
  result=upgrade(s,image,broker,a.backup_root,a.storage,a.qualify_seconds,a.verified_backup)
  print('UNCHANGED' if result.get('unchanged') else 'CANDIDATE_READY_IDENTITY_PRESERVED',result.get('candidate_sha256',result.get('sha256')))
 finally:pw=None;s.close()
if __name__=='__main__':main()

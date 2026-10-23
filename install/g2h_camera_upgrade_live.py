import getpass,hashlib,json,os,shlex,sys,time,uuid
from pathlib import Path
HERE=Path(__file__).resolve().parent;PKG=HERE.parent;sys.path.insert(0,str(HERE))
from g2h_identification_probe import Telnet
from g2h_upload_telnet_bulk import upload
Q=shlex.quote
password=getpass.getpass('Camera password: ');s=Telnet('192.168.1.50');s.login(password);del password
d='/tmp/g2h_camera_upgrade_'+uuid.uuid4().hex;base='/customer/g2h-matter-camera';init='/etc/init.d/S96g2hmattercamera'
record={'stage':d};copied=False;switched=False;stopped=False;inplace=False
# --in-place: /customer cannot hold two app copies; the rollback copy is kept in /tmp (RAM) instead.
IN_PLACE='--in-place' in sys.argv[1:]
# 2026-10-22 22:38: two 8 MiB copies in /tmp coincided with a vendor camera restart. So the
# rollback is NOT kept in RAM: it is the previous build kept on the Mac, re-uploaded on failure.
KEEP=HERE/'builds'
def local_build(md5):
    for f in KEEP.glob('*.camera'):
        data=f.read_bytes()
        if hashlib.md5(data).hexdigest()==md5:return data
def free_kb():return int(s.run('df -k /customer | tail -1').split()[3])
try:
 record['boot_before']=s.run('cat /proc/sys/kernel/random/boot_id').strip();record['camera_before']=s.run("ps | grep '[c]amera -M'").split()[0]
 record['status_before']=json.loads(s.run(Q(init)+' status'))
 assert not record['status_before']['video_session'],'Close the own camera session before upgrading'
 record['old_md5']=s.run('md5sum '+Q(base+'/camera')).split()[0]
 image=(PKG/'g2h-matter-camera').read_bytes();md5=hashlib.md5(image).hexdigest()
 # A second run of the same build must change nothing (20:15 incident).
 assert md5!=record['old_md5'],'This build is already installed; nothing changed'
 previous_exists=s.run('if [ -f '+Q(base+'/camera.previous')+' ]; then echo yes; fi').strip()
 if previous_exists:
  latest=sorted((HERE/'journals').glob('camera-upgrade-*.private.json'))[-1]
  prior=json.loads(latest.read_text());assert prior.get('verified')
  backup_md5=s.run('md5sum '+Q(base+'/camera.previous')).split()[0]
  assert backup_md5==prior['old_md5'],'Unrecognized prior rollback copy'
  s.run('rm -f '+Q(base+'/camera.previous'));record['rotated_previous_md5']=backup_md5
 s.run('[ ! -e '+Q(base+'/camera.next')+' ]\n[ ! -e '+Q(base+'/camera.previous')+' ]')
 s.run('umask 077\nmkdir '+Q(d))
 record['new_sha256']=hashlib.sha256(image).hexdigest()
 upload(s,d,'camera-next',image,budget_seconds=120) # 8 MiB over Telnet; 30 s was too tight on 2026-10-23
 if IN_PLACE:
  rollback=local_build(record['old_md5']);assert rollback,'previous build is not kept locally; refusing --in-place'
  s.run(Q(init)+' stop',timeout=12);stopped=True
  inplace=True # from here on a failure re-uploads the previous build from the Mac
  s.run('rm -f '+Q(base+'/camera')+'\ncp '+Q(d+'/camera-next')+' '+Q(base+'/camera')+'\nchmod 700 '+Q(base+'/camera')+'\nrm -f '+Q(d+'/camera-next'),timeout=150)
  assert s.run('md5sum '+Q(base+'/camera')).split()[0]==md5,'installed copy mismatch'
  record['free_kb_after_copy']=free_kb()
 # JFFS2 compression can take longer than the Telnet helper default timeout.
 if not IN_PLACE:s.run('cp '+Q(d+'/camera-next')+' '+Q(base+'/camera.next')+'\nchmod 700 '+Q(base+'/camera.next'),timeout=45);copied=True
 if not IN_PLACE:
  assert s.run('md5sum '+Q(base+'/camera.next')).split()[0]==md5
  # JFFS2 renames need free space too; never stop the service without a margin.
  record['free_kb_after_copy']=free_kb()
  if record['free_kb_after_copy']<512:
   s.run('rm -f '+Q(base+'/camera.next'));raise RuntimeError('Not enough space on /customer after copying; nothing switched, service untouched (retry with --in-place)')
  s.run(Q(init)+' stop',timeout=12);stopped=True
  s.run('mv '+Q(base+'/camera')+' '+Q(base+'/camera.previous')+'\nmv '+Q(base+'/camera.next')+' '+Q(base+'/camera'));switched=True
 s.run(Q(init)+' start')
 for _ in range(16):
  raw=s.run(Q(init)+' status || :').strip()
  if raw.startswith('{'):
   status=json.loads(raw)
   if 'last_video_stage' in status:break
  time.sleep(1)
 else:raise RuntimeError('Updated service readiness not confirmed')
 assert status['fabrics']==record['status_before']['fabrics'],'Existing Matter fabrics changed'
 record['status']=status
 record['boot_after']=s.run('cat /proc/sys/kernel/random/boot_id').strip()
 assert record['boot_before']==record['boot_after'],'host rebooted during the upgrade'
 # Stopping an app that had a /13 session has restarted the vendor camera. That is a
 # warning, not a reason to roll back (rolling back is what left the service stopped),
 # provided the vendor process is back and RTSP listens within 60 s.
 for _ in range(12):
  cam=s.run("ps | grep '[c]amera -M' || :").split()
  rtsp='0A' in s.run("awk 'NR>1 && $2 ~ /:022A$/ {print $4}' /proc/net/tcp || :")
  if cam and rtsp:break
  time.sleep(5)
 assert cam and rtsp,'vendor camera or RTSP not back within 60 s'
 record['camera_after']=cam[0]
 if record['camera_after']!=record['camera_before']:record['warning_vendor_restarted']=True;print('WARNING_VENDOR_CAMERA_RESTARTED',record['camera_before'],'->',record['camera_after'],flush=True)
 record['verified']=True;print('UPGRADED_APP_READY',json.dumps(status),flush=True)
 # Keep every installed build locally: the build output is overwritten by the next build.
 keep=HERE/'builds';keep.mkdir(exist_ok=True);(keep/(record['new_sha256']+'.camera')).write_bytes(image)
except BaseException as e:
 record['error_private']=str(e)
 if inplace:
  try:
   s.run(Q(init)+' stop',timeout=12)
   s.run('rm -f '+Q(d+'/camera-next'));upload(s,d,'camera-rollback',rollback,budget_seconds=120)
   s.run('rm -f '+Q(base+'/camera')+'\ncp '+Q(d+'/camera-rollback')+' '+Q(base+'/camera')+'\nchmod 700 '+Q(base+'/camera')+'\nrm -f '+Q(d+'/camera-rollback'),timeout=150)
   record['restored_md5']=s.run('md5sum '+Q(base+'/camera')).split()[0]
   s.run(Q(init)+' start');record['restored_from_ram']=record['restored_md5']==record['old_md5']
  except Exception as restore:record['restore_error_private']=str(restore)
 elif stopped and not switched:
  try:s.run(Q(init)+' start');record['service_restarted_unchanged']=True
  except Exception as restart:record['restart_error_private']=str(restart)
 if switched:
  try:
   s.run(Q(init)+' stop',timeout=12)
   s.run('mv '+Q(base+'/camera')+' '+Q(base+'/camera.failed')+'\nmv '+Q(base+'/camera.previous')+' '+Q(base+'/camera'))
   s.run(Q(init)+' start');record['restored_previous']=True
  except Exception as restore:record['restore_error_private']=str(restore)
 print('UPGRADE_NOT_VERIFIED; private diagnostics saved',flush=True)
finally:
 try:s.run('rm -f '+Q(d+'/camera-next')+' '+Q(d+'/camera-rollback')+'\nrmdir '+Q(d));record['stage_removed']=True
 except Exception as e:record['cleanup_error_private']=str(e)
 s.close();(HERE/'journals').mkdir(exist_ok=True);p=HERE/'journals'/('camera-upgrade-'+time.strftime('%Y%m%dT%H%M%S')+'.private.json');fd=os.open(p,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
 with os.fdopen(fd,'w') as f:json.dump(record,f,indent=2)

"""Select the retained previous app, without changing binaries or Matter state."""
import argparse, getpass, hashlib, json, os, time
from pathlib import Path
from g2h_identification_probe import Telnet
from g2h_camera_upgrade_live import CFG, INIT, Q, selected_path, checksum
from g2h_backup_live import identity, safe_status

def recover(session,backup):
 manifest=json.loads((backup/'manifest.json').read_text())
 if manifest.get('complete') is not True:raise ValueError('verified backup manifest required')
 # The original fallback remains installed; use exact private backup metadata.
 row=next(r for r in manifest['files'] if r['local_file']=='matter-camera-active') if any(r['local_file']=='matter-camera-active' for r in manifest['files']) else next(r for r in manifest['files'] if r['local_file']=='matter-camera')
 saved=backup/row['local_file']
 if saved.is_symlink() or hashlib.sha256(saved.read_bytes()).hexdigest()!=row['sha256']:raise ValueError('private backup changed')
 previous=selected_path(row['remote_path'])
 if checksum(session,previous)!=row['md5']:raise ValueError('retained app missing/changed; explicit re-upload recovery needed')
 metadata=json.loads((backup/'upgrade.json').read_text())
 previous_broker=metadata.get('previous_broker')
 if previous_broker not in ('0','1'):raise ValueError('backup lacks original broker state; no change made')
 boot=identity(session)
 # Pending rollback has its own authenticated previous path and survives reboot.
 pending=session.run('if [ -d '+CFG+'/update-pending ]; then echo yes; fi').strip()
 if pending:
  try:session.run(INIT+' rollback',timeout=30)
  except (OSError,EOFError,TimeoutError):
   if previous_broker!='0':raise
  if previous_broker=='0':return {'rebooting_to_previous':True}
 else:
  session.run(INIT+' stop',timeout=20)
  session.run('set -e\nprintf \'%s\\n\' '+Q(previous)+' > '+CFG+'/app-path.next\nmv '+CFG+'/app-path.next '+CFG+'/app-path\nsync')
  if previous_broker=='0':
   session.run('rm -f '+CFG+'/media-broker-enabled\nsync')
   try:session.run('reboot',timeout=3)
   except (OSError,EOFError,TimeoutError):pass
   return {'rebooting_to_previous':True}
  session.run(INIT+' start',timeout=12)
 for _ in range(15):
  time.sleep(2)
  try:status=safe_status(session)
  except Exception:continue
  if status['fabrics']!=manifest['status_before']['fabrics']:raise RuntimeError('fabric count differs')
  if identity(session)!=boot:raise RuntimeError('host/vendor changed during recovery')
  return status
 raise RuntimeError('previous service readiness not confirmed')

def main(argv=None):
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--host',default=os.environ.get('G2H_IP') or os.environ.get('CAMERA_IP'));p.add_argument('--backup',type=Path,required=True)
 a=p.parse_args(argv)
 if not a.host:p.error('provide --host or G2H_IP')
 pw=getpass.getpass('Camera root Telnet password: ');s=Telnet(a.host)
 try:
  s.login(pw);pw=None;status=recover(s,a.backup)
  print('PREVIOUS_APP_READY',json.dumps({k:status.get(k) for k in ('ready','fabrics','thread_role','rebooting_to_previous')}))
 finally:pw=None;s.close()
if __name__=='__main__':main()

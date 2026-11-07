"""Read-only host check. No logs, Matter secrets, UART writes or camera changes."""
import argparse,getpass,json,os
from g2h_identification_probe import Telnet
from g2h_backup_live import identity,safe_status

def main(argv=None):
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--host',default=os.environ.get('G2H_IP') or os.environ.get('CAMERA_IP'));a=p.parse_args(argv)
 if not a.host:p.error('provide --host or G2H_IP')
 pw=getpass.getpass('Camera root Telnet password: ');s=Telnet(a.host)
 try:
  s.login(pw);pw=None
  result=identity(s);result['matter']=safe_status(s)
  result['capacity']=s.run('df -k /customer /tmp /mnt/config /mnt/sdcard').strip().splitlines()
  print(json.dumps(result,indent=2))
 finally:pw=None;s.close()
if __name__=='__main__':main()

"""Place a prebuilt app.bin at /customer/app.bin and reboot. The camera's own
/etc/check_upgrade.sh then installs it at boot (vendor-native, no signature) and
reboots again. Adds only our Matter service; vendor files are not touched.

Usage: python3 g2h_place_appbin_live.py /path/to/app.bin [--reboot]
Without --reboot it only stages the file and reports; you reboot yourself.
"""
import getpass,hashlib,shlex,sys,time,os
from pathlib import Path
HERE=Path(__file__).resolve().parent;sys.path.insert(0,str(HERE))
from g2h_identification_probe import Telnet
from g2h_upload_telnet_bulk import upload

HOST=os.environ.get('G2H_IP') or os.environ.get('CAMERA_IP');q=shlex.quote
if not HOST:raise SystemExit('Set G2H_IP to the camera address; no default device is selected.')
if len(sys.argv)<2:
    print('usage: g2h_place_appbin_live.py /path/to/app.bin [--reboot]');raise SystemExit(2)
app=Path(sys.argv[1]);reboot='--reboot' in sys.argv[2:];remove='--remove' in sys.argv[1:]
if remove:
    pw=getpass.getpass('Camera password: ');s=Telnet(HOST);s.login(pw);del pw
    try:
        cur=s.run('[ -f /customer/app.bin ] && md5sum /customer/app.bin || echo NONE',timeout=30).strip()
        s.run('rm -f /customer/app.bin',timeout=30)
        print('REMOVED /customer/app.bin (was:',cur,')',flush=True)
    finally:s.close()
    raise SystemExit(0)
data=app.read_bytes()
assert app.name.endswith('.bin') and 1000<len(data)<=9*1024*1024,'app.bin size/name unexpected'
md5=hashlib.md5(data).hexdigest();need_kb=(len(data)//1024)+256

pw=getpass.getpass('Camera password: ');s=Telnet(HOST);s.login(pw);del pw
staged='/tmp/g2h_appbin_stage'
try:
    free=int(s.run("df -k /customer | tail -1 | awk '{print $4}'").strip())
    print('free /customer KiB:',free,' needed:',need_kb,flush=True)
    if free<need_kb:
        print('REFUSED: not enough free space on /customer for app.bin.')
        print('This route is for a fresh/empty camera; on a device that already runs our app, use the network install (INSTALLATIE.md).')
        raise SystemExit(1)
    s.run('umask 077\nmkdir '+q(staged),timeout=15)
    upload(s,staged,'appbin',data,budget_seconds=150)
    got=s.run('md5sum '+q(staged+'/appbin'),timeout=60).split()[0]
    assert got==md5,'uploaded copy mismatch'
    # JFFS2 write of several MB is slow; give it room.
    s.run('cp '+q(staged+'/appbin')+' /customer/app.bin\nrm -f '+q(staged+'/appbin')+'\nrmdir '+q(staged),timeout=180)
    on_device=s.run('md5sum /customer/app.bin',timeout=60).split()[0]
    assert on_device==md5,'/customer/app.bin mismatch'
    print('STAGED /customer/app.bin md5',md5,flush=True)
    if reboot:
        print('Rebooting; the camera installs app.bin at boot (~1-2 min).',flush=True)
        s.run('(sleep 1; sync; reboot) </dev/null >/dev/null 2>&1 &')
    else:
        print('Not rebooting. Reboot the camera to install (power-cycle or run reboot).',flush=True)
finally:
    s.close()

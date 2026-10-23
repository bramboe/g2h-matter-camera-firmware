"""Read-only host check: boot, uptime, vendor camera process, RTSP listener,
own Matter app status. Changes nothing on the camera."""
import getpass,json,sys,time
from pathlib import Path
sys.path[:0]=['outputs','work']
from g2h_identification_probe import Telnet

pw=getpass.getpass('Camera password: ');s=Telnet('192.168.1.50');s.login(pw);del pw
r={}
try:
    for _ in range(4): # the vendor camera may still be starting right after a boot
        r['boot']=s.run('cat /proc/sys/kernel/random/boot_id').strip()
        r['uptime_s']=s.run('cut -d" " -f1 /proc/uptime').strip()
        r['vendor_camera']=s.run("ps | grep '[c]amera -M' || :").strip()[:120]
        r['rtsp_listening']='0A' in s.run("awk 'NR>1 && $2 ~ /:022A$/ {print $4}' /proc/net/tcp || :")
        raw=s.run('/etc/init.d/S96g2hmattercamera status || :').strip()
        r['matter']=json.loads(raw) if raw.startswith('{') else raw[:80]
        if r['vendor_camera'] and r['rtsp_listening']:break
        time.sleep(15)
    m=r['matter'] if isinstance(r['matter'],dict) else {}
    print('HOST',json.dumps({k:r[k] for k in ('uptime_s','vendor_camera','rtsp_listening')}),flush=True)
    print('MATTER',json.dumps({k:m.get(k) for k in ('pid','ready','fabrics','video_enabled','source_fault','thread_role')}),flush=True)
except BaseException as e:
    r['error_private']=str(e)[:300];print('CHECK_NOT_COMPLETED',str(e)[:200],flush=True)
finally:
    s.close();jd=Path(__file__).resolve().parent/'journals';jd.mkdir(exist_ok=True);p=jd/('host-check-'+time.strftime('%Y%m%dT%H%M%S')+'.private.json')
    p.write_text(json.dumps(r,indent=2));p.chmod(0o600);print('CHECK_JOURNAL',p,flush=True)

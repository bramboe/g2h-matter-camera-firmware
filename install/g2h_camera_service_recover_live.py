"""Recover the Matter camera service to a known build and report the host state.

Reports: app MD5 on /customer, vendor camera PID, RTSP listener, MemAvailable,
/customer free space. Keeps a known-good app (d8b08cad… or 867ba1ee…) as is;
when the app is missing or unknown it installs the local 867ba1ee… build with
no RAM rollback copy (the /tmp upload is removed right after copying). Then
starts the service and confirms readiness and the 3 controller fabrics.
"""
import getpass,hashlib,json,shlex,sys,time,uuid
from pathlib import Path
sys.path[:0]=['outputs','work']
from g2h_identification_probe import Telnet
from g2h_upload_telnet_bulk import upload

Q=shlex.quote;base='/customer/g2h-matter-camera';init='/etc/init.d/S96g2hmattercamera'
KNOWN={'ec751c4fe1178afad1ed5b465df81c04':'d8b08cad…','ff55c3cf06a70bec359abac28c4ce04a':'867ba1ee…',
       'd4330b6738a5894b816de507da5bf44c':'88714880…','b1a4d4300ff5a746aa41be894acdc683':'ff4938a9…',
       '4cd75228dd345513d8e2f3f67e2163ca':'6df13874…','d336e606e9185d239f4ecb4396c6eaa2':'4d3a3c0c…','6e3a19f7704a250377b70982a57ac114':'b53484a4…'}
LOCAL=Path(__file__).resolve().parent/'builds'/'b53484a42844bd811760de1e1558249d59b22c2f740ed4bd5e47127c02b1d42d.camera' # b53484a4…: privacy + all fixes
r={}
def host_state():
    cam=s.run("ps | grep '[c]amera -M' || :").split()
    return {'vendor_camera_pid':cam[0] if cam else None,
            'rtsp_listening':'0A' in s.run("awk 'NR>1 && $2 ~ /:022A$/ {print $4}' /proc/net/tcp || :"),
            'mem_available_kb':s.run("awk '/MemAvailable:/{print $2}' /proc/meminfo").strip(),
            'customer_df':s.run('df -k /customer | tail -1').strip()}
pw=getpass.getpass('Camera password: ');s=Telnet('192.168.1.50');s.login(pw);del pw
try:
    r['boot']=s.run('cat /proc/sys/kernel/random/boot_id').strip();r['before']=host_state()
    md5=s.run('[ -f '+Q(base+'/camera')+' ] && md5sum '+Q(base+'/camera')+' || :').split()
    r['app_md5']=md5[0] if md5 else None;r['app_known']=KNOWN.get(r['app_md5'])
    print('STATE',json.dumps(r['before']),'app',r['app_md5'],r['app_known'],flush=True)
    s.run(Q(init)+' stop || :',timeout=12)
    s.run('rm -f '+Q(base+'/camera.next')+' '+Q(base+'/camera.failed'))
    if r['app_known'] not in ('b53484a4…',):
        image=LOCAL.read_bytes();want=hashlib.md5(image).hexdigest()
        d='/tmp/g2h_recover_'+uuid.uuid4().hex;s.run('umask 077\nmkdir '+Q(d))
        upload(s,d,'camera',image,budget_seconds=120)
        s.run('rm -f '+Q(base+'/camera')+'\ncp '+Q(d+'/camera')+' '+Q(base+'/camera')+'\nchmod 700 '+Q(base+'/camera')+'\nrm -f '+Q(d+'/camera')+'\nrmdir '+Q(d),timeout=150)
        got=s.run('md5sum '+Q(base+'/camera')).split()[0];assert got==want,'reinstalled copy mismatch'
        r['reinstalled']=KNOWN[got];print('REINSTALLED',r['reinstalled'],flush=True)
    s.run(Q(init)+' start || :')
    for _ in range(20):
        raw=s.run(Q(init)+' status || :').strip()
        if raw.startswith('{'):
            st=json.loads(raw)
            if st.get('ready') and 'last_video_stage' in st:r['status']=st;break
        time.sleep(1)
    else:raise RuntimeError('service readiness not confirmed')
    r['after']=host_state();r['verified']=r['status']['fabrics']==3
    print('MATTER_SERVICE_RECOVERED' if r['verified'] else 'SERVICE_UP_BUT_FABRICS_DIFFER',json.dumps({k:r['status'].get(k) for k in ('pid','fabrics','thread_role','audio_stage')}),'host',json.dumps(r['after']),flush=True)
except BaseException as e:
    r['error_private']=str(e)[:300];print('RECOVERY_NOT_VERIFIED',str(e)[:200],flush=True)
finally:
    s.close();jd=Path(__file__).resolve().parent/'journals';jd.mkdir(exist_ok=True);p=jd/('service-recover-'+time.strftime('%Y%m%dT%H%M%S')+'.private.json')
    p.write_text(json.dumps(r,indent=2));p.chmod(0o600);print('RECOVERY_JOURNAL',p,flush=True)

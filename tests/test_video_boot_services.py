"""Execute relocated production scripts; no camera, reboot or host sync."""
import hashlib,os,subprocess,tempfile,time,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class VideoBootServices(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(prefix='g2h-video-services-',dir='/tmp');self.addCleanup(self.temp.cleanup)
  self.root=Path(self.temp.name);self.cfg=self.root/'config';self.base=self.root/'base';self.cfg.mkdir();self.base.mkdir()
  self.native=self.root/'native';self.launched=self.root/'launched';self.rebooted=self.root/'rebooted';self.pending=self.root/'pending'
  self.native.write_text('#!/bin/sh\nprintf "%s" "${G2H_VIDEO_BOOT_BLOCKED-unset}" > "'+str(self.launched)+'"\n');self.native.chmod(0o700)
  self.preload=self.base/'video-boot-gate.so';self.preload.write_bytes(b'own synthetic fixture')
  self.write('video-profile',hashlib.md5(self.native.read_bytes()).hexdigest()+' '+hashlib.md5(self.preload.read_bytes()).hexdigest()+'\n')
  commands=self.root/'commands';commands.mkdir()
  stat=commands/'stat';stat.write_text('#!'+os.sys.executable+'\nimport os,sys\ns=os.stat(sys.argv[3]);print({"%u":s.st_uid,"%a":format(s.st_mode&0o777,"o"),"%s":s.st_size}[sys.argv[2]])\n');stat.chmod(0o700)
  md5=commands/'md5sum';md5.write_text('#!'+os.sys.executable+'\nimport hashlib,sys\nprint(hashlib.md5(open(sys.argv[1],"rb").read()).hexdigest()+"  "+sys.argv[1])\n');md5.chmod(0o700)
  sync=commands/'sync';sync.write_text('#!/bin/sh\nexit 0\n');sync.chmod(0o700)
  reboot=self.root/'reboot';reboot.write_text('#!/bin/sh\n: > "'+str(self.rebooted)+'"\n');reboot.chmod(0o700)
  self.env=dict(os.environ,PATH=str(commands)+os.pathsep+os.environ['PATH'])
  for name in ('g2h-native-camera','g2h-video-control'):
   source=(ROOT/'install/services'/name).read_text().replace('/customer/g2h-matter-camera',str(self.base)).replace('/mnt/config/g2h-matter-camera',str(self.cfg)).replace('/tmp/out/camera',str(self.native)).replace('/tmp/g2h-video-control-request',str(self.pending)).replace('/sbin/reboot',str(reboot)).replace('sleep 3','sleep .05').replace(')" = 0 ]',')" = '+str(os.geteuid())+' ]')
   (self.root/name).write_text(source)
 def write(self,name,data):
  p=self.cfg/name;p.write_text(data);p.chmod(0o600)
 def run_service(self,name,*args):return subprocess.run(['/bin/sh',str(self.root/name),*args],env=self.env,capture_output=True,timeout=5)
 def test_default_start_is_normal_and_verified_off_loads_filter(self):
  self.assertEqual(self.run_service('g2h-native-camera').returncode,0);self.assertEqual(self.launched.read_text(),'unset')
  self.write('video-enabled','0\n')
  self.assertEqual(self.run_service('g2h-native-camera').returncode,0);self.assertEqual(self.launched.read_text(),'1')
 def test_wrong_native_profile_fails_closed_without_launch(self):
  self.write('video-enabled','0\n');self.native.write_text('#!/bin/sh\nexit 0\n')
  self.assertEqual(self.run_service('g2h-native-camera').returncode,125);self.assertFalse(self.launched.exists())
 def test_untrusted_or_malformed_preference_never_launches_native(self):
  for value in ('invalid\n','0\n1\n'):
   self.write('video-enabled',value);self.assertEqual(self.run_service('g2h-native-camera').returncode,125)
  (self.cfg/'video-enabled').unlink();(self.cfg/'video-enabled').symlink_to(self.native)
  self.assertEqual(self.run_service('g2h-native-camera').returncode,125);self.assertFalse(self.launched.exists())
 def test_request_commits_preference_then_queues_one_reboot(self):
  result=self.run_service('g2h-video-control','set','0');self.assertEqual(result.returncode,0,result.stderr)
  self.assertEqual((self.cfg/'video-enabled').read_text(),'0\n')
  until=time.monotonic()+2
  while not self.rebooted.exists() and time.monotonic()<until:time.sleep(.01)
  self.assertTrue(self.rebooted.exists())
  self.assertNotEqual(self.run_service('g2h-video-control','set','1').returncode,0)
  self.assertEqual((self.cfg/'video-enabled').read_text(),'0\n')
 def test_rejected_request_never_changes_preference_or_queues_reboot(self):
  self.write('video-enabled','1\n');self.write('video-profile','invalid\n')
  self.assertNotEqual(self.run_service('g2h-video-control','set','0').returncode,0)
  self.assertEqual((self.cfg/'video-enabled').read_text(),'1\n');self.assertFalse(self.pending.exists());self.assertFalse(self.rebooted.exists())

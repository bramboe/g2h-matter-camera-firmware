"""Real Unix subscriptions over a synthetic RTSP server; never contacts a camera."""
import json,os,socket,struct,subprocess,tempfile,threading,time,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SDP='''v=0\r
m=video 0 RTP/AVP 36\r
a=rtpmap:36 H264/90000\r
a=fmtp:36 sprop-parameter-sets=Z0JAH5dAKALdNgI=,aM48gA==\r
m=audio 0 RTP/AVP 104\r
a=control:trackID=1\r
a=rtpmap:104 MPEG4-GENERIC/16000/1\r
a=fmtp:104 mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1408\r
'''.encode()
def exact(s,n):
 b=b''
 while len(b)<n:
  c=s.recv(n-len(b))
  if not c:raise EOFError()
  b+=c
 return b
class Native:
 def __init__(self,listener=None):
  self.listener=listener if listener is not None else socket.socket()
  if listener is None:self.listener.bind(('127.0.0.1',0))
  self.listener.listen();self.listener.settimeout(.1)
  self.port=self.listener.getsockname()[1];self.stop=False;self.partial_record=False;self.partial_pause=False;self.partial_header_entered=threading.Event();self.partial_resume=threading.Event();self.bad_sdp=False;self.describes=0;self.play_gate=None;self.play_entered=threading.Event();self.clients=[];self.played=0;self.teardowns=0;self.threads=[]
  self.thread=threading.Thread(target=self.accept,daemon=True);self.thread.start()
 def accept(self):
  while not self.stop:
   try:c,_=self.listener.accept()
   except (TimeoutError,OSError):continue
   self.clients.append(c);t=threading.Thread(target=self.client,args=(c,),daemon=True);self.threads.append(t);t.start()
 def client(self,c):
  try:
   c.settimeout(.1);b=b'';audio=False
   while not self.stop:
    try:b+=c.recv(4096)
    except TimeoutError:continue
    if not b:return
    if b'\r\n\r\n' not in b:continue
    request,b=b.split(b'\r\n\r\n',1);method=request.split()[0]
    body=(b'invalid SDP' if self.bad_sdp else SDP) if method==b'DESCRIBE' else b''
    if method==b'DESCRIBE':self.describes+=1
    if method==b'SETUP' and b'/trackID=1' in request:audio=True
    if method==b'PLAY':
     self.play_entered.set()
     if self.play_gate:self.play_gate.wait(3)
    c.sendall(b'RTSP/1.0 200 OK\r\nSession: test\r\nTransport: RTP/AVP/TCP;interleaved=2-3\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)
    if method==b'PLAY':break
   self.played+=1;c.settimeout(.01)
   if self.partial_record:
    c.sendall(b'$\x00'+struct.pack('>H',16000)+b'ABC');return
   if self.partial_pause:
    c.sendall(b'$\x00');self.partial_header_entered.set();self.partial_resume.wait(5)
    c.sendall(struct.pack('>H',16000)+b'\x80\x24'+b'P'*15998)
   record=b'$\x00'+struct.pack('>H',16000)+b'\x80\x24'+b'X'*15998
   tick=0
   while not self.stop:
    c.sendall(record)
    if audio and tick%6==0:c.sendall(b'$\x02'+struct.pack('>H',1024)+b'\x80\x68'+b'A'*1022)
    tick+=1
    try:
     data=c.recv(4096)
     if not data:return
     if b'TEARDOWN' in data:self.teardowns+=1;return
    except TimeoutError:pass
  except OSError:pass
  finally:c.close()
 def close(self):
  self.stop=True;self.partial_resume.set();self.listener.close()
  for c in self.clients:
   try:c.shutdown(socket.SHUT_RDWR)
   except OSError:pass
  self.thread.join(1)
  for t in self.threads:t.join(1)
class BrokerTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.build=tempfile.TemporaryDirectory();cls.binary=Path(cls.build.name)/'broker'
  subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-DG2H_BROKER_TESTING',str(ROOT/'app/src/g2h_media_broker.cpp'),'-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls):cls.build.cleanup()
 def setUp(self):
  self.native=Native();self.directory=tempfile.TemporaryDirectory();self.path=Path(self.directory.name)/'media'
  self.capture=Path(self.directory.name)/'capture';self.capture.mkdir(mode=0o700)
  env=dict(os.environ,G2H_TEST_RTSP_PORT=str(self.native.port),G2H_TEST_CAPTURE_DIR=str(self.capture))
  self.p=subprocess.Popen([str(self.binary),'--serve',str(self.path)],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  deadline=time.monotonic()+3
  while not all((self.path/name).exists() for name in ('video.sock','audio.sock')) and time.monotonic()<deadline:time.sleep(.01)
 def tearDown(self):
  self.p.terminate();self.p.communicate(timeout=5);self.native.close();self.directory.cleanup()
 def subscribe(self,audio=False):
  c=socket.socket(socket.AF_UNIX);c.settimeout(4)
  try:
   c.connect(str(self.path/('audio.sock' if audio else 'video.sock')))
   h=exact(c,16);self.assertEqual(h[:8],b'G2HMED01');self.assertEqual(h[8],int(audio));n,p=struct.unpack('>HH',h[12:]);self.assertLessEqual(n,2048);self.assertLessEqual(p,2048);exact(c,n+p);return c
  except BaseException:c.close();raise
 def record(self,c):
  h=exact(c,4);self.assertEqual(h[0],36);return exact(c,int.from_bytes(h[2:],'big'))
 def status(self,predicate):
  deadline=time.monotonic()+3
  while time.monotonic()<deadline:
   try:value=json.loads((self.path/'status.json').read_text())
   except (FileNotFoundError,ValueError):time.sleep(.02);continue
   if predicate(value):return value
   time.sleep(.02)
  self.fail('broker did not publish the expected diagnostic state')
 def test_diagnostics_observe_native_progress_and_transport_fault(self):
  with self.subscribe() as c:
   self.record(c)
   status=self.status(lambda value:value['video']['records']>0)
   self.assertFalse(status['video']['fault'])
   self.assertTrue(status['video']['native_ready'])
   self.assertEqual(status['pid'],self.p.pid)
   self.assertEqual((self.path/'status.json').stat().st_mode&0o777,0o600)
   self.native.close()
   fault=self.status(lambda value:value['video']['fault'])
   self.assertGreater(fault['monotonic_ms'],status['monotonic_ms'])
   self.assertEqual(set(fault['video']),{'fault','stage','native_ready','records','discarded_video_records'})
   self.assertIsNone(self.p.poll())
 def test_restarts_preserve_one_native_session(self):
  for _ in range(6):
   with self.subscribe() as c:self.assertEqual(len(self.record(c)),16000)
   time.sleep(.05)
  self.assertEqual(self.native.played,1);self.assertEqual(self.native.teardowns,0)
 def test_abandoned_handshake_does_not_poison_native_session(self):
  self.native.play_gate=threading.Event()
  abandoned=socket.socket(socket.AF_UNIX);abandoned.connect(str(self.path/'video.sock'))
  self.assertTrue(self.native.play_entered.wait(2))
  abandoned.close();self.native.play_gate.set();time.sleep(.1)
  with self.subscribe() as c:self.assertEqual(len(self.record(c)),16000)
  self.assertEqual(self.native.played,1);self.assertEqual(self.native.teardowns,0)
 def test_initial_connect_failure_recovers_when_native_server_ready(self):
  self.p.terminate();self.p.communicate(timeout=5);self.native.close()
  reserved=socket.socket();self.addCleanup(reserved.close);reserved.bind(('127.0.0.1',0));port=reserved.getsockname()[1]
  env=dict(os.environ,G2H_TEST_RTSP_PORT=str(port))
  self.p=subprocess.Popen([str(self.binary),'--serve',str(self.path)],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  deadline=time.monotonic()+3
  while not (self.path/'video.sock').exists() and time.monotonic()<deadline:time.sleep(.01)
  with socket.socket(socket.AF_UNIX) as c:
   c.settimeout(5);c.connect(str(self.path/'video.sock'));self.assertEqual(c.recv(16),b'')
  self.native=Native(listener=reserved)
  # Do not reconnect native TCP repeatedly while its startup is still settling.
  with socket.socket(socket.AF_UNIX) as c:
   c.settimeout(2);c.connect(str(self.path/'video.sock'));self.assertEqual(c.recv(16),b'')
  self.assertEqual(self.native.describes,0)
  time.sleep(5.05)
  for _ in range(2):
   with self.subscribe() as c:self.assertEqual(len(self.record(c)),16000)
   time.sleep(.05)
  self.assertEqual(self.native.played,1);self.assertEqual(self.native.teardowns,0)
 def test_rtsp_handshake_failure_stays_latched(self):
  self.native.bad_sdp=True
  with socket.socket(socket.AF_UNIX) as c:
   c.settimeout(2);c.connect(str(self.path/'video.sock'));self.assertEqual(c.recv(16),b'')
  self.native.bad_sdp=False
  with socket.socket(socket.AF_UNIX) as c:
   c.settimeout(2);c.connect(str(self.path/'video.sock'));self.assertEqual(c.recv(16),b'')
  self.assertEqual(self.native.describes,1);self.assertEqual(self.native.played,0)
 def test_native_partial_record_fault_rejects_reconnect_promptly(self):
  self.native.partial_record=True
  with self.subscribe() as c:
   with self.assertRaises(EOFError):self.record(c)
  time.sleep(.05)
  c=socket.socket(socket.AF_UNIX);c.settimeout(.5)
  with c:
   c.connect(str(self.path/'video.sock'));self.assertEqual(c.recv(16),b'')
  self.assertEqual(self.native.played,1)
 def test_slow_eviction_reconnect_starts_on_complete_record(self):
  with self.subscribe() as slow:
   time.sleep(1.2)
   # The old connection may end in a partial record. Its successor must start
   # with a fresh handshake and each entire original RTP payload, never a suffix.
   with self.subscribe() as new:
    for _ in range(12):self.assertEqual(self.record(new),b'\x80\x24'+b'X'*15998)
  self.assertEqual(self.native.played,1)
 def test_audio_independent_and_slow_subscriber_evicted(self):
  with self.subscribe() as slow:
   time.sleep(1.2)
   with self.subscribe() as new:self.assertEqual(len(self.record(new)),16000)
  with self.subscribe(True) as audio:self.assertEqual(self.record(audio),b'\x80\x68'+b'A'*1022)
  self.assertEqual(self.native.played,2);self.assertEqual(self.native.teardowns,0)
 def test_audio_discards_video_bursts_without_dropping_slow_decoder(self):
  with self.subscribe(True) as audio:
   # Vendor mixes video into the audio-only RTSP session. A decoder startup
   # pause must not let those irrelevant keyframes fill its local socket.
   time.sleep(.5)
   for index in range(20):
    try:payload=self.record(audio)
    except EOFError:
     self.p.terminate();_,errors=self.p.communicate(timeout=5)
     self.fail(f'Audio ended after {index} records: {errors.decode()}')
    self.assertEqual(payload,b'\x80\x68'+b'A'*1022)
  self.assertEqual(self.native.played,1);self.assertEqual(self.native.teardowns,0)
 def test_explicit_capture_pause_preserves_partial_native_record(self):
  self.native.partial_pause=True
  with self.subscribe() as c:
   self.assertTrue(self.native.partial_header_entered.wait(2))
   marker=self.capture/'paused';marker.touch(mode=0o600)
   time.sleep(2.4) # Longer than the normal per-record deadline.
   marker.unlink();self.native.partial_resume.set()
   self.assertEqual(self.record(c),b'\x80\x24'+b'X'*15998)
  self.assertEqual(self.native.played,1);self.assertEqual(self.native.teardowns,0)
 def test_paused_capture_never_creates_a_native_session(self):
  marker=self.capture/'paused';marker.touch(mode=0o600)
  with socket.socket(socket.AF_UNIX) as c:
   c.settimeout(1);c.connect(str(self.path/'video.sock'));self.assertEqual(c.recv(16),b'')
  self.assertEqual(self.native.describes,0)
  marker.unlink()
  with self.subscribe() as c:self.assertEqual(len(self.record(c)),16000)
 def test_existing_audio_is_independent_of_video_capture_pause(self):
  with self.subscribe(True) as c:
   self.assertEqual(self.record(c),b'\x80\x68'+b'A'*1022)
   marker=self.capture/'paused';marker.touch(mode=0o600)
   for _ in range(8):self.assertEqual(self.record(c),b'\x80\x68'+b'A'*1022)
   marker.unlink()
  self.assertEqual(self.native.played,1);self.assertEqual(self.native.teardowns,0)
 def test_untrusted_capture_marker_fails_closed(self):
  marker=self.capture/'paused';marker.touch(mode=0o644);marker.chmod(0o644)
  with socket.socket(socket.AF_UNIX) as c:
   c.settimeout(1);c.connect(str(self.path/'video.sock'));self.assertEqual(c.recv(16),b'')
  self.assertEqual(self.native.describes,0);self.assertIsNone(self.p.poll())
 def test_socket_permissions_and_single_instance(self):
  self.assertEqual((self.path/'video.sock').stat().st_mode&0o777,0o600)
  p=subprocess.run([str(self.binary),'--serve',str(self.path)],capture_output=True,timeout=3)
  self.assertNotEqual(p.returncode,0)
  with self.subscribe() as c:self.record(c)
 def test_public_directory_refused(self):
  bad=Path(self.directory.name)/'public';bad.mkdir(mode=0o777);bad.chmod(0o777)
  p=subprocess.run([str(self.binary),'--serve',str(bad)],capture_output=True,timeout=3)
  self.assertNotEqual(p.returncode,0);self.assertFalse((bad/'video.sock').exists())
if __name__=='__main__':unittest.main()

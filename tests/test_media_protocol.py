"""Untrusted/fragmented local metadata and partial-record tests; no camera access."""
import os,socket,struct,subprocess,tempfile,threading,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def metadata(audio=False,payload=36,channel=0,sps=b'\x67\x42\x40\x1f',pps=b'\x68\x01'):
 return b'G2HMED01'+bytes([int(audio),payload,channel,0])+struct.pack('>HH',len(sps),len(pps))+sps+pps
class ProtocolTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.build=tempfile.TemporaryDirectory();cls.binary=Path(cls.build.name)/'input'
  subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'app/src'),str(ROOT/'tests/media_input_probe.cpp'),'-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls):cls.build.cleanup()
 def probe(self,messages,records=0,audio=False,fragment=False):
  with tempfile.TemporaryDirectory() as td:
   directory=Path(td)/'media';directory.mkdir(mode=0o700);server=socket.socket(socket.AF_UNIX);path=directory/('audio.sock' if audio else 'video.sock');server.bind(str(path));path.chmod(0o600);server.listen(2);server.settimeout(5)
   errors=[]
   def serve():
    try:
     for message in messages:
      with server.accept()[0] as c:
       try:
        if fragment:
         for byte in message:c.sendall(bytes([byte]))
        else:c.sendall(message)
       except (BrokenPipeError,ConnectionResetError):pass
    except Exception as error:errors.append(error)
   worker=threading.Thread(target=serve);worker.start()
   result=subprocess.run([str(self.binary),str(directory),str(int(audio)),str(len(messages)),str(records)],capture_output=True,text=True,timeout=12)
   worker.join(6);server.close();self.assertFalse(worker.is_alive());self.assertEqual(errors,[]);return result
 def test_fragmented_valid_metadata_and_record(self):
  record=b'$\x00\x00\x10'+b'A'*16
  result=self.probe([metadata()+record],records=1,fragment=True)
  self.assertEqual(result.returncode,0,result.stderr);self.assertIn('RECORD 16 65 65',result.stdout)
 def test_malformed_metadata_is_rejected(self):
  good=metadata()
  malformed=[b'BADMAGIC'+good[8:],good[:11]+b'\x01'+good[12:],metadata(payload=127),metadata(channel=1),metadata(sps=b'\x65\x42\x40\x1f'),metadata(pps=b'\x67\x01'),good[:12]+b'\x08\x01\x00\x01',good[:13],metadata(audio=True)]
  for message in malformed:
   with self.subTest(message=message[:16]):
    result=self.probe([message]);self.assertEqual(result.returncode,1,result.stdout);self.assertNotIn('READY',result.stdout)
 def test_audio_metadata_is_validated(self):
  result=self.probe([metadata(True,104,2,b'',b'')],audio=True);self.assertEqual(result.returncode,0,result.stdout)
  for message in [metadata(True,35,2,b'',b''),metadata(True,104,255,b'',b''),metadata(True,104,2,b'\x67',b'')]:
   self.assertEqual(self.probe([message],audio=True).returncode,1)
 def test_partial_record_disconnect_then_same_input_reconnect(self):
  partial=metadata()+b'$\x00\x00\x10'+b'ABC'
  complete=metadata()+b'$\x00\x00\x10'+b'Z'*16
  result=self.probe([partial,complete],records=1,fragment=True)
  self.assertEqual(result.returncode,1,result.stdout);self.assertEqual(result.stdout.count('ERROR'),1);self.assertEqual(result.stdout.count('RECORD'),1);self.assertIn('RECORD 16 90 90',result.stdout)
 def test_failed_handshake_then_same_input_reconnect(self):
  result=self.probe([b'BADMAGIC'+metadata()[8:],metadata()]);self.assertEqual(result.returncode,1,result.stdout);self.assertEqual(result.stdout.count('READY'),1)
if __name__=='__main__':unittest.main()

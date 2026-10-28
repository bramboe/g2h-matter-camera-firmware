"""Execute the actual generated decoder pipeline on synthetic bytes, no camera."""
import base64,hashlib,re,shlex,subprocess,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'install'))
from g2h_upload_telnet_bulk import upload
class LocalReceiver:
    def __init__(self):self.initial=None;self.parts=[];self.decoded=None
    def run(self,command,timeout=8):
        if command=='stty -g':return 'cafe:1234'
        self.assert_postcondition=command
        return ''
    def send(self,value):
        if self.initial is None:
            self.initial=shlex.split(value.strip())[2]
            self.ready=re.search(r'G2H_UPLOAD_READY_[0-9a-f]+',self.initial)[0]
            self.done=re.search(r'G2H_UPLOAD_DONE_[0-9a-f]+',self.initial)[0]
            return
        self.parts.append(value)
        if value.startswith('#G2H_E_'):
            pipeline=re.search(r'; (awk -v .*?\| base64 -d > .*?); rc=',self.initial)[1]
            process=subprocess.run(['/bin/sh','-c',pipeline],input=''.join(self.parts).encode(),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            if process.returncode:raise AssertionError(process.stderr.decode())
    def until(self,pattern,timeout):
        if 'G2H_A_' in pattern:
            value=re.search(r'G2H_A_[0-9a-f]+_[0-9]+',pattern)[0]+'\n'
        else:value=(self.ready+'\n') if 'READY_' in pattern else self.done+' 0\n' 
        match=re.search(pattern,value)
        if not match:raise AssertionError(pattern)
        return value,match
    def close(self):pass
class UploadTests(unittest.TestCase):
    def test_generated_filter_preserves_all_bytes_and_restores_terminal(self):
        # This would have caught quoted-awk syntax errors before a device trial.
        with tempfile.TemporaryDirectory(prefix='g2h_upload_fixture_',dir='/tmp') as directory:
            receiver=LocalReceiver();image=bytes(range(256))*2048
            result=upload(receiver,directory,'probe',image,budget_seconds=30)
            self.assertEqual((Path(directory)/'probe').read_bytes(),image)
            self.assertTrue(result['own_telnet_terminal_restored'])
            self.assertIn('stty -icanon min 1 time 0 -echo',receiver.initial)
            self.assertIn('stty cafe:1234',receiver.initial)
            self.assertTrue(all(line.startswith('#') for block in receiver.parts for line in block.splitlines()))
            self.assertIn(hashlib.md5(image).hexdigest(),receiver.assert_postcondition)
    def test_refuses_unknown_destination_and_unbounded_time(self):
        for path,budget in (('/etc',30),('/tmp/g2h_fixture',601),('/tmp/g2h_fixture',0)):
            with self.subTest(path=path,budget=budget),self.assertRaises(ValueError):upload(LocalReceiver(),path,'probe',b'x',budget)
if __name__=='__main__':unittest.main()

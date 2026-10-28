from pathlib import Path
import shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which('c++'),'C++ compiler required')
class VideoControl(unittest.TestCase):
 def test_observations_never_echo_a_request_or_accept_untrusted_state(self):
  with tempfile.TemporaryDirectory(dir='/tmp',prefix='g2h-video-control-') as td:
   binary=Path(td)/'probe'
   subprocess.run(['c++','-std=c++17','-pthread','-Wall','-Wextra','-Werror','-UNDEBUG','-fsanitize=address,undefined',str(ROOT/'tests/test_video_control.cpp'),'-o',str(binary)],check=True,capture_output=True)
   result=subprocess.run([str(binary),td],capture_output=True,text=True,check=True,timeout=8)
   self.assertIn('tests passed',result.stdout)

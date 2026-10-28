from pathlib import Path
import shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which('cc'),'C compiler required')
class BootTasks(unittest.TestCase):
 def test_only_exact_video_tasks_are_suppressed_and_other_arguments_retained(self):
  with tempfile.TemporaryDirectory(dir='/tmp',prefix='g2h-boot-task-') as td:
   binary=Path(td)/'probe'
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-UNDEBUG','-fsanitize=address,undefined',str(ROOT/'tests/test_video_boot_tasks.c'),'-o',str(binary)],check=True,capture_output=True)
   result=subprocess.run([str(binary)],capture_output=True,text=True,check=True,timeout=5)
   self.assertIn('tests passed',result.stdout)

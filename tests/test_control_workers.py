"""Exercise actual worker I/O against local serial and native-setting peers."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('c++') and sys.platform in ('darwin','linux'),
                     'A C++17 compiler and POSIX sockets/PTYs are required')
class ControlWorkers(unittest.TestCase):
    def run_worker(self,source):
        with tempfile.TemporaryDirectory(prefix='g2h-control-workers-') as directory:
            exe=Path(directory)/'test-worker'
            subprocess.run(['c++','-std=c++17','-pthread','-UNDEBUG','-I',str(ROOT/'app/src'),
                str(ROOT/'tests'/source),'-o',str(exe)],check=True,capture_output=True,timeout=60)
            result=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=45)
            self.assertEqual(result.returncode,0)

    def test_native_audio_mutes_require_matching_readbacks(self):
        self.run_worker('test_vendor_control.cpp')

    def test_thread_cli_transitions_and_uart_ownership(self):
        self.run_worker('test_thread_control.cpp')

if __name__=='__main__':unittest.main()

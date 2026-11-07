"""Opt-in capture-control prototype with a fake native API and local Unix I/O."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('cc') and sys.platform in ('darwin','linux'),
                     'A C11 compiler and POSIX Unix sockets are required')
class CapturePreload(unittest.TestCase):
    def test_pending_driver_call_and_concurrent_policy_requests(self):
        with tempfile.TemporaryDirectory(prefix='g2h-capture-',dir='/tmp') as directory:
            path=Path(directory);private=path/'private';private.mkdir(mode=0o700)
            binary=path/'probe'
            build=['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread',
                   '-fsanitize=address,undefined','-UNDEBUG',str(ROOT/'tests/test_capture_preload.c'),'-o',str(binary)]
            if sys.platform=='linux':build.append('-ldl')
            compiled=subprocess.run(build,capture_output=True,text=True,timeout=60)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            result=subprocess.run([str(binary),str(private)],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('capture preload tests passed',result.stdout)

if __name__=='__main__':unittest.main()

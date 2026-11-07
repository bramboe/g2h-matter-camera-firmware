"""Real own-source/cache race tests, using synthetic broker records only."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('c++'), 'A C++17 compiler is required')
class VideoPrivacy(unittest.TestCase):
    def run_case(self,source,with_broker=False):
        with tempfile.TemporaryDirectory(prefix='g2h-video-privacy-',dir='/tmp') as directory:
            path=Path(directory)
            binary=path/'probe'
            subprocess.run(['c++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-UNDEBUG','-fsanitize=address,undefined',str(ROOT/'tests'/source),
                '-o',str(binary)],check=True,capture_output=True,timeout=60)
            args=[str(binary)]
            if with_broker:
                broker=path/'media'
                broker.mkdir(mode=0o700)
                args.append(str(broker))
            result=subprocess.run(args,check=False,capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('tests passed',result.stdout)

    def test_snapshot_invalidation_rejects_inflight_encode(self):
        self.run_case('test_snapshot_worker.cpp')

    def test_source_invalidation_rejects_inflight_and_fragmented_au(self):
        self.run_case('test_video_source_privacy.cpp',True)

if __name__=='__main__':unittest.main()

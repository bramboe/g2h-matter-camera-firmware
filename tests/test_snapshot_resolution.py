"""Optional real-codec integration test; only generated synthetic media is used.
Run G2H_HOST_MEDIA_PREFIX=/path/to/host/prefix python3 tests/test_snapshot_resolution.py
Requires clang++, ffmpeg (libx264), ffprobe and the host minimal FFmpeg libraries.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).parents[1]
class SnapshotResolution(unittest.TestCase):
    def test_real_codec_and_decoder(self):
        prefix=os.environ.get('G2H_HOST_MEDIA_PREFIX')
        if not prefix:self.skipTest('Set G2H_HOST_MEDIA_PREFIX to built host codec prefix')
        tools={name:shutil.which(name) for name in ('clang++','ffmpeg','ffprobe')}
        if not all(tools.values()):self.skipTest('clang++, ffmpeg and ffprobe required')
        def run(args):return subprocess.run([str(x) for x in args],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        with tempfile.TemporaryDirectory(prefix='g2h-snapshot-resolution-') as directory:
            d=Path(directory);frame=d/'fixture.h264';noise=d/'noise.h264';binary=d/'test-snapshot-codec'
            for path,source,qp in ((frame,'testsrc2=size=1280x720:rate=1','23'),
                    (noise,'nullsrc=s=1280x720,geq=random(1)*255:128:128','25')):
                run([tools['ffmpeg'],'-v','error','-f','lavfi','-i',source,'-frames:v','1','-c:v','libx264','-qp',qp,
                     '-profile:v','baseline','-pix_fmt','yuv420p','-preset','ultrafast','-tune','zerolatency',
                     '-x264-params','slices=1','-f','h264',path])
                self.assertLessEqual(path.stat().st_size,1024*1024)
            prefix=Path(prefix)
            run([tools['clang++'],'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                 ROOT/'tests/test_snapshot_codec.cpp','-I',prefix/'include','-L',prefix/'lib',
                 '-lavcodec','-lswscale','-lavutil','-lm','-pthread','-o',binary])
            result=run([binary,frame,d,noise]);self.assertIn(b'SNAPSHOT_CODEC_OK',result.stdout)
            images=list(d.glob('snapshot-*.jpg'));self.assertEqual(len(images),6)
            for image in images:
                width=int(image.name.split('-')[1]);self.assertLessEqual(image.stat().st_size,64000)
                info=json.loads(run([tools['ffprobe'],'-v','error','-show_entries','stream=width,height','-of','json',image]).stdout)['streams'][0]
                self.assertEqual((info['width'],info['height']),(width,width*9//16))
                run([tools['ffmpeg'],'-v','error','-i',image,'-frames:v','1','-f','null','-'])
if __name__=='__main__':unittest.main()

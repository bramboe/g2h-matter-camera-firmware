import hashlib,importlib.util,io,json,tarfile,tempfile,unittest
from pathlib import Path
R=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('recipe',R/'tools/build.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class InputsTests(unittest.TestCase):
 def test_locked_patch(self):
  lock=json.loads((R/'build/inputs.lock.json').read_text())
  self.assertEqual(m.sha(R/lock['matter']['patch']),lock['matter']['patch_sha256'])
 def test_archive_hash_rejects_change(self):
  with tempfile.TemporaryDirectory() as t:
   p=Path(t)/'a';p.write_bytes(b'first');item={'sha256':m.sha(p)};m.verified_archive(p,item);p.write_bytes(b'second')
   with self.assertRaises(ValueError):m.verified_archive(p,item)
 def test_extract_rejects_escape_and_links(self):
  with tempfile.TemporaryDirectory() as t:
   for name,kind in [('../outside',tarfile.REGTYPE),('link',tarfile.SYMTYPE)]:
    p=Path(t)/'a.tar'
    with tarfile.open(p,'w') as f:
     e=tarfile.TarInfo(name);e.type=kind;e.linkname='/etc/passwd';f.addfile(e,io.BytesIO(b''))
    with self.assertRaises(ValueError):m.extract(p,Path(t)/'dest')
if __name__=='__main__':unittest.main()

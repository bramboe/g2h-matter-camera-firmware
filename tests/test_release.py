import importlib.util,json,tempfile,unittest
from pathlib import Path
R=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('release',R/'tools/release.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class ReleaseTests(unittest.TestCase):
 def test_paths(self):
  for x in ['../secret','/camera.bin','a/../../x','a\\b','']:
   with self.assertRaises(ValueError):m.safe_relative(x)
 def test_valid_and_tampering(self):
  with tempfile.TemporaryDirectory() as tmp:
   base=Path(tmp);names=['g2h-matter-camera','relinkable.tar.gz','corresponding-sources.tar.gz']
   for x in names:(base/x).write_bytes(b'test')
   record={'format':'g2h-source-build-v1','vendor_firmware_included':False,'binary_matches_source':True,'hardware_qualified':False,'inputs':{},'test_evidence':{},'artifacts':[{'path':x,'bytes':4,'sha256':m.digest(base/x)} for x in names]}
   m.validate_manifest(record,base)
   (base/names[0]).write_bytes(b'changed')
   with self.assertRaises(ValueError):m.validate_manifest(record,base)
 def test_fail_closed_provenance(self):
  for flag in ['vendor_firmware_included','binary_matches_source']:
   with self.assertRaises(ValueError):m.validate_manifest({'format':'g2h-source-build-v1',flag:True},Path('.'))

class SourceStateTests(unittest.TestCase):
 def check(self,diff=None,rtc_dirty='',nested_rc=0):
  from unittest import mock
  import subprocess
  root=Path('/synthetic/sdk');rtc=root/'third_party/libdatachannel/repo'
  patch=(m.ROOT/m.BUILD.LOCK['matter']['patch']).read_bytes()
  responses=[patch if diff is None else diff,rtc_dirty]
  with mock.patch.object(m.BUILD,'check_checkout',return_value=rtc) as pinned, mock.patch.object(m.subprocess,'check_output',side_effect=responses), mock.patch.object(m.subprocess,'run',return_value=subprocess.CompletedProcess([],nested_rc,b'',b'')):
   result=m.check_source_checkout(root)
   pinned.assert_called_once_with(root)
   return result
 def test_exact_applied_patch_and_clean_submodules(self):self.assertEqual(self.check(),Path('/synthetic/sdk/third_party/libdatachannel/repo'))
 def test_unapplied_or_modified_sdk_patch_rejected(self):
  for diff in [b'',b'not the retained patch']:
   with self.subTest(diff=diff),self.assertRaises(ValueError):self.check(diff=diff)
 def test_dirty_rtc_source_rejected(self):
  with self.assertRaises(ValueError):self.check(rtc_dirty='src/peerconnection.cpp\n')
 def test_dirty_nested_submodule_rejected(self):
  with self.assertRaises(ValueError):self.check(nested_rc=1)
 def test_wrong_pinned_revision_is_not_bypassed(self):
  from unittest import mock
  with mock.patch.object(m.BUILD,'check_checkout',side_effect=ValueError('wrong commit')):
   with self.assertRaises(ValueError):m.check_source_checkout(Path('/synthetic/sdk'))

if __name__=='__main__':unittest.main()

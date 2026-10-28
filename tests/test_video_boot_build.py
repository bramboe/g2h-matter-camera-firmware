"""Wrong native profiles must fail before invoking the compiler."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('g2h_video_boot_build',ROOT/'tools/build_video_boot_gate.py')
recipe=importlib.util.module_from_spec(spec);spec.loader.exec_module(recipe)

class BootGateBuild(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.native=self.root/'native';self.native.write_bytes(b'own fixture')
        self.runtime=self.root/'runtime';self.runtime.mkdir()
        self.library=self.runtime/'libfixture.so'
        self.library.write_bytes(b'\x7fELF\x01\x01'+b'\0'*12+b'\x28\x00')
        self.native_patch=patch.object(recipe,'NATIVE_SHA256',hashlib.sha256(self.native.read_bytes()).hexdigest());self.native_patch.start();self.addCleanup(self.native_patch.stop)
        self.libs_patch=patch.object(recipe,'LIBRARIES',{'libfixture.so':hashlib.sha256(self.library.read_bytes()).hexdigest()});self.libs_patch.start();self.addCleanup(self.libs_patch.stop)

    def test_matching_profile_returns_only_explicit_library(self):
        self.assertEqual(recipe.verified_inputs(self.native,self.runtime),[self.library.resolve()])

    def test_wrong_native_or_runtime_never_invokes_compiler(self):
        with patch.object(recipe.subprocess,'run') as run:
            self.native.write_bytes(b'different native image')
            with self.assertRaisesRegex(ValueError,'Native camera image'):recipe.build(Path('zig'),self.native,self.runtime,self.root/'out')
            self.native.write_bytes(b'own fixture');self.library.write_bytes(b'different runtime')
            with self.assertRaisesRegex(ValueError,'Native runtime'):recipe.build(Path('zig'),self.native,self.runtime,self.root/'out')
            run.assert_not_called()

    def test_hash_matched_wrong_architecture_is_rejected(self):
        self.library.write_bytes(b'\x7fELF\x02\x01'+b'\0'*12+b'\x3e\x00')
        with patch.object(recipe,'LIBRARIES',{'libfixture.so':hashlib.sha256(self.library.read_bytes()).hexdigest()}):
            with self.assertRaisesRegex(ValueError,'ELF32 ARM'):recipe.verified_inputs(self.native,self.runtime)

    def test_existing_artifact_is_not_overwritten(self):
        out=self.root/'out';out.mkdir();artifact=out/'video-boot-gate.so';artifact.write_bytes(b'previous own artifact')
        version=json.loads((ROOT/'build/inputs.lock.json').read_text())['tools']['zig']
        with patch.object(recipe.subprocess,'check_output',return_value=version+'\n'),patch.object(recipe.subprocess,'run') as run:
            with self.assertRaisesRegex(ValueError,'never overwritten'):recipe.build(Path('zig'),self.native,self.runtime,out)
            run.assert_not_called()
        self.assertEqual(artifact.read_bytes(),b'previous own artifact')

if __name__=='__main__':unittest.main()

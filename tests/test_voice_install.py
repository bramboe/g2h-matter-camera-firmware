"""Verify private asset admission and actual filesystem rollback boundaries."""
import hashlib,io,os,subprocess,sys,tarfile,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'install'))
import g2h_voice_banks_live as install

AAC=bytes((255,241,96,64,1,31,252,0))

class VoiceInstallTests(unittest.TestCase):
    def bank(self,path):
        for language in ('en','nl'):
            bank=path/language;bank.mkdir(parents=True);manifest=''
            for name in install.FILES:
                file=name+'.aac';(bank/file).write_bytes(AAC)
                manifest+=hashlib.sha256(AAC).hexdigest()+'  '+file+'\n'
            (bank/'SHA256SUMS').write_text(manifest)
    def test_only_validated_fixed_assets_enter_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);self.bank(path);(path/'secret.txt').write_text('excluded')
            data,hashes=install.payload(path);self.assertEqual(len(hashes),36)
            with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                names=archive.getnames();self.assertEqual(len(names),36)
                self.assertTrue(all(m.isfile() for m in archive.getmembers()))
                self.assertNotIn('secret.txt',names)
            (path/'nl/add_ok.aac').unlink();(path/'nl/add_ok.aac').symlink_to(path/'en/add_ok.aac')
            with self.assertRaises(ValueError):install.payload(path)
    def test_corrupt_manifest_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);self.bank(path);(path/'nl/SHA256SUMS').write_text('unverified')
            with self.assertRaises(ValueError):install.payload(path)
    def test_timer_and_manual_recovery_preserve_original_and_foreign_paths(self):
        for state in ('alias','missing-alias','committed','foreign-alias'):
            with self.subTest(state=state),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);alias=root/'etc/ch/en';alias.parent.mkdir(parents=True)
                original=alias.parent/'original';original.mkdir();(original/'waiting.aac').write_bytes(AAC)
                current=root/'config/current';current.parent.mkdir();bank=root/'banks';bank.mkdir()
                current.symlink_to(bank/'en');transaction=root/'transaction';transaction.mkdir()
                (transaction/'original-md5').write_text(hashlib.md5(AAC).hexdigest()+'  waiting.aac\n')
                if state!='missing-alias':alias.symlink_to(current if state!='foreign-alias' else root/'foreign')
                if state=='committed':(transaction/'committed').touch()
                # The camera only has the filename form of md5sum, no -c flag.
                binpath=root/'bin';binpath.mkdir();md5=binpath/'md5sum'
                md5.write_text('#!'+sys.executable+'\nimport hashlib,sys\nfrom pathlib import Path\np=Path(sys.argv[1])\nprint(hashlib.md5(p.read_bytes()).hexdigest()+"  "+str(p))\n')
                md5.chmod(0o700);env=dict(os.environ,PATH=str(binpath)+os.pathsep+os.environ['PATH'])
                with patch.multiple(install,ALIAS=str(alias),CURRENT=str(current),BANK=str(bank)):
                    script=install.rollback_script(str(transaction),str(original))
                file=root/'rollback.sh';file.write_text(script)
                result=subprocess.run(['sh',str(file),'timed'],env=env,capture_output=True,timeout=10)
                if state=='foreign-alias':
                    self.assertNotEqual(result.returncode,0);self.assertTrue(original.is_dir())
                    self.assertEqual(os.readlink(alias),str(root/'foreign'))
                elif state=='committed':
                    self.assertEqual(result.returncode,0);self.assertTrue(alias.is_symlink())
                    result=subprocess.run(['sh',str(file),'force'],env=env,capture_output=True,timeout=10)
                    self.assertEqual(result.returncode,0,result.stderr);self.assertFalse(alias.is_symlink())
                else:
                    self.assertEqual(result.returncode,0,result.stderr);self.assertFalse(alias.is_symlink())
                if state!='foreign-alias':
                    self.assertEqual((alias/'waiting.aac').read_bytes(),AAC)
                    self.assertFalse(current.is_symlink());self.assertFalse(original.exists())

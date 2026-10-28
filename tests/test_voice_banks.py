"""Exercise real files, symlinks, hashes and commit/rollback durability faults."""
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

def openssl_flags():
    if shutil.which("pkg-config"):
        result=subprocess.run(["pkg-config","--cflags","--libs","openssl"],capture_output=True,text=True)
        if result.returncode==0:
            return shlex.split(result.stdout)
    for prefix in (Path("/opt/homebrew/opt/openssl@3"),Path("/usr/local/opt/openssl@3")):
        if (prefix/"include/openssl/evp.h").is_file():
            return ["-I"+str(prefix/"include"),"-L"+str(prefix/"lib"),"-lcrypto"]
    # Standard Linux development packages need no extra search directories.
    return ["-lcrypto"]

@unittest.skipUnless(shutil.which("c++"),"A C++17 compiler is required")
class VoiceBankTests(unittest.TestCase):
    def test_validated_atomic_bank_selection_and_rejections(self):
        with tempfile.TemporaryDirectory(prefix="g2h-voice-bank-test-") as directory:
            binary=Path(directory)/"bank-test"
            subprocess.run(["c++","-std=c++17","-pthread","-UNDEBUG",str(ROOT/"tests/test_voice_banks.cpp"),
                            "-o",str(binary),*openssl_flags()],check=True,capture_output=True,timeout=45)
            result=subprocess.run([str(binary)],check=True,capture_output=True,text=True,timeout=20)
            self.assertIn("VOICE_BANKS_ASSERTIONS_OK",result.stdout)

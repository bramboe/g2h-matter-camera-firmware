"""Check applied-language reporting, native failures and persisted voice choice."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_voice_banks import openssl_flags

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which("c++"),"A C++17 compiler is required")
class LanguageServiceTests(unittest.TestCase):
    def test_worker_reports_only_confirmed_native_voice_selection(self):
        with tempfile.TemporaryDirectory(prefix="g2h-language-service-") as directory:
            binary=Path(directory)/"service-test"
            result=subprocess.run(["c++","-std=c++17","-pthread","-UNDEBUG",str(ROOT/"tests/test_language_service.cpp"),
                                   "-o",str(binary),*openssl_flags()],capture_output=True,text=True,timeout=45)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn("LANGUAGE_SERVICE_ASSERTIONS_OK",result.stdout)

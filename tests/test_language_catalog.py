"""Validate locale/wire identities and bounded distinct user-facing labels."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which("c++"),"A C++17 compiler is required")
class LanguageCatalogTests(unittest.TestCase):
    def test_locale_roundtrip_and_mode_label_contract(self):
        with tempfile.TemporaryDirectory(prefix="g2h-language-catalog-") as directory:
            binary=Path(directory)/"catalog-test"
            subprocess.run(["c++","-std=c++17","-UNDEBUG",str(ROOT/"tests/test_language_catalog.cpp"),"-o",str(binary)],
                           check=True,capture_output=True,timeout=30)
            subprocess.run([str(binary)],check=True,capture_output=True,timeout=10)

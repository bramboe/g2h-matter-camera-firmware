"""Synthetic-only backup fixtures; no device, state, credentials or media."""
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import stat
import sys
import tempfile
import unittest

INSTALL = Path(__file__).resolve().parents[1] / "install"
sys.path.insert(0, str(INSTALL))
spec = importlib.util.spec_from_file_location("g2h_backup_live", INSTALL / "g2h_backup_live.py")
backup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backup)


class FakeTelnet:
    def __init__(self):
        self.data = {
            backup.FILES[0][0]: bytes(range(256)) * 513,  # >2 full 64 KiB chunks
            backup.FILES[1][0]: b"#!/bin/sh\nexec /example/app --serve\n",
        }
        self.commands = []
        self.chunk_failure = None
        self.change_checksum = False
        self.md5_calls = {}
        self.boot_calls = 0
        self.change_boot = False
        self.symlinks = {}
        self.active_path = backup.BASE_APP

    def run(self, command, timeout=8):
        self.commands.append(command)
        if command.startswith("[ ! -L "):
            path = command.split()[3]
            return self.symlinks.get(path, path)
        if command.startswith("if [ -L "):
            path = command.split()[3]
            return "symlink" if path in self.symlinks else "present" if path in self.data else "absent"
        if command == "readlink /proc/123/exe":
            return self.active_path
        if command == "cat /proc/123/stat":
            return "123 (camera) " + " ".join(["S"] + ["0"] * 18 + ["23456"]) + "\n"
        if command.startswith("stat -c %s "):
            return str(len(self.data[command[len("stat -c %s "):]])) + "\n"
        if command.startswith("md5sum "):
            path = command[7:]
            self.md5_calls[path] = self.md5_calls.get(path, 0) + 1
            digest = hashlib.md5(self.data[path]).hexdigest()
            if self.change_checksum and self.md5_calls[path] > 1:
                digest = "0" * 32
            return digest + "  " + path + "\n"
        if command == "base64 " + backup.SELECTOR + " | awk 'NR<=5{print} NR==5{exit}'":
            return base64.encodebytes(self.data[backup.SELECTOR][:285]).decode()
        if command.startswith("tail -c +"):
            match = re.fullmatch(r"tail -c \+(\d+) (\S+) \| base64 \| awk 'NR<=1024\{print\} NR==1024\{exit\}'", command)
            if not match:
                raise AssertionError(command)
            start = int(match[1]) - 1
            data = self.data[match[2]][start:start + backup.CHUNK]
            if self.chunk_failure == "short":
                data = data[:-1]
            if self.chunk_failure == "corrupt":
                data = bytes([data[0] ^ 1]) + data[1:]
            if self.chunk_failure == "encoding":
                return "!!!"
            if self.chunk_failure == "huge":
                return "A" * 90001
            return base64.encodebytes(data).decode()
        if command.startswith("if [ -f "):
            path = command.split()[3]
            return "present" if path in self.data else "absent"
        if command == "cat /proc/sys/kernel/random/boot_id":
            self.boot_calls += 1
            return ("2" if self.change_boot and self.boot_calls > 1 else "1") + "1111111-1111-1111-1111-111111111111\n"
        if command.startswith("for p in /proc/"):
            fields = ["S"] + ["0"] * 18 + ["12345"]
            return "211 (camera) " + " ".join(fields) + "\n"
        if command == "/etc/init.d/S96g2hmattercamera status":
            return json.dumps({"pid": 123, "ready": True, "fabrics": 2,
                               "thread_role": "router", "unexpected_private_field": "excluded"})
        if command == "df -k /customer /tmp /mnt/config":
            return "Filesystem 1K-blocks Used Available Use% Mounted\nexample 200 10 190 5% /customer\n"
        raise AssertionError("unexpected remote command: " + command)


class BackupTests(unittest.TestCase):
    def test_stream_multichunk_exact_and_private(self):
        session = FakeTelnet()
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "backup"
            result = backup.backup_file(session, backup.FILES[0][0], path)
            self.assertEqual(path.read_bytes(), session.data[backup.FILES[0][0]])
            self.assertEqual(result["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual(sum(c.startswith("tail -c +") for c in session.commands), 3)

    def test_failures_never_publish_a_complete_file(self):
        for failure in ("short", "corrupt", "encoding", "huge", "changed"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                session = FakeTelnet()
                session.chunk_failure = failure
                session.change_checksum = failure == "changed"
                target = Path(temp) / "backup"
                with self.assertRaises(backup.BackupError):
                    backup.backup_file(session, backup.FILES[0][0], target)
                self.assertFalse(target.exists())
                self.assertFalse(target.with_name("backup.partial").exists())

    def test_reject_oversize_and_nonallowlisted_path_before_transfer(self):
        session = FakeTelnet()
        with tempfile.TemporaryDirectory() as temp:
            for path, limit in ((backup.FILES[0][0], 1), ("/mnt/config/example", 100)):
                with self.assertRaises(backup.BackupError):
                    backup.backup_file(session, path, Path(temp) / "backup", limit)
        self.assertFalse(any(c.startswith("tail -c +") for c in session.commands))

    def test_complete_manifest_excludes_unknown_status_and_marks_optional_absent(self):
        session = FakeTelnet()
        with tempfile.TemporaryDirectory() as temp:
            directory = backup.create_backup(session, Path(temp))
            manifest = json.loads((directory / "manifest.json").read_text())
            self.assertTrue(manifest["complete"])
            self.assertEqual(len(manifest["files"]), 3)
            self.assertEqual(len(manifest["missing_optional"]), 2)
            self.assertNotIn("unexpected_private_field", manifest["status_before"])
            self.assertEqual(manifest["before"]["vendor_start_ticks"], 12345)
            self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE((directory / "manifest.json").stat().st_mode), 0o600)
            self.assertFalse(any("ttyS2" in c or "kill " in c or " > " in c for c in session.commands))

    def test_matching_verified_content_reused_with_new_remote_checks(self):
        session=FakeTelnet()
        with tempfile.TemporaryDirectory() as temp:
            original=backup.create_backup(session,Path(temp));session.commands=[]
            current=backup.create_backup(session,Path(temp),reuse=original)
            self.assertTrue(json.loads((current/'manifest.json').read_text())['complete'])
            self.assertEqual((current/'matter-camera').read_bytes(),(original/'matter-camera').read_bytes())
            self.assertFalse(any(c.startswith('tail -c +') for c in session.commands))
            self.assertGreater(sum(c.startswith('md5sum ') for c in session.commands),1)

    def test_corrupt_cache_is_never_accepted(self):
        session=FakeTelnet()
        with tempfile.TemporaryDirectory() as temp:
            original=backup.create_backup(session,Path(temp))
            source=original/'matter-camera';data=source.read_bytes();source.write_bytes(bytes([data[0]^1])+data[1:])
            with self.assertRaises(backup.BackupError):backup.create_backup(session,Path(temp),reuse=original)

    def test_changed_remote_content_requires_full_fresh_transfer(self):
        session=FakeTelnet()
        with tempfile.TemporaryDirectory() as temp:
            original=backup.create_backup(session,Path(temp));session.commands=[]
            session.data[backup.BASE_APP]=b'new synthetic app'
            current=backup.create_backup(session,Path(temp),reuse=original)
            self.assertEqual((current/'matter-camera').read_bytes(),b'new synthetic app')
            self.assertTrue(any(c.startswith('tail -c +') for c in session.commands))

    def test_reboot_marks_backup_incomplete(self):
        session = FakeTelnet()
        session.change_boot = True
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(backup.BackupError):
                backup.create_backup(session, Path(temp))
            manifests = list(Path(temp).glob("*/manifest.json"))
            self.assertEqual(len(manifests), 1)
            self.assertFalse(json.loads(manifests[0].read_text())["complete"])

    def test_optional_installed_broker_is_backed_up(self):
        session = FakeTelnet()
        session.data[backup.FILES[2][0]] = b"synthetic-broker"
        session.data[backup.FILES[3][0]] = b"#!/bin/sh\n# synthetic fixture\n"
        with tempfile.TemporaryDirectory() as temp:
            directory = backup.create_backup(session, Path(temp))
            manifest = json.loads((directory / "manifest.json").read_text())
            self.assertEqual(len(manifest["files"]), 5)
            self.assertEqual(manifest["missing_optional"], [])

    def test_selected_release_and_original_fallback_are_both_preserved(self):
        session = FakeTelnet()
        release = b"synthetic-current-release"
        digest = hashlib.sha256(release).hexdigest()
        selected = "/mnt/sdcard/g2h-matter-camera/releases/" + digest + "/camera"
        session.data[selected] = release
        session.data[backup.SELECTOR] = (selected + "\n").encode()
        session.active_path = selected
        with tempfile.TemporaryDirectory() as temp:
            directory = backup.create_backup(session, Path(temp))
            manifest = json.loads((directory / "manifest.json").read_text())
            self.assertEqual((directory / "matter-camera-active").read_bytes(), release)
            self.assertEqual((directory / "matter-camera").read_bytes(), session.data[backup.BASE_APP])
            self.assertEqual((directory / "app-path").read_bytes(), session.data[backup.SELECTOR])
            self.assertEqual(manifest["app_before"]["executable"], selected)

    def test_selector_rejects_traversal_uppercase_extra_lines_and_oversize(self):
        examples = [b"/customer/g2h-matter-camera/../camera\n",
                    ("/customer/g2h-matter-camera/releases/" + "A" * 64 + "/camera\n").encode(),
                    (backup.BASE_APP + "\nextra\n").encode(), b"A" * 300]
        for data in examples:
            with self.subTest(data=data[:60]):
                session = FakeTelnet()
                session.data[backup.SELECTOR] = data
                with self.assertRaises(backup.BackupError):
                    backup.read_selection(session)

    def test_selector_and_release_symlinks_fail_closed(self):
        session = FakeTelnet()
        session.data[backup.SELECTOR] = (backup.BASE_APP + "\n").encode()
        session.symlinks[backup.SELECTOR] = "/other"
        with self.assertRaises(backup.BackupError):
            backup.read_selection(session)
        session.symlinks = {backup.BASE_APP: "/other"}
        with self.assertRaises(backup.BackupError):
            backup.read_selection(session)

    def test_selected_executable_must_match_running_process(self):
        session = FakeTelnet()
        session.active_path = "/unexpected"
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(backup.BackupError):
                backup.create_backup(session, Path(temp))

    def test_release_directory_digest_must_match_download(self):
        session = FakeTelnet()
        selected = "/customer/g2h-matter-camera/releases/" + "0" * 64 + "/camera"
        session.data[selected] = b"synthetic"
        session.data[backup.SELECTOR] = selected.encode()
        session.active_path = selected
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(backup.BackupError):
                backup.create_backup(session, Path(temp))

    def test_existing_complete_file_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "backup"
            path.write_bytes(b"preserve")
            with self.assertRaises(backup.BackupError):
                backup.backup_file(FakeTelnet(), backup.FILES[0][0], path)
            self.assertEqual(path.read_bytes(), b"preserve")

    def test_existing_local_file_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "backup.partial"
            path.write_bytes(b"preserve")
            with self.assertRaises(FileExistsError):
                backup.backup_file(FakeTelnet(), backup.FILES[0][0], Path(temp) / "backup")
            self.assertEqual(path.read_bytes(), b"preserve")


if __name__ == "__main__":
    unittest.main()

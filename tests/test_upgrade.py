"""Offline update transaction tests using synthetic binaries and a fake camera."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

INSTALL = Path(__file__).resolve().parents[1] / "install"
sys.path.insert(0, str(INSTALL))
spec = importlib.util.spec_from_file_location("g2h_upgrade_test_target", INSTALL / "g2h_camera_upgrade_live.py")
upgrade = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upgrade)


def elf_fixture():
    data = bytearray(65536)
    data[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", data, 16, 2, 40, 1, 0x10000, 52, 0, 0x5000400,
                     52, 32, 1, 40, 0, 0)
    struct.pack_into("<8I", data, 52, 1, 0, 0x10000, 0x10000, len(data), len(data), 5, 0x1000)
    return bytes(data)


class FakeCamera:
    def __init__(self, image, broker):
        self.image, self.broker = image, broker
        self.commands = []
        self.events = []
        self.old = upgrade.BASE + "/camera"
        self.old_md5 = "a" * 32
        self.corrupt_slot = False
        self.free = 100000
        self.fail_at = None
        self.selector = self.old
        self.native_open = False
        self.force_old_exe = False
        self.rollback_failure = False

    def run(self, command, timeout=8):
        self.commands.append(command)
        if "\ntouch " in command:
            raise RuntimeError("camera shell: touch not available")
        if command.startswith("[ ! -L "):
            return command.split()[3]
        if command == "readlink /proc/123/exe":
            return self.old if self.force_old_exe else self.selector
        if command == "cat /proc/123/stat":
            return "123 (camera) " + " ".join(["S"] + ["0"] * 18 + ["23456"]) + "\n"
        if command.startswith("if [ -f ") and "media-broker-enabled" in command:
            return "0"
        if command == "ls -l /proc/123/fd":
            return "3 -> socket:[1234]" if self.native_open else ""
        if command == "cat /proc/net/tcp":
            return "header\n 0: 0100007F:1234 0100007F:022A 01 0 0 0 0 0 1234\n" if self.native_open else "header\n"
        if command.startswith("if [ -r ") and "/app-path" in command:
            return self.selector + "\n"
        if command.startswith("md5sum "):
            path = command[7:]
            if path == self.old:
                digest = self.old_md5
            elif path == upgrade.BROKER:
                digest = hashlib.md5(self.broker).hexdigest()
            elif "/releases/" in path:
                digest = "0" * 32 if self.corrupt_slot else hashlib.md5(self.image).hexdigest()
            else:
                raise AssertionError(command)
            return digest + "  " + path + "\n"
        if command.startswith("df -k "):
            return str(self.free)
        if command == "[ ! -e " + upgrade.PENDING + " ]":
            return ""
        if command.startswith("if [ -e ") and "/releases/" in command:
            return ""
        if command.startswith("if [ -e ") and upgrade.BROKER in command:
            return "yes\n"
        if command.startswith("mount |"):
            return ""
        if command == upgrade.INIT + " rollback":
            self.events.append("rollback")
            self.selector = self.old
            if self.fail_at == "rollback" or self.rollback_failure:
                raise RuntimeError("synthetic rollback failure")
            return ""
        if command == upgrade.INIT + " stop":
            self.events.append("stop")
            if self.fail_at == "stop":
                raise RuntimeError("synthetic stop failure")
            return ""
        if command == upgrade.INIT + " start":
            self.events.append("start")
            return ""
        if command.startswith("sh ") and "update-guard.sh" in command:
            self.events.append("guardian")
            if self.fail_at == "guardian":
                raise RuntimeError("synthetic guardian launch failure")
            return ""
        if command.startswith("set -e\n"):
            if "guard.ready" in command:
                self.events.append("guardian-ready")
                if self.fail_at == "guardian-ready":
                    raise RuntimeError("synthetic guardian readiness failure")
            elif "app-path.next" in command:
                self.events.append("select")
                self.selector = upgrade.BASE + "/releases/" + hashlib.sha256(self.image).hexdigest() + "/camera"
                if self.fail_at == "select":
                    raise RuntimeError("synthetic switch failure")
            elif "update-complete-" in command:
                self.events.append("commit")
                if self.fail_at == "commit":
                    raise RuntimeError("synthetic commit failure")
            elif "update-pending" in command:
                self.events.append("transaction")
                if self.fail_at == "transaction":
                    raise RuntimeError("synthetic transaction failure")
            else:
                self.events.append("stage")
            return ""
        if command.startswith("rm -f "):
            self.events.append("cleanup")
            return ""
        raise AssertionError("unhandled command: " + command)


class Clock:
    def __init__(self):
        self.now = 0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class UpgradeTests(unittest.TestCase):
    def setUp(self):
        self.image = elf_fixture()
        self.broker = self.image[:-1] + b"B"
        self.camera = FakeCamera(self.image, self.broker)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.backup_dir = Path(self.temp.name) / "verified"
        self.backup_dir.mkdir()
        self.clock = Clock()
        self.status = {"pid": 123, "ready": True, "fabrics": 2, "video_session": False,
                       "thread_fresh": True, "source_fault": False, "media_broker": True}
        for name, value in (
            ("identity", lambda session: {"boot_id": "synthetic-boot", "vendor_pid": 211, "vendor_start_ticks": 1234}),
            ("safe_status", lambda session: dict(self.status)),
            ("create_backup", lambda session, root: self.backup_dir),
            ("upload", lambda *args, **kwargs: self.camera.events.append("upload")),
            ("install_small", lambda *args, **kwargs: self.camera.events.append("helper")),
        ):
            patcher = patch.object(upgrade, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        patcher = patch.object(upgrade, "time", self.clock)
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_upgrade(self):
        return upgrade.upgrade(self.camera, self.image, self.broker, Path(self.temp.name), qualify_seconds=15)

    def test_no_backup_means_no_mutation(self):
        with patch.object(upgrade, "create_backup", side_effect=RuntimeError("synthetic download failure")):
            with self.assertRaises(RuntimeError):
                self.run_upgrade()
        self.assertEqual(self.camera.events, [])
        self.assertEqual(self.camera.selector, self.camera.old)

    def test_unchanged_hash_is_true_noop(self):
        self.camera.old_md5 = hashlib.md5(self.image).hexdigest()
        with patch.object(upgrade, "create_backup", side_effect=AssertionError("unexpected backup")):
            result = self.run_upgrade()
        self.assertTrue(result["unchanged"])
        self.assertEqual(self.camera.events, [])
        self.assertEqual(len(self.camera.commands), 2)

    def test_insufficient_space_preserves_original(self):
        self.camera.free = 1
        with self.assertRaisesRegex(RuntimeError, "insufficient space"):
            self.run_upgrade()
        self.assertEqual(self.camera.events, [])
        self.assertEqual(self.camera.selector, self.camera.old)

    def test_corrupt_uploaded_candidate_never_stops_app(self):
        self.camera.corrupt_slot = True
        with self.assertRaisesRegex(RuntimeError, "candidate slot mismatch"):
            self.run_upgrade()
        self.assertNotIn("stop", self.camera.events)
        self.assertNotIn("select", self.camera.events)
        self.assertEqual(self.camera.selector, self.camera.old)

    def test_failed_transaction_does_not_stop_app(self):
        self.camera.fail_at = "transaction"
        with self.assertRaises(RuntimeError):
            self.run_upgrade()
        self.assertNotIn("stop", self.camera.events)
        self.assertNotIn("select", self.camera.events)

    def test_guardian_launch_failure_requests_rollback_before_stop(self):
        self.camera.fail_at = "guardian"
        with self.assertRaises(RuntimeError):
            self.run_upgrade()
        self.assertIn("rollback", self.camera.events)
        self.assertNotIn("stop", self.camera.events)

    def test_guardian_readiness_failure_requests_rollback_before_stop(self):
        self.camera.fail_at = "guardian-ready"
        with self.assertRaises(RuntimeError):
            self.run_upgrade()
        self.assertIn("rollback", self.camera.events)
        self.assertNotIn("stop", self.camera.events)

    def test_first_migration_with_owned_native_stream_is_refused(self):
        self.camera.native_open = True
        with self.assertRaisesRegex(RuntimeError, "clean boot"):
            self.run_upgrade()
        self.assertEqual(self.camera.events, [])

    def test_candidate_cannot_qualify_on_old_executable(self):
        self.camera.force_old_exe = True
        with self.assertRaisesRegex(RuntimeError, "executable does not match"):
            self.run_upgrade()
        self.assertIn("rollback", self.camera.events)
        self.assertNotIn("commit", self.camera.events)

    def test_switch_stop_and_commit_failures_request_rollback(self):
        for failure in ("select", "stop", "commit"):
            with self.subTest(failure=failure):
                self.camera.events = []
                self.camera.selector = self.camera.old
                self.camera.fail_at = failure
                with self.assertRaises(RuntimeError):
                    self.run_upgrade()
                self.assertIn("rollback", self.camera.events)
                self.assertEqual(self.camera.selector, self.camera.old)
                (self.backup_dir / "upgrade.json").unlink(missing_ok=True)

    def test_failed_immediate_rollback_records_guardian_recovery_pending(self):
        self.camera.fail_at = "stop"
        self.camera.rollback_failure = True
        with self.assertRaises(RuntimeError):
            self.run_upgrade()
        record = json.loads((self.backup_dir / "upgrade.json").read_text())
        self.assertTrue(record["rollback_waiting_for_guardian_or_reboot"])
        self.assertFalse(record["verified"])
        self.assertNotIn("commit", self.camera.events)

    def test_live_session_refuses_any_update(self):
        self.status["video_session"] = True
        with self.assertRaisesRegex(RuntimeError, "live view"):
            self.run_upgrade()
        self.assertEqual(self.camera.events, [])

    def test_python_selector_rejects_traversal_foreign_paths_uppercase(self):
        valid = "/mnt/sdcard/g2h-matter-camera/releases/" + "a" * 64 + "/camera"
        self.assertEqual(upgrade.selected_path(valid + "\n"), valid)
        for path in ("/tmp/app", valid.replace("/camera", "/../camera"),
                     valid.replace("a" * 64, "A" * 64), valid + "\n" + valid):
            with self.subTest(path=path), self.assertRaises(ValueError):
                upgrade.selected_path(path)

    def test_compressed_payload_contains_only_exact_executable(self):
        import io,tarfile
        compressed=upgrade.packed_image(self.image)
        self.assertLess(len(compressed),len(self.image))
        with tarfile.open(fileobj=io.BytesIO(compressed),mode='r:gz') as archive:
            self.assertEqual(archive.getnames(),['camera'])
            self.assertEqual(archive.extractfile('camera').read(),self.image)
            self.assertEqual(archive.getmember('camera').mode,0o700)

    def test_image_validation_rejects_wrong_architecture_and_bounds(self):
        path = Path(self.temp.name) / "candidate"
        path.write_bytes(self.image)
        self.assertEqual(upgrade.image_bytes(path), self.image)
        for data in (b"", self.image[:100], b"Z" * 65536,
                     self.image[:18] + b"\x3e\x00" + self.image[20:]):
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                upgrade.image_bytes(path)

    def test_service_release_path_validator_matches_python_allowlist(self):
        # Execute the actual shipped validator function, without executing the
        # service or touching /proc, /etc or any camera path.
        source = (INSTALL / "services" / "S96g2hmattercamera").read_text()
        function = source[source.index("valid_path() {"):source.index("\nselect_app()")]
        valid = "/customer/g2h-matter-camera/releases/" + "a" * 64 + "/camera"
        for path, expected in ((upgrade.BASE + "/camera", True), (valid, True),
                               (valid.replace("a" * 64, "short"), False),
                               (valid.replace("a" * 64, "a" * 64 + "/nested"), False),
                               (valid.replace("a" * 64, "A" * 64), False)):
            with self.subTest(path=path):
                result = subprocess.run(["/bin/sh", "-c", function + '\nvalid_path "$1"', "test", path])
                self.assertEqual(result.returncode == 0, expected)

    def test_interrupted_update_reboot_restores_previous_before_launch(self):
        # Run the actual service in an isolated filesystem fixture. Only fixed
        # filesystem prefixes and the boot-id source are relocated; no service
        # functions are mocked. The executable is our own one-line marker script.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            cfg, base, sd, ram = (root / name for name in ("cfg", "base", "sd", "ram"))
            for path in (cfg, base, sd, ram):
                path.mkdir()
            (cfg / "state").mkdir()
            (cfg / "identity").write_text("synthetic fixture only")
            (cfg / "enabled").touch()
            (cfg / "media-broker-enabled").touch()
            previous = base / "camera"
            marker = root / "launched"
            previous.write_text("#!/bin/sh\nprintf previous > '" + str(marker) + "'\n")
            previous.chmod(0o700)
            candidate = base / "releases" / ("a" * 64) / "camera"
            candidate.parent.mkdir(parents=True)
            candidate.write_text("#!/bin/sh\nprintf candidate > '" + str(marker) + "'\n")
            candidate.chmod(0o700)
            (cfg / "app-path").write_text(str(candidate) + "\n")
            pending = cfg / "update-pending"
            pending.mkdir()
            (pending / "previous").write_text(str(previous) + "\n")
            (pending / "candidate").write_text(str(candidate) + "\n")
            (pending / "boot").write_text("old-boot\n")
            (pending / "previous-broker").write_text("0\n")
            (root / "boot").write_text("new-boot\n")
            source = (INSTALL / "services" / "S96g2hmattercamera").read_text()
            for old, new in (("/mnt/config/g2h-matter-camera", cfg),
                             ("/customer/g2h-matter-camera", base),
                             ("/mnt/sdcard/g2h-matter-camera", sd),
                             ("/tmp/g2h", ram / "g2h"),
                             ("/proc/sys/kernel/random/boot_id", root / "boot")):
                source = source.replace(old, str(new))
            service = root / "service"
            service.write_text(source)
            service.chmod(0o700)
            # Test pointer ordering, not a physical power-cut or flushing every
            # mounted Mac disk (which may include unrelated network volumes).
            commands=root / "commands";commands.mkdir()
            sync=commands / "sync";sync.write_text("#!/bin/sh\nexit 0\n");sync.chmod(0o700)
            environment=dict(os.environ,PATH=str(commands)+os.pathsep+os.environ['PATH'])
            result = subprocess.run(["/bin/sh", str(service), "start"], capture_output=True, timeout=10,env=environment)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            for _ in range(50):
                if marker.exists():
                    break
                time.sleep(0.02)
            self.assertEqual(marker.read_text(), "previous")
            self.assertEqual((cfg / "app-path").read_text(), str(previous) + "\n")
            self.assertFalse(pending.exists())
            self.assertFalse((cfg / "media-broker-enabled").exists())
            self.assertEqual((cfg / "identity").read_text(), "synthetic fixture only")
            self.assertTrue(candidate.exists())


if __name__ == "__main__":
    unittest.main()

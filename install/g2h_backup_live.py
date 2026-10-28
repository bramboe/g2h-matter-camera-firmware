"""Read-only, bounded backup of the installed G2H app before an update.

Backups are private and outside the source repository by default. This utility
never reads Matter identity/state, Thread datasets, logs, or credentials.
"""
from __future__ import annotations

import argparse
import base64
import binascii
from datetime import datetime, timezone
import getpass
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import uuid

from g2h_identification_probe import Telnet

CHUNK = 57 * 1024  # 1024 complete 76-column base64 lines
MAX_BINARY = 32 * 1024 * 1024
SELECTOR = "/mnt/config/g2h-matter-camera/app-path"
BASE_APP = "/customer/g2h-matter-camera/camera"
RELEASE_PATH = re.compile(r"(?:/customer|/mnt/sdcard)/g2h-matter-camera/releases/([0-9a-f]{64})/camera")
FILES = (
    ("/customer/g2h-matter-camera/camera", "matter-camera", True, MAX_BINARY),
    ("/etc/init.d/S96g2hmattercamera", "S96g2hmattercamera", True, CHUNK),
    ("/customer/g2h-matter-camera/g2h-media-broker", "g2h-media-broker", False, MAX_BINARY),
    ("/etc/init.d/S95g2hmediabroker", "S95g2hmediabroker", False, CHUNK),
)
ALLOWED_PATHS = frozenset(row[0] for row in FILES) | {SELECTOR}
STATUS_KEYS = ("pid", "ready", "fabrics", "video_session", "source_fault",
               "thread_fresh", "thread_role", "thread_channel", "media_broker", "monotonic_ms")


class BackupError(RuntimeError):
    pass


def allowlisted(path):
    return path in ALLOWED_PATHS or RELEASE_PATH.fullmatch(path) is not None


def canonical_file(session, path):
    if not allowlisted(path):
        raise BackupError("path is outside the backup allowlist")
    quoted = shlex.quote(path)
    result = session.run("[ ! -L " + quoted + " ] && readlink -f " + quoted, timeout=8).strip()
    if result != path:
        raise BackupError("backup source has a symlink or unexpected canonical path")


def read_selection(session):
    quoted = shlex.quote(SELECTOR)
    state = session.run("if [ -L " + quoted + " ]; then printf symlink; elif [ -f " +
                        quoted + " ]; then printf present; else printf absent; fi", timeout=8).strip()
    if state == "absent":
        return BASE_APP, False
    if state != "present":
        raise BackupError("invalid app selector file")
    canonical_file(session, SELECTOR)
    size = session.run("stat -c %s " + quoted, timeout=8).strip()
    if not size.isdigit() or not 1 <= int(size) <= 256:
        raise BackupError("invalid app selector length")
    encoded = session.run("base64 " + quoted + " | awk 'NR<=5{print} NR==5{exit}'", timeout=8)
    if len(encoded) > 512:
        raise BackupError("oversized app selector")
    try:
        raw = base64.b64decode("".join(encoded.split()), validate=True)
        text = raw.decode("ascii")
    except (ValueError, UnicodeError, binascii.Error) as exc:
        raise BackupError("invalid app selector encoding") from exc
    if not 1 <= len(raw) <= 256 or len(raw) != int(size):
        raise BackupError("invalid app selector length")
    selected = text[:-1] if text.endswith("\n") else text
    if selected != BASE_APP and RELEASE_PATH.fullmatch(selected) is None:
        raise BackupError("app selector is outside the permitted release roots")
    canonical_file(session, selected)
    return selected, True


def app_identity(session, status, selected):
    canonical_file(session, selected)
    pid = status["pid"]
    actual = session.run("readlink /proc/" + str(pid) + "/exe", timeout=8).strip()
    if actual != selected:
        raise BackupError("running Matter executable does not match app selector")
    raw = session.run("cat /proc/" + str(pid) + "/stat", timeout=8).strip()
    match = re.fullmatch(r"([0-9]+) \(.*\) (.*)", raw)
    if not match or int(match[1]) != pid:
        raise BackupError("invalid Matter process identity")
    fields = match[2].split()
    if len(fields) < 20 or not fields[19].isdigit():
        raise BackupError("invalid Matter process start time")
    return {"pid": pid, "start_ticks": int(fields[19]), "executable": actual}


def private_json(path: Path, value):
    temporary = path.with_name(path.name + ".partial")
    data = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def remote_info(session, path: str, limit: int):
    if not allowlisted(path):
        raise BackupError("path is outside the backup allowlist")
    canonical_file(session, path)
    quoted = shlex.quote(path)
    text = session.run("stat -c %s " + quoted, timeout=8).strip()
    if not re.fullmatch(r"[0-9]{1,10}", text):
        raise BackupError("invalid remote file size")
    size = int(text)
    if not 1 <= size <= limit:
        raise BackupError("remote file exceeds permitted size")
    checksum = session.run("md5sum " + quoted, timeout=20).strip().split()
    if len(checksum) != 2 or checksum[1] != path or not re.fullmatch(r"[0-9a-f]{32}", checksum[0]):
        raise BackupError("invalid remote checksum")
    return size, checksum[0]


def backup_file(session, remote: str, destination: Path, limit=MAX_BINARY):
    """Stream at most 64 KiB per remote read; never create a remote file."""
    if destination.exists() or destination.is_symlink():
        raise BackupError("local backup destination already exists")
    size, expected_md5 = remote_info(session, remote, limit)
    temporary = destination.with_name(destination.name + ".partial")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    md5, sha256 = hashlib.md5(), hashlib.sha256()
    try:
        with os.fdopen(fd, "wb") as stream:
            for offset in range(0, size, CHUNK):
                command = ("tail -c +" + str(offset + 1) + " " + shlex.quote(remote) +
                           " | base64 | awk 'NR<=1024{print} NR==1024{exit}'")
                encoded = session.run(command, timeout=20)
                if len(encoded) > 90000:
                    raise BackupError("oversized transfer response")
                try:
                    block = base64.b64decode("".join(encoded.split()), validate=True)
                except (ValueError, binascii.Error) as exc:
                    raise BackupError("invalid transfer encoding") from exc
                if len(block) != min(CHUNK, size - offset):
                    raise BackupError("short or oversized transfer chunk")
                stream.write(block)
                md5.update(block)
                sha256.update(block)
            stream.flush()
            os.fsync(stream.fileno())
        if remote_info(session, remote, limit) != (size, expected_md5):
            raise BackupError("remote file changed during backup")
        if md5.hexdigest() != expected_md5:
            raise BackupError("download checksum differs from remote file")
        os.replace(temporary, destination)
        return {"remote_path": remote, "local_file": destination.name, "bytes": size,
                "md5": expected_md5, "sha256": sha256.hexdigest()}
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def identity(session):
    boot = session.run("cat /proc/sys/kernel/random/boot_id", timeout=8).strip()
    if not re.fullmatch(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", boot):
        raise BackupError("invalid boot identity")
    # Read the vendor identity only. No command line, environment or memory dump.
    command = ("for p in /proc/[0-9]*; do "
               "[ -r \"$p/comm\" ] || continue; "
               "[ \"$(cat \"$p/comm\" 2>/dev/null)\" = camera ] || continue; "
               "[ \"$(readlink \"$p/exe\" 2>/dev/null)\" = /tmp/out/camera ] || continue; "
               "cat \"$p/stat\"; done")
    rows = session.run(command, timeout=10).strip().splitlines()
    if len(rows) != 1:
        raise BackupError("expected exactly one vendor camera process")
    match = re.fullmatch(r"([0-9]+) \(camera\) (.*)", rows[0])
    if not match:
        raise BackupError("invalid vendor process identity")
    fields = match[2].split()
    if len(fields) < 20 or not fields[19].isdigit():
        raise BackupError("invalid vendor process start time")
    return {"boot_id": boot, "vendor_pid": int(match[1]), "vendor_start_ticks": int(fields[19])}


def safe_status(session):
    raw = session.run("/etc/init.d/S96g2hmattercamera status", timeout=12)
    if len(raw) > 16384:
        raise BackupError("oversized service status")
    try:
        status = json.loads(raw)
    except (ValueError, TypeError) as exc:
        raise BackupError("service status is not JSON") from exc
    if not isinstance(status, dict):
        raise BackupError("invalid service status")
    if (type(status.get("fabrics")) is not int or not 0 <= status["fabrics"] <= 254 or
            type(status.get("pid")) is not int or status["pid"] < 2 or status.get("ready") is not True):
        raise BackupError("service is not ready with a valid fabric count")
    result = {}
    for key in STATUS_KEYS:
        value = status.get(key)
        if isinstance(value, (bool, int)) or value is None:
            result[key] = value
        elif key == "thread_role" and value in ("disabled", "detached", "child", "router", "leader"):
            result[key] = value
    return result


def reuse_verified_file(session, remote, destination, limit, cache, cached):
    """Deduplicate unchanged content; recheck the current remote metadata twice."""
    record=next((r for r in cached.get('files',[]) if r.get('remote_path')==remote and r.get('local_file')==destination.name),None)
    if record is None:return None
    info=remote_info(session,remote,limit)
    if info!=(record.get('bytes'),record.get('md5')):return None
    source=cache/destination.name
    if source.is_symlink() or not source.is_file() or source.stat().st_size!=info[0]:raise BackupError('cached backup file unavailable')
    data=source.read_bytes()
    if hashlib.sha256(data).hexdigest()!=record.get('sha256') or hashlib.md5(data).hexdigest()!=info[1]:
        raise BackupError('cached backup content changed')
    fd=os.open(destination,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as stream:
        stream.write(data);stream.flush();os.fsync(stream.fileno())
    if remote_info(session,remote,limit)!=info:
        destination.unlink();raise BackupError('remote file changed during local reuse')
    return dict(record,content_source='verified-local-copy-current-remote-size-md5')

def create_backup(session, output: Path, reuse: Path | None = None):
    cached={}
    if reuse is not None:
        file=reuse/'manifest.json'
        if reuse.is_symlink() or file.is_symlink() or file.stat().st_size>131072:
            raise BackupError('invalid verified backup cache')
        cached=json.loads(file.read_text())
        if cached.get('complete') is not True or cached.get('format')!=1:
            raise BackupError('complete verified cache required')
    output.mkdir(parents=True, exist_ok=True)
    # Refuse a symlink as the requested destination; each run gets a fresh dir.
    if output.is_symlink():
        raise BackupError("backup parent must not be a symlink")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    directory = output / ("g2h-" + stamp + "-" + uuid.uuid4().hex[:12])
    directory.mkdir(mode=0o700)
    os.chmod(directory, 0o700)
    manifest = {"format": 1, "complete": False, "created_utc": stamp,
                "camera_mutations": False, "files": [], "missing_optional": []}
    def obtain(remote,name,limit=MAX_BINARY):
        result=reuse_verified_file(session,remote,directory/name,limit,reuse,cached) if reuse else None
        return result if result is not None else backup_file(session,remote,directory/name,limit)
    try:
        manifest["before"] = identity(session)
        manifest["status_before"] = safe_status(session)
        selected, selector_present = read_selection(session)
        manifest["selected_app"] = selected
        manifest["app_before"] = app_identity(session, manifest["status_before"], selected)
        # df contains capacity/mount metadata only, never configuration contents.
        disk = session.run("df -k /customer /tmp /mnt/config", timeout=10)
        if len(disk) > 4096:
            raise BackupError("oversized disk-capacity response")
        manifest["disk_capacity"] = disk.strip().splitlines()
        # Current app polls the radio directly; do not read UART or Thread state.
        manifest["radio_status_source"] = "allowlisted thread fields from Matter service status"
        for remote, name, required, limit in FILES:
            present = session.run("if [ -f " + shlex.quote(remote) +
                                  " ]; then printf present; else printf absent; fi", timeout=8).strip()
            if present == "absent" and not required:
                manifest["missing_optional"].append(remote)
                continue
            if present != "present":
                raise BackupError("required backup file missing")
            manifest["files"].append(obtain(remote,name,limit))
        if selected == BASE_APP:
            fallback = directory / "matter-camera"
            active = directory / "matter-camera-active"
            # Hard-link only within this private directory; both labels identify
            # the same already-verified fallback image without a second transfer.
            os.link(fallback, active)
            record = dict(manifest["files"][0], local_file=active.name)
        else:
            record = obtain(selected,"matter-camera-active")
            if record["sha256"] != RELEASE_PATH.fullmatch(selected)[1]:
                raise BackupError("selected release hash does not match its directory")
        manifest["files"].append(record)
        if selector_present:
            manifest["files"].append(obtain(SELECTOR,"app-path",256))
        manifest["after"] = identity(session)
        manifest["status_after"] = safe_status(session)
        if read_selection(session) != (selected, selector_present):
            raise BackupError("app selector changed during backup")
        manifest["app_after"] = app_identity(session, manifest["status_after"], selected)
        if manifest["app_before"] != manifest["app_after"]:
            raise BackupError("running Matter executable changed during backup")
        if manifest["before"] != manifest["after"]:
            raise BackupError("camera rebooted or vendor process changed during backup")
        if manifest["status_before"].get("fabrics") != manifest["status_after"].get("fabrics"):
            raise BackupError("fabric count changed during backup")
        if manifest["status_before"].get("pid") != manifest["status_after"].get("pid"):
            raise BackupError("Matter service restarted during backup")
        manifest["complete"] = True
        private_json(directory / "manifest.json", manifest)
        return directory
    except BaseException as exc:
        # Error class only: arbitrary remote output could contain private text.
        manifest["failure_type"] = type(exc).__name__
        private_json(directory / "manifest.json", manifest)
        raise BackupError("backup incomplete; keep private directory: " + str(directory)) from exc


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("G2H_IP"))
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[2] / "private-g2h-backups")
    parser.add_argument("--reuse-backup",type=Path,help="Reuse verified matching local content; current remote metadata and process lifetime are checked again")
    args = parser.parse_args(argv)
    if not args.host:
        parser.error("provide --host or G2H_IP")
    password = getpass.getpass("Camera root Telnet password: ")
    session = Telnet(args.host)
    try:
        session.login(password)
        password = None
        result = create_backup(session, args.output, args.reuse_backup)
        print("Verified private backup:", result)
    finally:
        password = None
        session.close()


if __name__ == "__main__":
    main()

"""Bounded DK6 protocol core; no Telnet, GPIO or device access on import/run.

The caller must supply an already prepared transport with finite read/write
deadlines. This module deliberately does not enter ISP, reset a radio, stop
processes, or implement erase/write/device-unlock commands.
"""

from __future__ import annotations

import json
import math
import struct
import time
import zlib
from dataclasses import dataclass
from typing import Protocol


DEFAULT_KEY = bytes.fromhex("11223344556677881122334455667788")
MAX_FRAME = 521
MAX_READ = 512
# NXP ROM API access is a bitfield: READ=1, WRITE=2, ERASE=4.
# SPSDK MemoryAccessValues labels use conflicting numeric values; do not copy them.
MEMORIES = frozenset((0, 1, 2, 3, 5))


class Rejected(ValueError):
    pass


class DeviceError(RuntimeError):
    pass


class Transport(Protocol):
    def write_all(self, data: bytes, deadline: float) -> None: ...
    def read_some(self, maximum: int, deadline: float) -> bytes: ...


def validate_command(command: int, payload: bytes) -> None:
    """Fail closed on unknown commands, authentication modes and access modes."""
    if type(command) is not int or type(payload) is not bytes:
        raise Rejected("integer command and immutable bytes required")
    if command == 0x4E:
        if payload not in (b"\x00", b"\x01" + DEFAULT_KEY):
            raise Rejected("device unlock / extended unlock / custom key forbidden")
    elif command == 0x32:
        if payload:
            raise Rejected("chip ID takes no payload")
    elif command == 0x4C:
        if len(payload) != 1 or payload[0] not in MEMORIES:
            raise Rejected("unsupported memory information request")
    elif command == 0x40:
        if len(payload) != 2 or payload[0] not in MEMORIES or payload[1] != 1:
            raise Rejected("memory open must explicitly use READ bit only = 0x01")
    elif command == 0x46:
        if len(payload) != 10:
            raise Rejected("invalid memory read shape")
        _, mode, address, length = struct.unpack("<BBII", payload)
        if mode != 0 or not 1 <= length <= MAX_READ or address + length > 2**32:
            raise Rejected("invalid read mode, size or address")
    elif command == 0x4A:
        if len(payload) != 1:
            raise Rejected("invalid close handle")
    else:
        raise Rejected(f"command 0x{command:02x} is not on the read-only allowlist")


def encode_command(command: int, payload: bytes = b"") -> bytes:
    validate_command(command, payload)
    body = struct.pack(">BHB", 0, len(payload) + 8, command) + payload
    return body + struct.pack(">I", zlib.crc32(body))


def decode_response(frame: bytes, expected_type: int) -> bytes:
    if not 9 <= len(frame) <= MAX_FRAME:
        raise Rejected("invalid response size")
    flag, length, kind = struct.unpack(">BHB", frame[:4])
    if flag != 0 or length != len(frame) or kind != expected_type:
        raise Rejected("unexpected response header")
    if zlib.crc32(frame[:-4]) != int.from_bytes(frame[-4:], "big"):
        raise Rejected("response CRC mismatch")
    status, data = frame[4], frame[5:-4]
    if status:
        raise DeviceError(f"bootloader rejected request: status 0x{status:02x}")
    return data


@dataclass(frozen=True)
class Memory:
    id: int
    base: int
    size: int
    sector: int
    type: int
    access: int
    name_hex: str


class ReadOnlyClient:
    def __init__(self, transport: Transport, timeout: float = 2.0):
        if not math.isfinite(timeout) or not 0 < timeout <= 10:
            raise ValueError("timeout must be finite and at most 10 seconds")
        self.transport = transport
        self.timeout = timeout

    def _exact(self, count: int, deadline: float) -> bytes:
        result = bytearray()
        while len(result) < count:
            if time.monotonic() >= deadline:
                raise TimeoutError("response deadline expired")
            part = self.transport.read_some(count - len(result), deadline)
            if not part or len(part) > count - len(result):
                raise Rejected("transport EOF or oversized read")
            result.extend(part)
        return bytes(result)

    def _request(self, command: int, payload: bytes = b"") -> bytes:
        frame = encode_command(command, payload)  # validate before transport write
        deadline = time.monotonic() + self.timeout
        self.transport.write_all(frame, deadline)
        header = self._exact(4, deadline)
        flag, length, kind = struct.unpack(">BHB", header)
        if flag != 0 or not 9 <= length <= MAX_FRAME or kind != command + 1:
            raise Rejected("response header rejected before allocating payload")
        return decode_response(header + self._exact(length - 4, deadline), command + 1)

    def identify(self) -> dict[str, int]:
        if self._request(0x4E, b"\x00"):
            raise Rejected("unexpected default-session response")
        data = self._request(0x32)
        if len(data) not in (4, 8):
            raise Rejected("unsupported chip identity length")
        result = {"chip_id": int.from_bytes(data[:4], "little")}
        if len(data) == 8:
            result["chip_version"] = int.from_bytes(data[4:], "little")
        return result

    def start_read_session(self) -> None:
        if self._request(0x4E, b"\x01" + DEFAULT_KEY):
            raise Rejected("unexpected start-session response")

    def memory_info(self, memory_id: int) -> Memory:
        if type(memory_id) is not int or memory_id not in MEMORIES:
            raise Rejected("memory ID is not allowed")
        data = self._request(0x4C, bytes((memory_id,)))
        size = struct.calcsize("<BIIIBB")
        if len(data) < size:
            raise Rejected("short memory metadata")
        ident, base, length, sector, kind, access = struct.unpack("<BIIIBB", data[:size])
        if ident != memory_id or length == 0 or base + length > 2**32:
            raise Rejected("invalid memory bounds")
        return Memory(ident, base, length, sector, kind, access, data[size:].hex())

    def read(self, memory_id: int, address: int, length: int) -> bytes:
        """Read at most 4 KiB after independently querying bounds; no whole-chip dump."""
        if type(address) is not int or type(length) is not int:
            raise Rejected("integer address and length required")
        if not 1 <= length <= 4096 or not 0 <= address < 2**32:
            raise Rejected("read outside bounded diagnostic size")
        memory = self.memory_info(memory_id)
        if not memory.base <= address or address + length > memory.base + memory.size:
            raise Rejected("read falls outside reported memory")
        opened = self._request(0x40, bytes((memory_id, 1)))
        if len(opened) != 1:
            raise Rejected("invalid memory handle")
        handle = opened[0]
        result = bytearray()
        try:
            while len(result) < length:
                count = min(MAX_READ, length - len(result))
                data = self._request(0x46, struct.pack("<BBII", handle, 0, address + len(result), count))
                if len(data) != count:
                    raise Rejected("short or oversized memory reply")
                result.extend(data)
        finally:
            if self._request(0x4A, bytes((handle,))):
                raise Rejected("unexpected memory-close response")
        return bytes(result)


if __name__ == "__main__":
    # No device access: a locally inspectable first-probe plan only.
    print(json.dumps({
        "mode": "offline plan only; no transport configured",
        "first_probe": [encode_command(0x4E, b"\x00").hex(), encode_command(0x32).hex()],
        "forbidden": ["reset", "execute", "set baud", "erase", "write", "device unlock", "extended unlock"],
        "limits": {"frame_bytes": MAX_FRAME, "chunk_bytes": MAX_READ, "diagnostic_read_bytes": 4096},
    }, indent=2))

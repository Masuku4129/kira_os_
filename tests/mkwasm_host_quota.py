#!/usr/bin/env python3
"""Generate valid WASM modules for host-call, output and sleep quota tests."""
import sys
from pathlib import Path


def uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7f
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def sleb(value):
    out = bytearray()
    more = True
    while more:
        byte = value & 0x7f
        value >>= 7
        sign = byte & 0x40
        more = not ((value == 0 and not sign) or (value == -1 and sign))
        out.append(byte | (0x80 if more else 0))
    return bytes(out)


def section(kind, payload):
    return bytes([kind]) + uleb(len(payload)) + payload


def write_loop(length):
    types = bytes([2, 0x60, 2, 0x7f, 0x7f, 1, 0x7f, 0x60, 0, 0])
    imports = bytes([1, 4]) + b"kira" + bytes([5]) + b"write" + bytes([0, 0])
    functions = bytes([1, 1])
    memory = bytes([1, 1, 1, 1])
    exports = bytes([1, 4]) + b"main" + bytes([0, 1])
    body = bytes([0, 3, 0x40, 0x41, 0, 0x41]) + uleb(length) + bytes([0x10, 0, 0x1a, 0x0c, 0, 0x0b, 0x0b])
    code = bytes([1]) + uleb(len(body)) + body
    data_bytes = b"x" * length
    data = bytes([1, 0, 0x41, 0, 0x0b]) + uleb(len(data_bytes)) + data_bytes
    return (b"\x00asm\x01\x00\x00\x00" + section(1, types) +
            section(2, imports) + section(3, functions) + section(5, memory) +
            section(7, exports) + section(10, code) + section(11, data))


def sleep_once(ticks):
    types = bytes([2, 0x60, 1, 0x7f, 0, 0x60, 0, 0])
    imports = bytes([1, 4]) + b"kira" + bytes([5]) + b"sleep" + bytes([0, 0])
    functions = bytes([1, 1])
    exports = bytes([1, 4]) + b"main" + bytes([0, 1])
    body = bytes([0, 0x41]) + sleb(ticks) + bytes([0x10, 0, 0x0b])
    code = bytes([1]) + uleb(len(body)) + body
    return (b"\x00asm\x01\x00\x00\x00" + section(1, types) +
            section(2, imports) + section(3, functions) +
            section(7, exports) + section(10, code))


if len(sys.argv) != 2:
    raise SystemExit("usage: mkwasm_host_quota.py OUTPUT_DIRECTORY")
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
(out / "wasm_output_quota.wasm").write_bytes(write_loop(4096))
(out / "wasm_host_call_quota.wasm").write_bytes(write_loop(1))
(out / "wasm_sleep_quota.wasm").write_bytes(sleep_once(60001))

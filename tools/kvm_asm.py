#!/usr/bin/env python3
"""Experimental assembler for the current KVP0 v1 bytecode ABI.

This emits the 32-byte KVP0 container implemented by kvm/kvm.c. The format is
not yet the frozen public KIRA VM ABI; keep generated modules versioned and
rebuild them if the ABI decision gate selects a different container.
"""
from __future__ import annotations

import argparse
import ast
import re
import struct
from pathlib import Path

OP = {
    "halt": 0, "const": 1, "mov": 2, "add": 3, "sub": 4, "mul": 5,
    "udiv": 6, "umod": 7, "and": 8, "or": 9, "xor": 10, "shl": 11,
    "shr": 12, "load8": 13, "load16": 14, "load32": 15, "load64": 16,
    "store8": 17, "store16": 18, "store32": 19, "store64": 20,
    "jmp": 21, "jz": 22, "jnz": 23, "call": 24, "ret": 25, "syscall": 26,
}
CAP = {"console": 0, "ticks": 1, "yield": 2}
REG = re.compile(r"r(1[0-5]|[0-9])\Z")
LABEL = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
MAX_MODULE = 16 * 1024 * 1024
MAX_PAGES = 4096


def _strip_comment(line: str) -> str:
    quote = None
    escaped = False
    for i, ch in enumerate(line):
        if quote:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = None
        elif ch in ("'", '"'):
            quote = ch
        elif ch in ("#", ";"):
            return line[:i]
    return line


def _args(text: str) -> list[str]:
    return [part.strip() for part in text.split(",")] if text.strip() else []


def _integer(token: str, what: str) -> int:
    try:
        return int(token, 0)
    except ValueError as exc:
        raise ValueError(f"invalid {what}: {token}") from exc


def _reg(token: str) -> int:
    match = REG.fullmatch(token.strip().lower())
    if not match:
        raise ValueError(f"invalid register: {token}")
    return int(match.group(1))


def _target(token: str, labels: dict[str, int]) -> int:
    name = token.strip()
    if name not in labels:
        raise ValueError(f"unknown label: {name}")
    return labels[name]


def _signed32(value: int, what: str) -> int:
    if value < -(1 << 31) or value > (1 << 31) - 1:
        raise ValueError(f"{what} is outside signed 32-bit range")
    return value


def assemble(source: str) -> bytes:
    instructions: list[tuple[int, int, int, int, int, str | None]] = []
    labels: dict[str, int] = {}
    data = bytearray()
    memory_pages = None
    entry_label = None
    caps = 0
    in_data = False

    for line_no, original in enumerate(source.splitlines(), 1):
        line = _strip_comment(original).strip()
        if not line:
            continue
        try:
            label_match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):(?:\s*(.*))?$", line)
            if label_match:
                label, rest = label_match.group(1), label_match.group(2) or ""
                if in_data:
                    raise ValueError("labels are only allowed in code")
                if label in labels:
                    raise ValueError(f"duplicate label: {label}")
                labels[label] = len(instructions)
                line = rest.strip()
                if not line:
                    continue

            if line == ".data":
                if in_data:
                    raise ValueError(".data may appear only once")
                in_data = True
                continue
            if line.startswith(".memory "):
                if memory_pages is not None or in_data:
                    raise ValueError(".memory must appear once before .data")
                memory_pages = _integer(line[8:].strip(), "memory page count")
                if not 1 <= memory_pages <= MAX_PAGES:
                    raise ValueError(f"memory pages must be 1..{MAX_PAGES}")
                continue
            if line.startswith(".caps "):
                if in_data:
                    raise ValueError(".caps must appear before .data")
                for name in _args(line[6:]):
                    name = name.lower()
                    if name not in CAP:
                        raise ValueError(f"unknown capability: {name}")
                    caps |= 1 << CAP[name]
                continue
            if line.startswith(".entry "):
                if in_data or entry_label is not None:
                    raise ValueError(".entry must appear once before .data")
                entry_label = line[7:].strip()
                if not LABEL.fullmatch(entry_label):
                    raise ValueError(f"invalid entry label: {entry_label}")
                continue
            if line.startswith(".ascii "):
                if not in_data:
                    raise ValueError(".ascii requires .data")
                value = ast.literal_eval(line[7:].strip())
                if not isinstance(value, str):
                    raise ValueError(".ascii expects a quoted string")
                data.extend(value.encode("utf-8"))
                continue
            if line.startswith(".byte "):
                if not in_data:
                    raise ValueError(".byte requires .data")
                values = _args(line[6:])
                if not values:
                    raise ValueError(".byte requires at least one value")
                for token in values:
                    value = _integer(token, "byte")
                    if not 0 <= value <= 255:
                        raise ValueError(".byte values must be 0..255")
                    data.append(value)
                continue
            if line.startswith(".zero "):
                if not in_data:
                    raise ValueError(".zero requires .data")
                count = _integer(line[6:].strip(), "zero count")
                if not 0 <= count <= MAX_MODULE:
                    raise ValueError(".zero count is outside module limit")
                data.extend(bytes(count))
                continue
            if in_data:
                raise ValueError("instructions are not allowed after .data")

            parts = line.split(None, 1)
            mnemonic = parts[0].lower()
            if mnemonic not in OP:
                raise ValueError(f"unknown instruction: {mnemonic}")
            operands = _args(parts[1] if len(parts) == 2 else "")
            instructions.append((OP[mnemonic], 0, 0, 0, 0, None))
            # Store unresolved operands separately; labels are resolved after pass 1.
            instructions[-1] = (OP[mnemonic], 0, 0, 0, 0, "|".join(operands))
        except (ValueError, SyntaxError) as exc:
            raise ValueError(f"line {line_no}: {exc}") from exc

    if memory_pages is None:
        raise ValueError("missing .memory directive")
    if not instructions:
        raise ValueError("module must contain at least one instruction")

    code = bytearray()
    for opcode, _, _, _, _, raw in instructions:
        operands = raw.split("|") if raw else []
        dst = a = b = imm = 0
        name = next(key for key, value in OP.items() if value == opcode)
        try:
            if name in ("halt", "ret"):
                if operands:
                    raise ValueError(f"{name} takes no operands")
            elif name == "const":
                if len(operands) != 2:
                    raise ValueError("const expects: rD, immediate")
                dst, imm = _reg(operands[0]), _signed32(_integer(operands[1], "immediate"), "immediate")
            elif name == "mov":
                if len(operands) != 2:
                    raise ValueError("mov expects: rD, rA")
                dst, a = _reg(operands[0]), _reg(operands[1])
            elif name in ("add", "sub", "mul", "udiv", "umod", "and", "or", "xor", "shl", "shr"):
                if len(operands) != 3:
                    raise ValueError(f"{name} expects: rD, rA, rB")
                dst, a, b = _reg(operands[0]), _reg(operands[1]), _reg(operands[2])
            elif name.startswith("load"):
                if len(operands) != 3:
                    raise ValueError(f"{name} expects: rD, rA, offset")
                dst, a = _reg(operands[0]), _reg(operands[1])
                imm = _signed32(_integer(operands[2], "offset"), "offset")
            elif name.startswith("store"):
                if len(operands) != 3:
                    raise ValueError(f"{name} expects: rD, rA, offset")
                dst, a = _reg(operands[0]), _reg(operands[1])
                imm = _signed32(_integer(operands[2], "offset"), "offset")
            elif name == "jmp" or name == "call":
                if len(operands) != 1:
                    raise ValueError(f"{name} expects: label")
                imm = _signed32(_target(operands[0], labels), "branch target")
            elif name in ("jz", "jnz"):
                if len(operands) != 2:
                    raise ValueError(f"{name} expects: rA, label")
                a = _reg(operands[0])
                imm = _signed32(_target(operands[1], labels), "branch target")
            elif name == "syscall":
                if len(operands) != 2:
                    raise ValueError("syscall expects: number, capability")
                imm = _signed32(_integer(operands[0], "syscall number"), "syscall number")
                cap_name = operands[1].lower()
                if cap_name.startswith("cap") and cap_name[3:].isdigit():
                    b = int(cap_name[3:])
                elif cap_name in CAP:
                    b = CAP[cap_name]
                else:
                    raise ValueError(f"unknown capability: {cap_name}")
                if imm < 0 or not 0 <= b < 64:
                    raise ValueError("invalid syscall number or capability bit")
                caps |= 1 << b
            code.extend(struct.pack("<BBBBi", opcode, dst, a, b, imm))
        except (ValueError, struct.error) as exc:
            raise ValueError(str(exc)) from exc

    entry = _target(entry_label, labels) if entry_label else 0
    if entry >= len(instructions):
        raise ValueError("entry target is outside code")
    if len(data) > MAX_MODULE or len(code) + len(data) + 32 > MAX_MODULE:
        raise ValueError("module exceeds 16 MiB")
    header = struct.pack("<4sHHIIIIQ", b"KVP0", 0, 32, len(code), len(data),
                         memory_pages, entry, caps)
    return header + code + data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        blob = assemble(args.source.read_text(encoding="utf-8"))
        args.output.write_bytes(blob)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"KVP0: {len(blob)} bytes -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

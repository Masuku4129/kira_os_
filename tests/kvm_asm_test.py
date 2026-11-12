#!/usr/bin/env python3
import importlib.util
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("kvm_asm", ROOT / "tools" / "kvm_asm.py")
ASM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ASM)


def expect_error(source, fragment):
    try:
        ASM.assemble(source)
    except ValueError as exc:
        assert fragment in str(exc), (fragment, str(exc))
    else:
        raise AssertionError(f"expected assembler failure containing {fragment!r}")


def main():
    hello = r'''
.memory 1
.caps console
.entry start
start:
    const r0, 0
    const r1, 6
    syscall 0, console
    halt
.data
.ascii "hello:"
'''
    blob = ASM.assemble(hello)
    version, header, code_len, data_len, pages, entry, caps = struct.unpack_from("<HHIIIIQ", blob, 4)
    assert blob[:4] == b"KVP0"
    assert (version, header, code_len, data_len, pages, entry) == (0, 32, 32, 6, 1, 0)
    assert caps == 1
    assert blob[32 + code_len:] == b"hello:"
    assert len(blob) == 32 + code_len + data_len

    branch = '''
.memory 1
.entry main
main:
    jmp done
    const r0, 99
done:
    halt
'''
    b = ASM.assemble(branch)
    assert struct.unpack_from("<i", b, 32 + 4)[0] == 2

    expect_error(".entry start\nstart: halt", "missing .memory")
    expect_error(".memory 1\nfoo: halt\nfoo: halt", "duplicate label")
    expect_error(".memory 1\njmp nowhere", "unknown label")
    expect_error(".memory 1\nconst r16, 1", "invalid register")
    expect_error(".memory 1\n.data\n.ascii 123", ".ascii expects a quoted string")
    expect_error(".memory 1\n.data\n.ascii \"unterminated", "unterminated string literal")
    expect_error(".memory 1\n.caps network\nhalt", "unknown capability")
    expect_error(".memory 1\nsyscall 0, cap64", "invalid syscall number or capability")
    expect_error(".memory 0\nhalt", "memory pages must be")
    print("KVM assembler: header, data, labels, capabilities and rejection cases passed")


if __name__ == "__main__":
    main()

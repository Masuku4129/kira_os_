#!/usr/bin/env python3
"""Negative tests for the untrusted WebAssembly module header/section envelope."""
import os
import pathlib
import subprocess
import sys
import tempfile

host = sys.argv[1]
test_env = os.environ.copy()
test_env["WASM_NOJIT"] = "1"  # exercise the portable interpreter, as on ARM64
magic = b"\x00asm\x01\x00\x00\x00"
cases = [
    ("nonzero-version-reserved-byte", magic[:7] + b"\x01", "not a WebAssembly 1.0 module"),
    ("section-length-past-end", magic + b"\x01\x7f", "section past end"),
    ("maximum-section-length-past-end", magic + b"\x01\xff\xff\xff\xff\x0f", "section past end"),
    ("truncated-section-length-leb", magic + b"\x01\x80", "bad unsigned LEB128"),
    ("custom-section-truncated-name-length", magic + b"\x00\x01\x80",
     "bad unsigned LEB128"),
    ("custom-section-name-past-end", magic + b"\x00\x02\x02x",
     "truncated custom section name"),
    ("custom-section-invalid-utf8-name", magic + b"\x00\x03\x02\xc0\x80",
     "invalid UTF-8 custom section name"),
    ("custom-section-truncated-utf8-name", magic + b"\x00\x03\x02\xe2\x82",
     "invalid UTF-8 custom section name"),
    ("custom-section-surrogate-utf8-name", magic + b"\x00\x04\x03\xed\xa0\x80",
     "invalid UTF-8 custom section name"),
    ("custom-section-out-of-range-utf8-name", magic + b"\x00\x05\x04\xf4\x90\x80\x80",
     "invalid UTF-8 custom section name"),
    ("custom-section-invalid-continuation-utf8-name", magic + b"\x00\x03\x02\xc2x",
     "invalid UTF-8 custom section name"),
    ("import-module-invalid-utf8-name",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x02\x0c\x01\x02\xc0\x80\x05ticks\x00\x00",
     "invalid UTF-8 import module name"),
    ("import-field-invalid-utf8-name",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x02\x0b\x01\x04kira\x02\xc0\x80\x00\x00",
     "invalid UTF-8 import field name"),
    ("export-invalid-utf8-name",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x03\x02\x01\x00" +
     b"\x07\x06\x01\x02\xc0\x80\x00\x00",
     "invalid UTF-8 export name"),
    ("duplicate-type-section", magic + b"\x01\x01\x00" + b"\x01\x01\x00",
     "duplicate WebAssembly section"),
    ("sections-out-of-order", magic + b"\x02\x01\x00" + b"\x01\x01\x00",
     "WebAssembly sections out of order"),
    ("invalid-function-value-type",
     magic + b"\x01\x05\x01\x60\x01\x70\x00",
     "unsupported parameter value type"),
    ("truncated-function-type-vector",
     magic + b"\x01\x03\x01\x60\x01",
     "bad type parameter vector"),
    ("KIRA-import-signature-mismatch",
     magic + b"\x01\x06\x01\x60\x01\x7e\x01\x7f" +
     b"\x02\x0e\x01\x04kira\x05ticks\x00\x00",
     "bad KIRA import signature"),
    ("memory-maximum-below-minimum",
     magic + b"\x05\x04\x01\x01\x02\x01",
     "invalid or excessive memory limits"),
    ("memory-grow-without-memory",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x08\x01\x06\x00\x41\x01\x40\x00\x0b",
     "memory size/grow without memory"),
    ("constant-expression-global-index-out-of-range",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" +
     b"\x06\x06\x01\x7f\x00\x23\x00\x0b" +
     b"\x07\x09\x01\x05kmain\x00\x00",
     "constant expression global index out of range"),
    ("constant-expression-local-global-reference",
     magic + b"\x06\x0b\x02" +
     b"\x7f\x00\x41\x00\x0b" +
     b"\x7f\x00\x23\x00\x0b",
     "constant expression references unsupported local global"),
    ("constant-expression-global-type-mismatch",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" +
     b"\x06\x06\x01\x7f\x00\x42\x00\x0b" +
     b"\x07\x09\x01\x05kmain\x00\x00",
     "constant expression type mismatch"),
    ("constant-expression-data-offset-type-mismatch",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" + b"\x05\x03\x01\x00\x01" +
     b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x04\x01\x02\x00\x0b" +
     b"\x0b\x09\x01\x00\x43\x00\x00\x00\x00\x0b\x00",
     "constant expression type mismatch"),
    ("section-size-u32-overflow",
     magic + b"\x01\xff\xff\xff\xff\x10",
     "bad unsigned LEB128"),
    ("section-size-u32-too-long",
     magic + b"\x01\x80\x80\x80\x80\x80\x00",
     "bad unsigned LEB128"),
    ("function-i32-const-signed-overflow",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x0a\x01\x08\x00\x41\x80\x80\x80\x80\x08\x0b",
     "bad signed LEB128"),
    ("function-i32-const-truncated-signed-leb",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x05\x01\x03\x00\x41\x80",
     "bad signed LEB128"),
    ("function-i64-const-overflow",
     magic + b"\x01\x05\x01\x60\x00\x01\x7e" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x0f\x01\x0d\x00\x42" +
     b"\x80\x80\x80\x80\x80\x80\x80\x80\x80\x02\x0b",
     "bad signed LEB128"),
    ("unreachable-function-signed-immediate-overflow",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x0b\x01\x09\x00\x00\x41\x80\x80\x80\x80\x08\x0b",
     "bad signed LEB128"),
    ("unreachable-unknown-opcode",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x06\x01\x04\x00\x00\xff\x0b",
     "unsupported instruction in unreachable code"),
    ("call-indirect-nonzero-table-index",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x09\x01\x07\x00\x41\x00\x11\x00\x01\x0b",
     "unsupported call_indirect table index"),
    ("memory-size-nonzero-memory-index",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x03\x02\x01\x00" + b"\x05\x03\x01\x00\x00" +
     b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x06\x01\x04\x00\x3f\x01\x0b",
     "unsupported memory index immediate"),
    ("typed-select-multiple-result-types",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x0e\x01\x0c\x00\x41\x01\x41\x02\x41\x01" +
     b"\x1c\x02\x7f\x7f\x0b",
     "unsupported typed select result type"),
    ("call-indirect-result-type-mismatch",
     magic +
     b"\x01\x09\x02\x60\x00\x01\x7f\x60\x00\x01\x7e" +
     b"\x03\x03\x02\x00\x01" +
     b"\x04\x04\x01\x70\x00\x01" +
     b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x09\x07\x01\x00\x41\x00\x0b\x01\x01" +
     b"\x0a\x0e\x02\x07\x00\x41\x00\x11\x00\x00\x0b" +
     b"\x04\x00\x42\x00\x0b",
     "call_indirect: signature mismatch"),
    ("function-body-past-code-section",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" + b"\x0a\x02\x01\x7f",
     "function body past code section"),
    ("load-alignment-exceeds-natural-width",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" + b"\x05\x03\x01\x00\x01" +
     b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x0a\x01\x08\x00\x41\x00\x28\x03\x00\x1a\x0b",
     "invalid WebAssembly load alignment"),
    ("unreachable-local-index-out-of-range",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x07\x01\x05\x00\x00\x20\x01\x0b",
     "local index out of range in unreachable code"),
    ("unreachable-load-alignment-exceeds-natural-width",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" + b"\x05\x03\x01\x00\x01" +
     b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x08\x01\x06\x00\x00\x28\x03\x00\x0b",
     "memory alignment exceeds natural alignment"),
    ("unreachable-branch-depth-out-of-range",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x06\x01\x04\x00\x00\x0c\x01\x0b",
     "branch depth out of range in unreachable code"),
    ("unreachable-call-indirect-type-out-of-range",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x00" +
     b"\x0a\x07\x01\x05\x00\x00\x11\x01\x00\x0b",
     "call_indirect type index out of range in unreachable code"),
    ("export-function-index-out-of-range",
     magic + b"\x01\x05\x01\x60\x00\x01\x7f" +
     b"\x03\x02\x01\x00" + b"\x07\x09\x01\x05kmain\x00\x01",
     "export index out of range"),
    ("import-function-type-out-of-range",
     magic + b"\x01\x01\x00" + b"\x02\x0e\x01\x04kira\x05ticks\x00\x00",
     "bad imported function type"),
]

with tempfile.TemporaryDirectory(prefix="kira-wasm-bounds-") as td:
    for name, module, expected in cases:
        path = pathlib.Path(td) / (name + ".wasm")
        path.write_bytes(module)
        run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, env=test_env)
        if run.returncode == 0 or expected not in run.stdout:
            print("FAIL:", name, "exit=", run.returncode)
            print(run.stdout)
            raise SystemExit(1)
        print("PASS:", name)


# Unreachable instructions still have to obey the opcode's exact natural alignment.
# Keep a valid exported entry in function 0; function 1 exercises structural validation.
def unreachable_memarg_module(opcode, align):
    body0 = bytes((0, 0x41, 0, 0x0b))
    body1 = bytes((0, 0, opcode, align, 0, 0x0b))
    code_payload = bytes((2, len(body0))) + body0 + bytes((len(body1),)) + body1
    return (magic +
            bytes((1, 5, 1, 0x60, 0, 1, 0x7f)) +
            bytes((3, 3, 2, 0, 0)) +
            bytes((5, 3, 1, 0, 1)) +
            bytes((7, 9, 1, 5)) + b"kmain" + bytes((0, 0)) +
            bytes((0x0a, len(code_payload))) + code_payload)

with tempfile.TemporaryDirectory(prefix="kira-wasm-unreachable-align-") as td:
    natural_alignments = [
        2, 3, 2, 3, 0, 0, 1, 1, 0, 0, 1, 1, 2, 2,
        2, 3, 2, 3, 0, 1, 0, 1, 2,
    ]
    for opcode, natural in enumerate(natural_alignments, start=0x28):
        path = pathlib.Path(td) / f"op-{opcode:02x}-natural.wasm"
        path.write_bytes(unreachable_memarg_module(opcode, natural))
        run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, env=test_env)
        if run.returncode != 0 or "wasm: finished with code 0" not in run.stdout:
            print(f"FAIL: natural alignment {natural} rejected for opcode 0x{opcode:02x}")
            print(run.stdout)
            raise SystemExit(1)
        path = pathlib.Path(td) / f"op-{opcode:02x}-overaligned.wasm"
        path.write_bytes(unreachable_memarg_module(opcode, natural + 1))
        run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, env=test_env)
        if "memory alignment exceeds natural alignment" not in run.stdout:
            print(f"FAIL: over-alignment {natural + 1} accepted for opcode 0x{opcode:02x}")
            print(run.stdout)
            raise SystemExit(1)
    print("PASS: all 23 unreachable load/store natural-alignment boundaries")

# A declared maximum of zero is distinct from an omitted maximum. The module
# must not be silently granted the runtime's default 32 pages.
zero_max_module = (
    magic +
    b"\x01\x05\x01\x60\x00\x01\x7f" +
    b"\x03\x02\x01\x00" +
    b"\x05\x04\x01\x01\x00\x00" +
    b"\x07\x09\x01\x05kmain\x00\x00" +
    b"\x0a\x08\x01\x06\x00\x41\x01\x40\x00\x0b"
)
with tempfile.TemporaryDirectory(prefix="kira-wasm-memory-max-") as td:
    path = pathlib.Path(td) / "zero-max.wasm"
    path.write_bytes(zero_max_module)
    run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, env=test_env)
    if "wasm: finished with code 4294967295" not in run.stdout:
        print("FAIL: explicit zero memory maximum was not enforced")
        print(run.stdout)
        raise SystemExit(1)
    print("PASS: explicit zero memory maximum is enforced")

# A valid 10-byte negative i64 LEB must not shift by the 64-bit width.
i64_min_module = (
    magic +
    b"\x01\x05\x01\x60\x00\x01\x7e" +
    b"\x03\x02\x01\x00" +
    b"\x07\x09\x01\x05kmain\x00\x00" +
    b"\x0a\x0f\x01\x0d\x00\x42" +
    b"\x80\x80\x80\x80\x80\x80\x80\x80\x80\x7f\x0b"
)
with tempfile.TemporaryDirectory(prefix="kira-wasm-i64-leb-") as td:
    path = pathlib.Path(td) / "i64-min.wasm"
    path.write_bytes(i64_min_module)
    run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, env=test_env)
    if run.returncode != 0 or "wasm: finished with code 0" not in run.stdout:
        print("FAIL: valid 10-byte negative i64 LEB was rejected")
        print(run.stdout)
        raise SystemExit(1)
    print("PASS: valid 10-byte negative i64 LEB is accepted")

# A memoryless MVP module is valid and must not receive an implicit 64 KiB memory.
memoryless_module = (
    magic + b"\x00\x05\x02\xc3\xa9\xaa\xbb" +
    b"\x01\x05\x01\x60\x00\x01\x7f" +
    b"\x03\x02\x01\x00" +
    b"\x07\x09\x01\x05kmain\x00\x00" +
    b"\x0a\x06\x01\x04\x00\x41\x00\x0b"
)
with tempfile.TemporaryDirectory(prefix="kira-wasm-memoryless-") as td:
    path = pathlib.Path(td) / "memoryless.wasm"
    path.write_bytes(memoryless_module)
    run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, env=test_env)
    if run.returncode != 0 or "wasm: finished with code 0" not in run.stdout:
        print("FAIL: valid memoryless module was rejected")
        print(run.stdout)
        raise SystemExit(1)
    print("PASS: valid memoryless module runs without implicit memory")

# The 10-byte signed-LEB64 encoding of INT64_MIN must be accepted without
# shifting a 64-bit value by 64 (undefined behavior in C).
signed_i64_min_module = (
    magic +
    b"\x01\x05\x01\x60\x00\x01\x7f" +
    b"\x03\x02\x01\x00" +
    b"\x06\x0f\x01\x7e\x00\x42" + (b"\x80" * 9) + b"\x7f\x0b" +
    b"\x07\x09\x01\x05kmain\x00\x00" +
    b"\x0a\x06\x01\x04\x00\x41\x00\x0b"
)
with tempfile.TemporaryDirectory(prefix="kira-wasm-sleb64-") as td:
    path = pathlib.Path(td) / "sleb64-min.wasm"
    path.write_bytes(signed_i64_min_module)
    run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, env=test_env)
    if run.returncode != 0 or "wasm: finished with code 0" not in run.stdout:
        print("FAIL: valid INT64_MIN constant expression was rejected")
        print(run.stdout)
        raise SystemExit(1)
    print("PASS: signed LEB64 minimum constant is accepted")


# An immutable defined global cannot be modified by global.set. The runtime
# must reject the module during validation, not defer it to execution.
immutable_global_set_module = (
    magic +
    b"\x01\x04\x01\x60\x00\x00" +
    b"\x03\x02\x01\x00" +
    b"\x06\x06\x01\x7f\x00\x41\x00\x0b" +
    b"\x07\x09\x01\x05kmain\x00\x00" +
    b"\x0a\x08\x01\x06\x00\x41\x01\x24\x00\x0b"
)
with tempfile.TemporaryDirectory(prefix="kira-wasm-global-mutability-") as td:
    path = pathlib.Path(td) / "immutable-global-set.wasm"
    path.write_bytes(immutable_global_set_module)
    run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, env=test_env)
    if run.returncode == 0 or "global.set on immutable global" not in run.stdout:
        print("FAIL: immutable global.set was not rejected")
        print(run.stdout)
        raise SystemExit(1)
    print("PASS: immutable global.set is rejected during validation")

# The same mutability rule applies after unreachable: structural validation
# must not let malformed instructions hide behind an unconditional trap.
immutable_global_set_unreachable_module = (
    magic +
    b"\x01\x04\x01\x60\x00\x00" +
    b"\x03\x02\x01\x00" +
    b"\x06\x06\x01\x7f\x00\x41\x00\x0b" +
    b"\x07\x09\x01\x05kmain\x00\x00" +
    b"\x0a\x07\x01\x05\x00\x00\x24\x00\x0b"
)
with tempfile.TemporaryDirectory(prefix="kira-wasm-global-unreachable-") as td:
    path = pathlib.Path(td) / "immutable-global-set-unreachable.wasm"
    path.write_bytes(immutable_global_set_unreachable_module)
    run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, env=test_env)
    if run.returncode == 0 or "global.set on immutable global" not in run.stdout:
        print("FAIL: unreachable immutable global.set was not rejected")
        print(run.stdout)
        raise SystemExit(1)
    print("PASS: unreachable immutable global.set is rejected")

# Mutable globals remain writable; this is a positive regression case.
mutable_global_set_module = (
    magic +
    b"\x01\x04\x01\x60\x00\x00" +
    b"\x03\x02\x01\x00" +
    b"\x06\x06\x01\x7f\x01\x41\x00\x0b" +
    b"\x07\x09\x01\x05kmain\x00\x00" +
    b"\x0a\x08\x01\x06\x00\x41\x01\x24\x00\x0b"
)
with tempfile.TemporaryDirectory(prefix="kira-wasm-global-mutable-") as td:
    path = pathlib.Path(td) / "mutable-global-set.wasm"
    path.write_bytes(mutable_global_set_module)
    run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, env=test_env)
    if run.returncode != 0 or "wasm: finished with code 0" not in run.stdout:
        print("FAIL: valid mutable global.set was rejected")
        print(run.stdout)
        raise SystemExit(1)
    print("PASS: mutable global.set remains supported")

# Truncated block types and floating constants must be rejected without
# reading beyond the function body's one-past-end pointer.
truncated_immediate_modules = [
    ("block-type", b"\x0a\x04\x01\x02\x00\x02", "truncated block type"),
    ("f32-constant", b"\x0a\x06\x01\x04\x00\x43\x00\x00", "truncated f32.const"),
    ("f64-constant", b"\x0a\x07\x01\x05\x00\x44\x00\x00\x00", "truncated f64.const"),
]
for name, code_section, expected in truncated_immediate_modules:
    module = (
        magic +
        b"\x01\x04\x01\x60\x00\x00" +
        b"\x03\x02\x01\x00" +
        b"\x07\x09\x01\x05kmain\x00\x00" +
        code_section
    )
    with tempfile.TemporaryDirectory(prefix="kira-wasm-truncated-imm-") as td:
        path = pathlib.Path(td) / (name + ".wasm")
        path.write_bytes(module)
        run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, env=test_env)
        if run.returncode == 0 or expected not in run.stdout:
            print("FAIL: truncated " + name + " immediate was not rejected")
            print(run.stdout)
            raise SystemExit(1)
        print("PASS: truncated " + name + " immediate is rejected")

print("wasm loader bounds: all malformed envelopes rejected")

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
    magic +
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

print("wasm loader bounds: all malformed envelopes rejected")

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
    ("truncated-section-length-leb", magic + b"\x01\x80", "bad LEB128"),
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
     magic + b"\x06\x06\x01\x7f\x00\x23\x00\x0b",
     "constant expression global index out of range"),
    ("function-body-past-code-section",
     magic + b"\x01\x04\x01\x60\x00\x00" +
     b"\x03\x02\x01\x00" + b"\x0a\x02\x01\x7f",
     "function body past code section"),
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

print("wasm loader bounds: all malformed envelopes rejected")

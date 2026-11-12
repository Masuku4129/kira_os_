#!/usr/bin/env python3
"""Negative tests for the untrusted WebAssembly module header/section envelope."""
import pathlib
import subprocess
import sys
import tempfile

host = sys.argv[1]
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
    ("import-function-type-out-of-range",
     magic + b"\x01\x01\x00" + b"\x02\x0e\x01\x04kira\x05ticks\x00\x00",
     "bad imported function type"),
]

with tempfile.TemporaryDirectory(prefix="kira-wasm-bounds-") as td:
    for name, module, expected in cases:
        path = pathlib.Path(td) / (name + ".wasm")
        path.write_bytes(module)
        run = subprocess.run([host, str(path)], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
        if run.returncode == 0 or expected not in run.stdout:
            print("FAIL:", name, "exit=", run.returncode)
            print(run.stdout)
            raise SystemExit(1)
        print("PASS:", name)

print("wasm loader bounds: all malformed envelopes rejected")

#!/usr/bin/env python3
"""Write a tiny valid WASM module whose exported main loops forever."""
import sys
from pathlib import Path

module = bytes.fromhex(
    "0061736d01000000"          # magic + version
    "010401600000"              # type: () -> ()
    "03020100"                  # one function, type 0
    "070801046d61696e0000"      # export main, function 0
    "0a0901070003400c000b0b"    # body: loop { br 0 }
)
if len(sys.argv) != 2:
    raise SystemExit("usage: mkwasm_fuel_loop.py OUTPUT.wasm")
Path(sys.argv[1]).write_bytes(module)

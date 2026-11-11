#!/usr/bin/env python3
"""Generate a disposable test package and sign it through the production CLI."""
import os
import subprocess
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: kdrv_manifest_fixture.py OUTPUT_DIR")
root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
package = out / "package.kdrv"
seed = out / "test-only.seed"
manifest = out / "manifest.bin"
package.write_bytes(bytes(range(32)))
# RFC 8032 test vector seed only; it is public test material, never a release key.
seed.write_bytes(bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"))
os.chmod(seed, 0o600)
tool = root / "tools" / "kdrvsign.py"
subprocess.run([sys.executable, str(tool), "--package", str(package), "--key", str(seed),
                "--version", "7", "--abi", "1", "--output", str(manifest)],
               check=True, stdout=subprocess.DEVNULL)
bad_seed = out / "bad.seed"
bad_seed.write_bytes(b"short")
os.chmod(bad_seed, 0o600)
bad_output = out / "bad.manifest"
p = subprocess.run([sys.executable, str(tool), "--package", str(package), "--key", str(bad_seed),
                    "--version", "7", "--output", str(bad_output)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
if p.returncode == 0 or bad_output.exists():
    raise SystemExit("kdrvsign accepted malformed private key or left partial output")

#!/usr/bin/env python3
"""Generate signer fixtures and exercise bounded input/key-file policy."""
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
tool = root / "tools" / "kdrvsign.py"
package.write_bytes(bytes(range(32)))
# RFC 8032 test vector seed only; it is public test material, never a release key.
seed.write_bytes(bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"))
os.chmod(seed, 0o600)


def invoke(key=seed, pkg=package, output=manifest):
    return subprocess.run(
        [sys.executable, str(tool), "--package", str(pkg), "--key", str(key),
         "--version", "7", "--abi", "1", "--output", str(output)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )


def must_reject(label, key=seed, pkg=package):
    target = out / (label.replace("/", "_") + ".manifest")
    target.unlink(missing_ok=True)
    result = invoke(key=key, pkg=pkg, output=target)
    if result.returncode == 0 or target.exists():
        raise SystemExit(f"kdrvsign accepted invalid {label} or left partial output")


if invoke().returncode != 0 or manifest.stat().st_size != 132:
    raise SystemExit("kdrvsign failed valid RFC test fixture")

for size in (31, 33):
    bad = out / f"seed-{size}.bin"
    bad.write_bytes(b"K" * size)
    os.chmod(bad, 0o600)
    must_reject(f"seed-{size}", key=bad)

insecure = out / "insecure.seed"
insecure.write_bytes(seed.read_bytes())
os.chmod(insecure, 0o644)
if os.name == "posix":
    must_reject("insecure-key-mode", key=insecure)

oversized = out / "oversized.sparse"
with oversized.open("wb") as stream:
    stream.truncate(64 * 1024 * 1024 + 1)
must_reject("oversized-sparse-package", pkg=oversized)

empty = out / "empty.package"
empty.write_bytes(b"")
must_reject("empty-package", pkg=empty)

directory = out / "package-directory"
directory.mkdir()
must_reject("non-regular-package", pkg=directory)

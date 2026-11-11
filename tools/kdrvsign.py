#!/usr/bin/env python3
"""Create a canonical KDRV v1 Ed25519 manifest. The private seed is supplied externally."""
import argparse
import hashlib
import os
import stat
import struct
import tempfile
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

MAGIC = 0x4D56444B
FORMAT = 1
MANIFEST_SIZE = 132
MAX_PACKAGE_SIZE = 64 * 1024 * 1024
PRIVATE_SEED_SIZE = 32


def read_regular_file(path, label, max_size, exact_size=None, private=False):
    """Open without following symlinks where supported; bound reads before allocation."""
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            raise ValueError(f"{label} must be a regular file")
        if exact_size is not None:
            if info.st_size != exact_size:
                raise ValueError(f"{label} must be exactly {exact_size} bytes")
        elif info.st_size < 1 or info.st_size > max_size:
            raise ValueError(f"{label} must be non-empty and at most {max_size} bytes")
        if private and os.name == "posix" and stat.S_IMODE(info.st_mode) & 0o077:
            raise ValueError("private key file permissions must be 0600 or stricter")
        with os.fdopen(fd, "rb", closefd=False) as source:
            data = source.read((exact_size if exact_size is not None else info.st_size) + 1)
        if len(data) != info.st_size:
            raise ValueError(f"{label} changed while being read")
        if exact_size is not None and len(data) != exact_size:
            raise ValueError(f"{label} must be exactly {exact_size} bytes")
        if len(data) > max_size:
            raise ValueError(f"{label} exceeds the maximum size")
        return data
    finally:
        os.close(fd)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--package", required=True, type=Path)
    ap.add_argument("--key", required=True, type=Path, help="raw 32-byte Ed25519 private seed; never commit it")
    ap.add_argument("--version", required=True, type=int)
    ap.add_argument("--abi", type=int, default=1)
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    if not 1 <= args.version <= 0xFFFFFFFF or not 1 <= args.abi <= 0xFFFFFFFF:
        ap.error("version and ABI must be in 1..2^32-1")
    if args.package.resolve() in (args.key.resolve(), args.output.resolve()) or args.key.resolve() == args.output.resolve():
        ap.error("package, private key and output must be different files")
    try:
        seed = read_regular_file(args.key, "private seed", PRIVATE_SEED_SIZE,
                                 exact_size=PRIVATE_SEED_SIZE, private=True)
        package = read_regular_file(args.package, "package", MAX_PACKAGE_SIZE)
    except (OSError, ValueError) as exc:
        ap.error(str(exc))
    private = Ed25519PrivateKey.from_private_bytes(seed)
    public = private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    key_id = hashlib.sha256(public).digest()[:16]
    package_sha = hashlib.sha256(package).digest()
    prefix = struct.pack("<IHH16s32sIII", MAGIC, FORMAT, MANIFEST_SIZE,
                         key_id, package_sha, args.version, args.abi, 0)
    if len(prefix) != 68:
        raise RuntimeError("internal canonical manifest prefix size mismatch")
    manifest = prefix + private.sign(prefix)
    if len(manifest) != MANIFEST_SIZE:
        raise RuntimeError("internal canonical manifest size mismatch")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=args.output.name + ".", suffix=".tmp", dir=args.output.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(manifest)
            out.flush()
            os.fsync(out.fileno())
        os.chmod(temp_name, 0o644)
        os.replace(temp_name, args.output)
    except BaseException:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass
        raise
    print("KDRV manifest v1 signed")
    print("key_id=" + key_id.hex())
    print("package_sha256=" + package_sha.hex())


if __name__ == "__main__":
    main()

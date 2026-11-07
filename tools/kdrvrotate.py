#!/usr/bin/env python3
"""Create an offline Ed25519 certificate that rotates a KDRV publisher key."""
from __future__ import annotations
import argparse
import getpass
import os
import struct
import sys
import tempfile
from pathlib import Path

DOMAIN = b"KIRA-KDRV-KEYROT-V1\0"
MAGIC = 0x3152544B
FORMAT = 1
SIZE = 160


def raw_hex(path: Path) -> bytes:
    value = bytes.fromhex("".join(path.read_text(encoding="ascii").split()))
    if len(value) != 32 or not any(value):
        raise ValueError(f"{path}: expected a nonzero 32-byte public key in hex")
    return value


def private_key(path: Path):
    from cryptography.hazmat.primitives.serialization import load_pem_private_key
    try:
        key = load_pem_private_key(path.read_bytes(), password=None)
    except TypeError:
        key = load_pem_private_key(path.read_bytes(),
                                   password=getpass.getpass("Old key passphrase: ").encode())
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError("old signing key must be Ed25519")
    return key


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".kdrv-rotate-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
        dfd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    except Exception:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--old-key", required=True, type=Path, help="offline old PKCS#8 PEM key")
    ap.add_argument("--old-public", required=True, type=Path, help="old public key, 32-byte hex")
    ap.add_argument("--old-id", required=True, help="old 16-byte ASCII key ID")
    ap.add_argument("--new-public", required=True, type=Path, help="new public key, 32-byte hex")
    ap.add_argument("--new-id", required=True, help="new 16-byte ASCII key ID")
    ap.add_argument("--generation", required=True, type=int, help="strictly increasing trust generation")
    ap.add_argument("--not-before", required=True, type=int, help="Unix UTC start")
    ap.add_argument("--not-after", required=True, type=int, help="exclusive Unix UTC expiry")
    ap.add_argument("--output", required=True, type=Path)
    a = ap.parse_args()

    old_id, new_id = a.old_id.encode("ascii"), a.new_id.encode("ascii")
    if len(old_id) != 16 or len(new_id) != 16 or old_id == new_id:
        raise ValueError("key IDs must be distinct and exactly 16 ASCII bytes")
    if a.generation <= 0 or a.not_before <= 0 or a.not_after <= a.not_before or a.not_after > 0x7FFFFFFFFFFFFFFF:
        raise ValueError("invalid generation or validity interval")
    old_pub, new_pub = raw_hex(a.old_public), raw_hex(a.new_public)
    key = private_key(a.old_key)
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
    actual = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    if actual != old_pub:
        raise ValueError("old private key does not match old public key")
    msg = DOMAIN + old_id + new_id + new_pub + struct.pack("<QQQ", a.generation, a.not_before, a.not_after)
    sig = key.sign(msg)
    envelope = struct.pack("<IHH16s16s32sQQQ64s", MAGIC, FORMAT, SIZE,
                           old_id, new_id, new_pub, a.generation,
                           a.not_before, a.not_after, sig)
    if len(envelope) != SIZE:
        raise AssertionError("internal transition envelope size mismatch")
    atomic_write(a.output, envelope)
    print(f"Transition certificate: {a.output}")
    print(f"Old key: {a.old_id}; new key: {a.new_id}; generation: {a.generation}")
    print(f"Validity: [{a.not_before}, {a.not_after}) UTC")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"kdrvrotate: {exc}", file=sys.stderr)
        raise SystemExit(1)

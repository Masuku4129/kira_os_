#!/usr/bin/env python3
"""Create an Ed25519-signed KDRV publisher-key transition record."""
from __future__ import annotations
import argparse, getpass, os, re, struct, sys, tempfile
from pathlib import Path

MAGIC, FORMAT, SIZE = 0x3152544B, 1, 160
DOMAIN = b"KIRA-KDRV-KEYROT-V1\0"
MAX_RECORDS = 32
KEY_ID = re.compile(r"^[A-Za-z0-9._-]{16}$")

def raw_public(path: Path) -> bytes:
    value = bytes.fromhex("".join(path.read_text(encoding="ascii").split()))
    if len(value) != 32 or not any(value):
        raise ValueError(f"{path}: expected a nonzero 32-byte public key in hex")
    return value

def load_private(path: Path):
    from cryptography.hazmat.primitives.serialization import load_pem_private_key
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    raw = path.read_bytes()
    try: key = load_pem_private_key(raw, password=None)
    except TypeError: key = load_pem_private_key(raw, password=getpass.getpass("Old key passphrase: ").encode())
    if not isinstance(key, Ed25519PrivateKey): raise ValueError("private key must be Ed25519")
    return key

def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".kdrv-rotate-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.chmod(tmp, 0o644); os.replace(tmp, path)
        dfd = os.open(path.parent, os.O_RDONLY)
        try: os.fsync(dfd)
        finally: os.close(dfd)
    except Exception:
        try: os.unlink(tmp)
        except FileNotFoundError: pass
        raise

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--old-key", required=True, type=Path, help="offline PEM private key for the current trusted publisher")
    ap.add_argument("--old-public", required=True, type=Path, help="current trusted public key as 64 hex digits")
    ap.add_argument("--old-id", required=True)
    ap.add_argument("--new-public", required=True, type=Path, help="new Ed25519 public key as 64 hex digits")
    ap.add_argument("--new-id", required=True)
    ap.add_argument("--generation", required=True, type=int)
    ap.add_argument("--not-before", required=True, type=int)
    ap.add_argument("--not-after", required=True, type=int)
    ap.add_argument("--output", required=True, type=Path, help="the /drv/.rotate journal or a single record file")
    ap.add_argument("--append", action="store_true", help="append atomically to an existing journal")
    a = ap.parse_args()
    for label, value in (("old-id", a.old_id), ("new-id", a.new_id)):
        if not KEY_ID.fullmatch(value): raise ValueError(f"--{label} must be 16 safe ASCII characters")
    if a.old_id == a.new_id: raise ValueError("old and new key IDs must differ")
    if a.generation <= 0 or a.not_before <= 0 or a.not_after <= a.not_before or a.not_after > 0x7FFFFFFFFFFFFFFF:
        raise ValueError("invalid generation or validity interval")
    old_pub, new_pub = raw_public(a.old_public), raw_public(a.new_public)
    key = load_private(a.old_key)
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
    if key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw) != old_pub:
        raise ValueError("old private key does not match --old-public")
    message = DOMAIN + a.old_id.encode("ascii") + a.new_id.encode("ascii") + new_pub + struct.pack("<QQQ", a.generation, a.not_before, a.not_after)
    record = struct.pack("<IHH16s16s32sQQQ64s", MAGIC, FORMAT, SIZE,
                         a.old_id.encode("ascii"), a.new_id.encode("ascii"), new_pub,
                         a.generation, a.not_before, a.not_after, key.sign(message))
    if len(record) != SIZE: raise AssertionError("transition ABI mismatch")
    existing = a.output.read_bytes() if a.append and a.output.exists() else b""
    if len(existing) % SIZE or len(existing) // SIZE >= MAX_RECORDS:
        raise ValueError("existing transition journal is malformed or full")
    if existing and a.generation <= struct.unpack_from("<Q", existing, len(existing)-SIZE+72)[0]:
        raise ValueError("generation must be greater than the last transition in this journal")
    if not a.append and a.output.exists(): raise ValueError("output exists; use --append to preserve the journal")
    atomic_write(a.output, existing + record)
    print(f"Transition record written: generation {a.generation}, {a.old_id} -> {a.new_id}")
    return 0

if __name__ == "__main__":
    try: raise SystemExit(main())
    except Exception as exc:
        print(f"kdrvrotate: {exc}", file=sys.stderr); raise SystemExit(1)

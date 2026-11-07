#!/usr/bin/env python3
"""Create an Ed25519-signed KDRV publisher-key revocation record."""
from __future__ import annotations
import argparse, getpass, os, re, struct, sys, tempfile
from pathlib import Path

MAGIC, FORMAT, SIZE, PREFIX_SIZE = 0x3156524B, 1, 628, 564
MAX_IDS, MAX_RECORDS = 32, 32
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
    except TypeError: key = load_pem_private_key(raw, password=getpass.getpass("Issuer key passphrase: ").encode())
    if not isinstance(key, Ed25519PrivateKey): raise ValueError("private key must be Ed25519")
    return key

def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".kdrv-revoke-", dir=str(path.parent))
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
    ap.add_argument("--issuer-key", required=True, type=Path, help="offline PEM private key for an active trusted publisher")
    ap.add_argument("--issuer-public", required=True, type=Path, help="issuer public key as 64 hex digits")
    ap.add_argument("--issuer-id", required=True)
    ap.add_argument("--revoke", required=True, action="append", help="16-character key ID; repeat for multiple IDs")
    ap.add_argument("--generation", required=True, type=int)
    ap.add_argument("--not-before", required=True, type=int)
    ap.add_argument("--not-after", required=True, type=int)
    ap.add_argument("--output", required=True, type=Path, help="the /drv/.revoke journal or a single record file")
    ap.add_argument("--append", action="store_true", help="append atomically to an existing journal")
    a = ap.parse_args()
    if not KEY_ID.fullmatch(a.issuer_id): raise ValueError("--issuer-id must be 16 safe ASCII characters")
    if not 1 <= len(a.revoke) <= MAX_IDS: raise ValueError(f"provide 1..{MAX_IDS} --revoke IDs")
    if any(not KEY_ID.fullmatch(x) for x in a.revoke): raise ValueError("each revoked key ID must be 16 safe ASCII characters")
    if len(set(a.revoke)) != len(a.revoke): raise ValueError("duplicate revoked key ID")
    if a.issuer_id in a.revoke: raise ValueError("a statement cannot revoke its own issuer")
    if a.generation <= 0 or a.not_before <= 0 or a.not_after <= a.not_before or a.not_after > 0x7FFFFFFFFFFFFFFF:
        raise ValueError("invalid generation or validity interval")
    issuer_pub = raw_public(a.issuer_public)
    key = load_private(a.issuer_key)
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
    if key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw) != issuer_pub:
        raise ValueError("issuer private key does not match --issuer-public")
    ids = b"".join(x.encode("ascii") for x in a.revoke).ljust(MAX_IDS * 16, b"\0")
    prefix = struct.pack("<IHHQQQ16sHH512s", MAGIC, FORMAT, SIZE,
                         a.generation, a.not_before, a.not_after,
                         a.issuer_id.encode("ascii"), len(a.revoke), 0, ids)
    if len(prefix) != PREFIX_SIZE: raise AssertionError("revocation ABI mismatch")
    record = prefix + key.sign(prefix)
    if len(record) != SIZE: raise AssertionError("revocation record size mismatch")
    if a.output.resolve() in (a.issuer_key.resolve(), a.issuer_public.resolve()):
        raise ValueError("output must not overwrite private or public key material")
    existing = a.output.read_bytes() if a.append and a.output.exists() else b""
    if len(existing) % SIZE or len(existing) // SIZE >= MAX_RECORDS:
        raise ValueError("existing revocation journal is malformed or full")
    if existing and a.generation <= struct.unpack_from("<Q", existing, len(existing)-SIZE+8)[0]:
        raise ValueError("generation must be greater than the last revocation in this journal")
    if not a.append and a.output.exists(): raise ValueError("output exists; use --append to preserve the journal")
    atomic_write(a.output, existing + record)
    print(f"Revocation record written: generation {a.generation}; revoked {len(a.revoke)} key(s)")
    return 0

if __name__ == "__main__":
    try: raise SystemExit(main())
    except Exception as exc:
        print(f"kdrvrevoke: {exc}", file=sys.stderr); raise SystemExit(1)

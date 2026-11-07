#!/usr/bin/env python3
"""Create a detached KDRV Ed25519 signature using an offline publisher key."""
from __future__ import annotations

import argparse
import getpass
import hashlib
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

MAGIC = 0x31474953
FORMAT = 1
SIZE = 144
MAX_PACKAGE = (8 << 20) + 136
DOMAIN = b"KIRA-KDRV-SIG-V1"


def load_key(path: Path):
    from cryptography.hazmat.primitives.serialization import load_pem_private_key
    raw = path.read_bytes()
    try:
        key = load_pem_private_key(raw, password=None)
    except TypeError:
        password = getpass.getpass("Passphrase for offline signing key: ").encode()
        key = load_pem_private_key(raw, password=password)
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError("the signing key is not Ed25519")
    return key


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="input .kdrv package")
    parser.add_argument("--key", required=True, type=Path, help="offline PKCS#8 PEM private key")
    parser.add_argument("--public-key", required=True, type=Path, help="trusted raw public key, 32 bytes in hex")
    parser.add_argument("--trust-header", required=True, type=Path, help="header containing KDRV_TRUST_KEY_ID")
    parser.add_argument("--key-id", required=True, help="16-byte ASCII key ID from the trust header")
    parser.add_argument("--not-before", required=True, type=int, help="signature start, Unix UTC seconds")
    parser.add_argument("--not-after", required=True, type=int, help="exclusive expiry, Unix UTC seconds")
    parser.add_argument("--output", type=Path, help="output .sig path (default: package basename with .sig)")
    args = parser.parse_args()

    if len(args.key_id.encode("ascii", "strict")) != 16:
        raise ValueError("--key-id must be exactly 16 ASCII bytes")
    trust_text = args.trust_header.read_text(encoding="utf-8")
    match = re.search(r'^#define KDRV_TRUST_KEY_ID "([^"]{16})"', trust_text, re.MULTILINE)
    if not match or match.group(1) != args.key_id:
        raise ValueError("--key-id does not match the selected trust header")
    if args.not_before <= 0 or args.not_after <= args.not_before or args.not_after > 0x7FFFFFFFFFFFFFFF:
        raise ValueError("invalid signature validity interval")

    package_path = args.package.resolve(strict=True)
    key_path = args.key.resolve(strict=True)
    pub_path = args.public_key.resolve(strict=True)
    output = (args.output if args.output else package_path.with_suffix(".sig")).resolve()
    if output in (package_path, key_path, pub_path):
        raise ValueError("output path must not overwrite package or key material")

    package = package_path.read_bytes()
    if not package or len(package) > MAX_PACKAGE:
        raise ValueError("package size is outside KDRV limits")
    expected = bytes.fromhex("".join(pub_path.read_text(encoding="ascii").split()))
    if len(expected) != 32:
        raise ValueError("trusted public key file must contain exactly 32 bytes in hex")

    key = load_key(key_path)
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
    actual = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    if actual != expected:
        raise ValueError("private key does not match the trusted KIRA public key")

    key_id = args.key_id.encode("ascii")
    digest = hashlib.sha256(package).digest()
    message = DOMAIN + key_id + digest + struct.pack("<QQQ", len(package), args.not_before, args.not_after)
    signature = key.sign(message)
    envelope = struct.pack("<IHH16s32sQQQ64s", MAGIC, FORMAT, SIZE, key_id, digest,
                           len(package), args.not_before, args.not_after, signature)
    if len(envelope) != SIZE:
        raise AssertionError("internal KDRV signature envelope size mismatch")

    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".kdrv-sign-", dir=str(output.parent))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(envelope)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, output)
        dirfd = os.open(str(output.parent), os.O_RDONLY)
        try:
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
    except Exception:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise

    print(f"Signed: {package_path.name}")
    print(f"Publisher key ID: {args.key_id}")
    print(f"Package SHA-256: {digest.hex()}")
    print(f"Validity: [{args.not_before}, {args.not_after}) UTC")
    print(f"Signature: {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"kdrvsign: {exc}", file=sys.stderr)
        raise SystemExit(1)

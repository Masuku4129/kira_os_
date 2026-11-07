import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, NoEncryption, PrivateFormat, PublicFormat


ROOT = Path(__file__).resolve().parents[1]
SEED = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")


class KdrvSignerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.private = Ed25519PrivateKey.from_private_bytes(SEED)
        self.private_path = self.root / "owner.pem"
        self.private_path.write_bytes(self.private.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption()))
        self.public_path = self.root / "owner.hex"
        self.public = self.private.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        self.public_path.write_text(self.public.hex() + "\n", encoding="ascii")
        self.trust_path = self.root / "trust.h"
        self.trust_path.write_text(
            '#define KDRV_TRUST_KEY_ID "TEST-ED25519-001"\n'
            + "static const unsigned char pub[] = {"
            + ",".join(f"0x{x:02x}" for x in self.public) + "};\n",
            encoding="ascii"
        )
        self.package_path = self.root / "demo.kdrv"
        self.package = b"KIRA signer test package"
        self.package_path.write_bytes(self.package)
        self.output_path = self.root / "demo.sig"

    def tearDown(self):
        self.temp.cleanup()

    def run_signer(self, *extra):
        return subprocess.run(
            [sys.executable, str(ROOT / "tools" / "kdrvsign.py"), str(self.package_path),
             "--key", str(self.private_path), "--public-key", str(self.public_path),
             "--trust-header", str(self.trust_path), "--key-id", "TEST-ED25519-001",
             "--not-before", "1700000000", "--not-after", "4102444800",
             "--output", str(self.output_path), *extra],
            cwd=ROOT, text=True, capture_output=True
        )

    def test_creates_canonical_signature_that_verifies(self):
        result = self.run_signer()
        self.assertEqual(result.returncode, 0, result.stderr)
        raw = self.output_path.read_bytes()
        self.assertEqual(len(raw), 144)
        magic, version, size, key_id, digest, package_size, start, end, signature = struct.unpack(
            "<IHH16s32sQQQ64s", raw
        )
        self.assertEqual((magic, version, size), (0x31474953, 1, 144))
        self.assertEqual(key_id, b"TEST-ED25519-001")
        self.assertEqual(digest, hashlib.sha256(self.package).digest())
        self.assertEqual(package_size, len(self.package))
        self.assertEqual((start, end), (1700000000, 4102444800))
        message = b"KIRA-KDRV-SIG-V1" + key_id + digest + struct.pack("<QQQ", package_size, start, end)
        self.private.public_key().verify(signature, message)

    def test_refuses_mismatched_public_trust_anchor(self):
        self.public_path.write_text(("00" * 32) + "\n", encoding="ascii")
        result = self.run_signer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not present", result.stderr)
        self.assertFalse(self.output_path.exists())

    def test_refuses_invalid_validity_window(self):
        result = self.run_signer("--not-after", "1600000000")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output_path.exists())


if __name__ == "__main__":
    unittest.main()

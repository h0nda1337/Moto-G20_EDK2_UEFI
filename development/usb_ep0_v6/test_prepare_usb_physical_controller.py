"""Generator validation with temporary local fixtures; no device access."""
import tempfile
import unittest
from pathlib import Path
import prepare_usb_physical_controller as generator


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / generator.IMAGE
        self.image.parent.mkdir()
        self.image.write_bytes(b'local manifest fixture, not a boot image')
        self.identity = generator.sha(self.image.read_bytes())
        self.row = {'relative_path': generator.IMAGE, 'size': self.image.stat().st_size, 'sha256': self.identity}

    def test_valid_manifest_and_new_identity(self):
        generator.validate_manifest(self.root, [self.row], self.identity)
        generator.validate_identity(self.identity, b'[]')

    def test_consumed_or_safe_identity_rejected(self):
        for identity in (generator.PARENT, generator.SAFE_B):
            with self.subTest(identity=identity), self.assertRaisesRegex(ValueError, 'reuses'):
                generator.validate_identity(identity, b'[]')
        with self.assertRaisesRegex(ValueError, 'already in ledger'):
            generator.validate_identity(self.identity, ('{"candidate_sha256":"' + self.identity.lower() + '"}').encode())

    def test_malformed_identity_rejected(self):
        for identity in ('', '0' * 63, 'G' * 64):
            with self.assertRaisesRegex(ValueError, 'Malformed'):
                generator.validate_identity(identity, b'[]')

    def test_duplicate_and_alias_rejected(self):
        for alias in (self.row, {**self.row, 'relative_path': 'CANDIDATE/../' + generator.IMAGE}):
            with self.assertRaisesRegex(ValueError, 'Duplicate'):
                generator.validate_manifest(self.root, [self.row, alias], self.identity)

    def test_escape_rejected(self):
        with self.assertRaisesRegex(ValueError, 'escapes'):
            generator.validate_manifest(self.root, [{**self.row, 'relative_path': '../outside.img'}], self.identity)

    def test_missing_candidate_rejected(self):
        with self.assertRaisesRegex(ValueError, 'absent'):
            generator.validate_manifest(self.root, [], self.identity)

    def test_modified_candidate_rejected(self):
        self.image.write_bytes(b'changed fixture')
        with self.assertRaisesRegex(ValueError, 'drift'):
            generator.validate_manifest(self.root, [self.row], self.identity)

    def test_identity_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, 'identity mismatch'):
            generator.validate_manifest(self.root, [self.row], '0' * 64)


if __name__ == '__main__':
    unittest.main(verbosity=2)

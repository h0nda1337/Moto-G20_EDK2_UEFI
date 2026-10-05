"""Offline structural/mutation tests. Never builds or writes a candidate image."""
import copy
import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

import package_microsd_candidate as p


class PackagingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.parent, cls.fv = p.baseline_inputs()
        cls.rows = p.parse_fv(cls.fv, 23)
        cls.build = p.DEFAULT_BUILD / 'BUILD/DEBUG_GCCNOLTO/AARCH64'

    def test_verified_baseline_and_no_usb(self):
        self.assertEqual(len(self.rows), 23)
        self.assertIn(p.BDS, [r['guid'] for r in self.rows])
        self.assertFalse(p.USB_GUIDS.intersection(r['guid'] for r in self.rows))

    def test_real_compiled_arm64_pe_headers(self):
        for name, *_ in p.FIXED:
            with self.subTest(module=name):
                self.assertEqual(p.pe_audit((self.build / (name + '.efi')).read_bytes())['machine'], 'AA64')

    def test_pe_architecture_rejected(self):
        data = bytearray((self.build / 'MotoG20SdDxe.efi').read_bytes())
        header = struct.unpack_from('<I', data, 0x3c)[0]
        struct.pack_into('<H', data, header + 4, 0x8664)
        with self.assertRaisesRegex(ValueError, 'ARM64'):
            p.pe_audit(data)

    def test_pe_subsystem_rejected(self):
        data = bytearray((self.build / 'MotoG20SdDxe.efi').read_bytes())
        header = struct.unpack_from('<I', data, 0x3c)[0]
        struct.pack_into('<H', data, header + 24 + 68, 10)
        with self.assertRaisesRegex(ValueError, 'driver header'):
            p.pe_audit(data)

    def test_pe_truncation_rejected(self):
        data = (self.build / 'MotoG20SdDxe.efi').read_bytes()
        with self.assertRaises(ValueError):
            p.pe_audit(data[:len(data) // 2])

    def test_fv_header_checksum_rejected(self):
        data = bytearray(self.fv)
        data[16] ^= 1
        with self.assertRaisesRegex(ValueError, 'FV header checksum'):
            p.parse_fv(data)

    def test_fv_ffs_state_rejected(self):
        data = bytearray(self.fv)
        data[self.rows[0]['offset'] + 23] = 7
        with self.assertRaisesRegex(ValueError, 'data-valid'):
            p.parse_fv(data)

    def test_fv_free_space_contamination_rejected(self):
        data = bytearray(self.fv)
        data[-1] = 0
        with self.assertRaisesRegex(ValueError, 'free-space'):
            p.parse_fv(data)

    def test_fv_wrong_count_rejected(self):
        with self.assertRaisesRegex(ValueError, 'file count'):
            p.parse_fv(self.fv, 28)

    def test_missing_provider_blocks_before_build_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'Missing platform provider'):
                p.build_inputs(Path(directory), '03', 'MotoG20Pkg/Drivers/Missing/Missing.inf')

    def test_provider_path_escape_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, 'outside MotoG20Pkg'):
                p.build_inputs(Path(directory), '03', '../outside.inf')

    def test_image_baseline_transport_valid(self):
        transport = self.parent[p.TRANSPORT_OFFSET:p.TRANSPORT_OFFSET + p.TRANSPORT_SIZE]
        template = bytearray(self.parent)
        template[p.IDENTITY] = bytes(32)
        p.verify_image(self.parent, self.parent, transport, template, self.fv)

    def test_image_wrong_size_rejected(self):
        with self.assertRaisesRegex(ValueError, '64 MiB'):
            p.verify_image(self.parent[:-1], self.parent, b'', self.parent, self.fv)

    def test_image_identity_corruption_rejected(self):
        image = bytearray(self.parent)
        image[p.IDENTITY.start] ^= 1
        template = bytearray(self.parent)
        template[p.IDENTITY] = bytes(32)
        transport = self.parent[p.TRANSPORT_OFFSET:p.TRANSPORT_OFFSET + p.TRANSPORT_SIZE]
        with self.assertRaisesRegex(ValueError, 'identity'):
            p.verify_image(image, self.parent, transport, template, self.fv)

    def test_rebound_unapproved_boot_change_rejected(self):
        template = bytearray(self.parent)
        template[p.IDENTITY] = bytes(32)
        template[0x50000] ^= 1
        image = bytearray(template)
        image[p.IDENTITY] = hashlib.sha256(template).digest()
        transport = self.parent[p.TRANSPORT_OFFSET:p.TRANSPORT_OFFSET + p.TRANSPORT_SIZE]
        with self.assertRaisesRegex(ValueError, 'Unapproved boot image delta'):
            p.verify_image(image, self.parent, transport, template, self.fv)

    def test_fv_changes_cannot_skip_module_contract(self):
        with self.assertRaisesRegex(ValueError, 'Required six modules'):
            p.verify_fv(self.fv, self.fv, {})


if __name__ == '__main__':
    unittest.main(verbosity=2)

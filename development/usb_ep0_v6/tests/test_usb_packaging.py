"""In-memory positive/negative packaging contracts. Never writes a boot image.

The fixture uses accepted V6 BDS_b01/HC_h01 and inherited generic module bytes.
All generated FV/image data stays in RAM; no boot image is written or executed.
"""
import contextlib
import io
import json
import pathlib
import struct
import sys
import unittest
import uuid

sys.dont_write_bytecode = True
HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE.parent))
import package_usb_candidate as p


def section_bytes(sections):
    data = bytearray()
    for index, section in enumerate(sections):
        payload = section['data']
        data.extend((len(payload) + 4).to_bytes(3, 'little') + bytes([section['type']]) + payload)
        if index != len(sections) - 1:
            data.extend(bytes(p.align(len(data), 4) - len(data)))
    return bytes(data)


def ffs(guid, sections, file_type=7):
    data = section_bytes(sections)
    header = bytearray(24)
    header[:16] = uuid.UUID(guid).bytes_le
    header[17] = -sum(data) & 0xff
    header[18] = file_type
    header[19] = 0x40
    header[20:23] = (len(data) + 24).to_bytes(3, 'little')
    header[23] = 0xf8
    header[16] = (-sum(header) + header[17] + header[23]) & 0xff
    return bytes(header) + data


class ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.baseline = (p.PARENT / 'FV/MOTOG20FOUNDATIONFV.Fv').read_bytes()
        cls.parent = (p.PARENT / 'CANDIDATE/MOTO_G20_CONSOLE_FLAT_GOP_HOST_ONLY.img').read_bytes()
        cls.rows = p.parse_fv(cls.baseline, 23)
        cls.modules = {}
        runs = ((p.BDS, p.ROOT, 'BDS_b01'), (p.HC, p.ROOT, 'HC_h01'),
                (p.BUS, p.GENERIC_ROOT, 'UsbBusDxe_bus01'),
                (p.KB, p.GENERIC_ROOT, 'UsbKbDxe_kb01'))
        for guid, root, run in runs:
            report = json.loads((root / 'REPORTS' / run / 'BUILD_AUDIT.json').read_text())
            if report.get('status') != 'PASS_HOST_COMPILATION_AND_PE_STATIC_CHECKS':
                raise ValueError('Fixture requires accepted final build ' + run)
            artifact = report['artifacts'][0]
            raw = p.checked_file(pathlib.Path(artifact['evidence_path']), artifact['size'], artifact['sha256'])
            depex = bytes.fromhex(report['depex']['hex']) if guid in (p.BDS, p.HC) else None
            cls.modules[guid] = {'pe': raw, 'depex': depex}
        cls.modules[p.KB]['depex'] = (p.WORK.parent / 'usb_generic_build/depex_review/UsbKbDxe.arch_and_hii.depex').read_bytes()
        cls.sections = {}
        cls.files = []
        for row in cls.rows:
            if row['guid'] != p.BDS:
                cls.files.append(row['raw'])
                continue
            sections = [{'type': item['type'], 'data': cls.modules[p.BDS]['pe'] if item['type'] == 0x10 else
                         cls.modules[p.BDS]['depex'] if item['type'] == 0x13 else item['data']} for item in row['sections']]
            cls.sections[p.BDS] = sections
            cls.files.append(ffs(p.BDS, sections))
        for guid in p.ADDITIONS:
            cls.sections[guid] = p.expected_sections(guid, cls.modules[guid])
            cls.files.append(ffs(guid, cls.sections[guid]))
        cls.fv = cls.volume(cls.files)

    @classmethod
    def volume(cls, files):
        result = bytearray(cls.baseline[:cls.rows[0]['offset']])
        for raw in files:
            result.extend(raw)
            result.extend(b'\xff' * (p.align(len(result), 8) - len(result)))
        result.extend(b'\xff' * (p.FV_SIZE - len(result)))
        return bytes(result)

    def reject_fv(self, files, message):
        with self.assertRaisesRegex(ValueError, message):
            p.verify_fv(self.volume(files), self.baseline, self.modules)

    def test_01_real_parent_and_structurally_valid_26_layout(self):
        self.assertEqual(p.sha(self.baseline), p.PARENT_FV_SHA)
        self.assertEqual(p.sha(self.parent), p.PARENT_SHA)
        # Verify final provenance through the actual V6 input auditor before
        # treating the in-memory layout as a positive structural fixture.
        _, _, accepted, registry = p.inputs('b01', 'h01')
        self.assertEqual(registry['events'], 18)
        for guid, module in self.modules.items():
            self.assertEqual(module['pe'], accepted[guid]['pe'])
            self.assertEqual(module['depex'], accepted[guid]['depex'])
        report = p.verify_fv(self.fv, self.baseline, self.modules)
        self.assertEqual(report['ffs_count'], 26)
        self.assertEqual(len(report['preserved_ffs']), 22)

    def test_02_missing_driver(self):
        self.reject_fv(self.files[:-1], 'file count')

    def test_03_duplicate_guid(self):
        files = self.files.copy()
        files[-1] = ffs(p.BUS, self.sections[p.KB])
        self.reject_fv(files, 'Duplicate FFS GUID')

    def test_04_extra_apriori(self):
        self.reject_fv(self.files + [ffs('FC510EE7-FFDC-11D4-BD41-0080C73C8881', [{'type': 0x19, 'data': uuid.UUID(p.KB).bytes_le}], 1)], 'file count')

    def test_05_reordered_modules(self):
        files = self.files.copy()
        files[-1], files[-2] = files[-2], files[-1]
        self.reject_fv(files, 'GUID order/set')

    def test_06_changed_preserved_gop(self):
        files = self.files.copy()
        index = next(i for i, row in enumerate(self.rows) if row['guid'] == '50F9E65A-1C38-4D77-AB1D-830EA33AA1C0')
        row = self.rows[index]
        sections = [dict(item) for item in row['sections']]
        sections[0]['data'] = sections[0]['data'] + bytes(4)
        files[index] = ffs(row['guid'], sections)
        self.reject_fv(files, 'Unapproved parent FFS change')

    def test_07_stale_bds_depex(self):
        files = self.files.copy()
        sections = [dict(item) for item in self.sections[p.BDS]]
        next(item for item in sections if item['type'] == 0x13)['data'] = b'\x06\x08'
        index = next(i for i, row in enumerate(self.rows) if row['guid'] == p.BDS)
        files[index] = ffs(p.BDS, sections)
        self.reject_fv(files, 'BDS section payload mismatch')

    def test_08_keyboard_hii_only_is_rejected(self):
        files = self.files.copy()
        sections = [dict(item) for item in self.sections[p.KB]]
        sections[0]['data'] = b'\x06\x08'
        files[-1] = ffs(p.KB, sections)
        self.reject_fv(files, 'USB PE/DEPEX/UI section mismatch')

    def test_09_wrong_keyboard_pe(self):
        files = self.files.copy()
        sections = [dict(item) for item in self.sections[p.KB]]
        next(item for item in sections if item['type'] == 0x10)['data'] = self.modules[p.BUS]['pe']
        files[-1] = ffs(p.KB, sections)
        self.reject_fv(files, 'USB PE/DEPEX/UI section mismatch')

    def test_10_wrong_usb_file_type(self):
        files = self.files.copy()
        files[-1] = ffs(p.KB, self.sections[p.KB], 9)
        self.reject_fv(files, 'must be FFS DRIVER')

    def test_11_bad_fv_checksum(self):
        raw = bytearray(self.fv)
        raw[50] ^= 1
        with self.assertRaisesRegex(ValueError, 'FV checksum'):
            p.verify_fv(raw, self.baseline, self.modules)

    def test_12_nonempty_free_tail(self):
        raw = bytearray(self.fv)
        raw[-1] = 0
        with self.assertRaisesRegex(ValueError, 'free-space content'):
            p.verify_fv(raw, self.baseline, self.modules)

    def test_13_bad_ffs_header_checksum(self):
        raw = bytearray(self.fv)
        raw[self.rows[0]['offset'] + 16] ^= 1
        with self.assertRaisesRegex(ValueError, 'FFS header checksum'):
            p.verify_fv(raw, self.baseline, self.modules)

    def test_14_duplicate_source_manifest(self):
        with self.assertRaisesRegex(ValueError, 'Duplicate source'):
            p.manifest_map([{'path': 'x', 'size': 1, 'sha256': 'A'}] * 2)

    def image_fixture(self):
        transport = self.fv + bytes(p.TRANSPORT_SIZE - p.FV_SIZE)
        template = bytearray(self.parent)
        template[p.IDENTITY] = bytes(32)
        template[p.DIGEST] = __import__('hashlib').sha256(transport).digest()
        template[p.TRANSPORT_OFFSET:p.TRANSPORT_OFFSET + p.TRANSPORT_SIZE] = transport
        candidate = bytearray(template)
        candidate[p.IDENTITY] = __import__('hashlib').sha256(template).digest()
        return candidate, template, transport

    def test_15_in_memory_image_contract_positive(self):
        candidate, template, transport = self.image_fixture()
        report = p.verify_image(candidate, self.parent, transport, template, self.fv, self.baseline, self.modules)
        self.assertEqual(report['ffs_count'], 26)

    def test_16_unapproved_outside_delta_even_with_valid_identity(self):
        candidate, template, transport = self.image_fixture()
        template[0x1020] ^= 1
        candidate[:] = template
        candidate[p.IDENTITY] = __import__('hashlib').sha256(template).digest()
        with self.assertRaisesRegex(ValueError, 'Unapproved boot-image delta'):
            p.verify_image(candidate, self.parent, transport, template, self.fv, self.baseline, self.modules)

    def test_17_noncanonical_identity(self):
        candidate, template, transport = self.image_fixture()
        candidate[p.IDENTITY.start] ^= 1
        with self.assertRaisesRegex(ValueError, 'Canonical identity'):
            p.verify_image(candidate, self.parent, transport, template, self.fv, self.baseline, self.modules)

    def test_18_wrong_transport_digest(self):
        candidate, template, transport = self.image_fixture()
        candidate[p.DIGEST.start] ^= 1
        with self.assertRaisesRegex(ValueError, 'Transport digest'):
            p.verify_image(candidate, self.parent, transport, template, self.fv, self.baseline, self.modules)

    def test_19_wrong_transport_tail(self):
        candidate, template, transport = self.image_fixture()
        transport = bytearray(transport)
        transport[-1] = 1
        with self.assertRaisesRegex(ValueError, 'zero tail'):
            p.verify_image(candidate, self.parent, transport, template, self.fv, self.baseline, self.modules)

    def test_21_extracted_ffs_state_converted_for_genfv(self):
        for row in p.parse_fv(self.baseline, 23):
            before = row['raw']
            result = p.genfv_input_ffs(before)
            self.assertEqual(result[:23], before[:23])
            self.assertEqual(result[24:], before[24:])
            self.assertEqual(result[23], 7)
            self.assertEqual(result[23] ^ 0xff, before[23])
        with self.assertRaisesRegex(ValueError, 'on-flash'):
            p.genfv_input_ffs(result)

    def test_20_modified_fv_attributes_with_repaired_checksum(self):
        raw = bytearray(self.fv)
        raw[44] ^= 1
        raw[50:52] = bytes(2)
        checksum = -sum(struct.unpack_from('<36H', raw)) & 0xffff
        struct.pack_into('<H', raw, 50, checksum)
        with self.assertRaisesRegex(ValueError, 'attributes/extended-header prefix'):
            p.verify_fv(raw, self.baseline, self.modules)


if __name__ == '__main__':
    output = io.StringIO()
    result = unittest.TextTestRunner(stream=output, verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ContractTests))
    note = ('In-memory V6 structural fixture: accepted BDS_b01/HC_h01 and inherited bus01/kb01. '
            'No candidate, FV or transport file was generated. No device I/O.\n')
    text = note + output.getvalue()
    (HERE / 'PACKAGER_TESTS.txt').write_text(text)
    print(text)
    raise SystemExit(0 if result.wasSuccessful() else 1)

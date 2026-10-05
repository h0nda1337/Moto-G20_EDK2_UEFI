"""Exercise rejection of missing/stale test evidence before a firmware freeze."""
import hashlib
import json
import shutil
import tempfile
import unittest
from pathlib import Path
import freeze_usb_candidate as freeze

class HostBindingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work, self.root = (Path(self.tmp.name) / name for name in ('work', 'root'))
        self.report = self.root / 'REPORTS/VALIDATION_V6.json'
        self.report.parent.mkdir(parents=True)
        self.validation = json.loads((freeze.ROOT / 'REPORTS/VALIDATION_V6.json').read_text())
        for row in self.validation['binding']:
            source = Path(row['path'])
            dest = self.work / source.relative_to(freeze.WORK)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, dest)
            row['path'] = str(dest)
        shutil.copytree(freeze.ROOT / 'WORKSPACE/MotoG20Pkg', self.root / 'WORKSPACE/MotoG20Pkg')
        self.save()

    def save(self):
        self.report.write_text(json.dumps(self.validation))

    def check(self):
        return freeze.verify_host_validation(self.work, self.root)

    def test_current_tested_overlay_accepted(self):
        self.assertEqual(self.check(), len(self.validation['binding']))

    def test_missing_validation_rejected(self):
        self.report.unlink()
        with self.assertRaises(FileNotFoundError): self.check()

    def test_failed_validation_rejected(self):
        self.validation['status'] = 'FAILED'; self.save()
        with self.assertRaisesRegex(ValueError, 'missing or failed'): self.check()

    def test_duplicate_binding_rejected(self):
        self.validation['binding'].append(self.validation['binding'][0]); self.save()
        with self.assertRaisesRegex(ValueError, 'duplicate'): self.check()

    def test_source_missing_from_binding_rejected(self):
        self.validation['binding'] = [r for r in self.validation['binding'] if not r['path'].endswith('MotoG20MusbDxe.c')]
        self.save()
        with self.assertRaisesRegex(ValueError, 'absent from host validation'): self.check()

    def test_stale_source_rejected(self):
        path = self.work / 'usb_musb_driver/MotoG20MusbDxe.c'
        path.write_bytes(path.read_bytes() + b'\n')
        with self.assertRaisesRegex(ValueError, 'Artifact drift'): self.check()

    def test_overlay_header_drift_rejected(self):
        path = self.root / 'WORKSPACE/MotoG20Pkg/Include/Protocol/MotoG20UsbDiagnostic.h'
        path.write_bytes(path.read_bytes() + b'\n')
        with self.assertRaisesRegex(ValueError, 'Artifact drift'): self.check()

    def test_old_protocol_contract_rejected(self):
        self.validation['protocol_revision'] = 5; self.save()
        with self.assertRaisesRegex(ValueError, 'missing or failed'): self.check()

if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(HostBindingTests))
    report = {'status': 'PASS' if result.wasSuccessful() else 'FAILED',
              'tests': result.testsRun, 'device_io': 'NONE',
              'freeze_tool_sha256': hashlib.sha256(Path(freeze.__file__).read_bytes()).hexdigest().upper()}
    (freeze.ROOT / 'REPORTS/FREEZE_BINDING_TESTS.json').write_text(json.dumps(report, indent=2) + '\n')
    raise SystemExit(0 if result.wasSuccessful() else 1)

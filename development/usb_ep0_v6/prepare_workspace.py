"""Prepare an isolated V6 build from verified V5; no device access."""
import csv
import hashlib
import json
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
PREVIOUS_TOOLS = HERE.parent / 'usb_enumeration_v5'
LAB = Path(r'C:\MOTO_G20_WOA_LAB')
OLD = LAB / 'AUTOMATION/BUILD_ARTIFACTS/USB_HOST_20260927_V5'
ROOT = OLD.with_name('USB_HOST_20260929_V6')
GENERIC = OLD.with_name('USB_HOST_20260917')
MANIFEST = 'D5C37048D992C250156392EDCE1D44D0A880AA45C5326E01D7551A55AD31A35B'
LEDGER = '61BA467929B5E50CC9CEF28C482F364A401782B38ADF40B6689D98055D12884A'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()

def main():
    assert not ROOT.exists()
    assert sha(OLD / 'FREEZE_MANIFEST.csv') == MANIFEST
    assert sha(LAB / 'AUTOMATION/STATE/candidate_execution_registry.jsonl') == LEDGER
    rows = list(csv.DictReader((OLD / 'FREEZE_MANIFEST.csv').open(encoding='utf-8')))
    for row in rows:
        path = OLD / row['relative_path']
        assert path.resolve().is_relative_to(OLD.resolve())
        assert path.stat().st_size == int(row['size']) and sha(path) == row['sha256'], str(path)
    ROOT.mkdir()
    for name in ('WORKSPACE', 'CONF', 'BASELINE'):
        shutil.copytree(OLD / name, ROOT / name)
    shutil.copytree(OLD / 'SOURCE/MotoG20Pkg', ROOT / 'SOURCE/MotoG20Pkg')
    (ROOT / 'REPORTS').mkdir()
    shutil.copy2(OLD / 'REPORTS/SHARED_DEPENDENCIES.json', ROOT / 'REPORTS/SHARED_DEPENDENCIES.json')
    for pattern, folder in (('*.dsc', 'WORKSPACE'), ('*.txt', 'CONF')):
        for path in (ROOT / folder).rglob(pattern):
            path.write_text(path.read_text().replace(OLD.name, ROOT.name), newline='\n')
    names = ('build_usb_host.sh', 'integrate_usb_sources.py', 'record_usb_build.py',
             'package_usb_candidate.py', 'gop_candidate.py', 'gop_photo_forensics.py',
             'freeze_usb_candidate.py', 'prepare_usb_physical_controller.py',
             'test_prepare_usb_physical_controller.py', 'validate_physical_controller.py',
             'record_host_validation.py')
    for name in names:
        target = HERE / name
        assert not target.exists()
        text = (PREVIOUS_TOOLS / name).read_text()
        text = text.replace(OLD.name, ROOT.name).replace('V5', 'V6')
        text = text.replace('C2F7FF713A2F3C767BCC2D12B6410AEA21CC7F87CC5389CD29F46B95D705D864', LEDGER)
        text = text.replace('len(lines) == 17', 'len(lines) == 18')
        text = text.replace('len(registry.splitlines()) == 17', 'len(registry.splitlines()) == 18')
        if name == 'prepare_usb_physical_controller.py':
            text = text.replace('D719E353244EE0AFBF7D9FD2BD1408C1A29A6F6B2E85373FA50208C42051A7E1',
                                'E1E431DA519E139644D1AE6C140B1FF0D0E62F58B42418ACFD299921341388F8')
            text = text.replace('New V6 enumeration diagnostics and user-authorized removal of session exhaustion',
                                'New V6 EP0 causal snapshots and detached published-HC diagnostic throttle; no session exhaustion')
        if name == 'record_host_validation.py':
            text = text.replace('args.hc_groups > 51', 'args.hc_groups > 59')
            text = text.replace('args.observer_groups > 18', 'args.observer_groups > 28')
            text = text.replace("status['protocol_revision'] == 5", "status['protocol_revision'] == 6")
            text = text.replace("'protocol_revision': 5", "'protocol_revision': 6")
            text = text.replace('platform_source_unchanged_from_v4', 'platform_source_unchanged_from_v5')
        target.write_text(text, encoding='utf-8', newline='\n')
    (HERE / 'tests').mkdir()
    test = (PREVIOUS_TOOLS / 'tests/test_usb_packaging.py').read_text().replace('V5', 'V6')
    test = test.replace("registry['events'], 17", "registry['events'], 18")
    (HERE / 'tests/test_usb_packaging.py').write_text(test, encoding='utf-8', newline='\n')
    products = ROOT / 'BUILD/DEBUG_GCCNOLTO/AARCH64'
    products.mkdir(parents=True)
    inherited = []
    for name, tag in (('UsbBusDxe', 'bus01'), ('UsbKbDxe', 'kb01')):
        source = GENERIC / 'REPORTS' / (name + '_' + tag) / 'OUTPUT_EVIDENCE' / (name + '.efi')
        shutil.copy2(source, products / source.name)
        inherited.append({'path': str(source), 'sha256': sha(source)})
    report = {'status': 'V6_WORKSPACE_PREPARED_NOT_BUILT', 'root': str(ROOT),
              'previous_freeze_checked_files': len(rows), 'previous_manifest_sha256': MANIFEST,
              'registry_sha256': LEDGER, 'generic_products': inherited,
              'physical_result': 'NOT_TESTED', 'device_io': 'NONE'}
    (ROOT / 'REPORTS/PREPARATION_V6.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()

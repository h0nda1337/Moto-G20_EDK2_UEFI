"""Freeze new V6 source/evidence and bind immutable generic provenance; no device I/O."""
import csv
import json
import shutil
from pathlib import Path
import package_usb_candidate as package

WORK = Path(__file__).resolve().parent
ROOT = package.ROOT

def verify_host_validation(work=WORK, root=ROOT):
    """Reject stale PASS evidence or a compiled overlay unlike tested sources."""
    validation = json.loads((root / 'REPORTS/VALIDATION_V6.json').read_text())
    package.need(validation['status'] == 'PASS_HOST_MODELS_ONLY_NOT_PHYSICAL_USB'
                 and validation['protocol_revision'] == 6, 'V6 host validation missing or failed')
    package.need(validation['tests'] == {'platform': 35, 'hc': 65,
                 'console_policy': 10, 'producer_observer': 35}, 'Unexpected host test counts')
    bound = {}
    for row in validation['binding']:
        path = Path(row['path']).resolve()
        package.need(path.is_relative_to(work.resolve()) and path not in bound,
                     'Host binding path escape or duplicate')
        package.checked_file(path, row['size'], row['sha256'])
        bound[path] = row
    pairs = {
        'usb_musb_driver/MotoG20MusbDxe.c': ['Drivers/MotoG20MusbDxe/MotoG20MusbDxe.c'],
        'usb_musb_driver/MotoG20MusbRegs.h': ['Drivers/MotoG20MusbDxe/MotoG20MusbRegs.h'],
        'usb_musb_driver/MotoG20UsbDiagnostic.h': ['Drivers/MotoG20MusbDxe/MotoG20UsbDiagnostic.h', 'Include/Protocol/MotoG20UsbDiagnostic.h'],
        'usb_platform_driver/Library/MotoG20UsbPlatformLib.c': ['Library/MotoG20UsbPlatformLib/MotoG20UsbPlatformLib.c'],
        'usb_platform_driver/Library/MotoG20UsbPlatformLib.h': ['Library/MotoG20UsbPlatformLib/MotoG20UsbPlatformLib.h', 'Include/Library/MotoG20UsbPlatformLib.h'],
        'usb_console_bridge/MotoG20UsbConsoleStatus.c': ['Library/MotoG20PlatformBootManagerLib/MotoG20UsbConsoleStatus.c'],
        'usb_console_bridge/MotoG20UsbConsolePolicy.c': ['Library/MotoG20PlatformBootManagerLib/MotoG20UsbConsolePolicy.c'],
    }
    for relative, destinations in pairs.items():
        path = (work / relative).resolve()
        package.need(path in bound, 'Compiled source absent from host validation')
        for target in destinations:
            package.checked_file(root / 'WORKSPACE/MotoG20Pkg' / target,
                                 bound[path]['size'], bound[path]['sha256'])
    return len(bound)

def main():
    bindings = verify_host_validation()
    report = json.loads((ROOT / 'REPORTS/USB_CANDIDATE_HOST_AUDIT.json').read_text())
    replay = package.replay(report['bds_tag'], report['hc_tag'])
    package.need(not (ROOT / 'FREEZE_MANIFEST.csv').exists(), 'Already frozen')
    evidence = ROOT / 'HOST_VALIDATION'
    package.need(not evidence.exists(), 'Host evidence already exists')
    evidence.mkdir()
    for name in ('usb_musb_driver', 'usb_platform_driver', 'usb_console_bridge', 'tests'):
        shutil.copytree(WORK / name, evidence / name,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    shutil.copytree(WORK.parent / 'usb_generic_build/depex_review', evidence / 'usb_generic_build/depex_review',
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    for path in WORK.iterdir():
        if path.is_file() and path.suffix in ('.py', '.sh', '.md'):
            shutil.copy2(path, evidence / path.name)
    # Preserve generic accepted products and source snapshots locally as well
    # as binding their original frozen provenance.
    generic = ROOT / 'INHERITED_GENERIC'
    generic.mkdir()
    for name in ('UsbBusDxe_bus01', 'UsbKbDxe_kb01'):
        shutil.copytree(package.GENERIC_ROOT / 'REPORTS' / name, generic / 'REPORTS' / name)
    for name in ('EDK2', 'EDK2_BUILD_DEPENDENCIES'):
        shutil.copytree(package.GENERIC_ROOT / 'SOURCE' / name, generic / 'SOURCE' / name)
    for name in ('PREPARATION.json', 'SHARED_DEPENDENCIES.json'):
        shutil.copy2(package.GENERIC_ROOT / 'REPORTS' / name, generic / 'REPORTS' / name)
    old_manifest = package.GENERIC_ROOT / 'FREEZE_MANIFEST.csv'
    package.need(package.sha(old_manifest.read_bytes()) == 'D1B703F1840F74F05257F710BBEEA8CC32C0FEACD3A49078255317C3E53CC5AE', 'Inherited manifest drift')
    folders = ('CANDIDATE', 'FV', 'FVINPUT', 'WORKSPACE', 'CONF', 'REPORTS',
               'SOURCE', 'BASELINE', 'HOST_VALIDATION', 'INHERITED_GENERIC')
    policy = {'status': 'HOST_ONLY_PHYSICAL_USB_NOT_TESTED', 'replay': replay,
              'host_validation_bindings_checked': bindings,
              'included_roots': list(folders), 'excluded': ['BUILD: mutable compiler cache'],
              'generic_provenance': {'manifest': str(old_manifest), 'sha256': package.sha(old_manifest.read_bytes())},
              'physical_execution_count': 0,
              'later_controller_and_phone_reports': 'Separate records bound to this manifest; no circular self hash.'}
    (ROOT / 'FREEZE_POLICY.json').write_text(json.dumps(policy, indent=2) + '\n')
    paths = [ROOT / 'FREEZE_POLICY.json']
    for folder in folders:
        paths.extend(path for path in (ROOT / folder).rglob('*') if path.is_file())
    rows = []
    for path in sorted(paths):
        package.need(path.resolve().is_relative_to(ROOT.resolve()), 'Freeze path escape')
        rows.append({'relative_path': path.relative_to(ROOT).as_posix(),
                     'size': path.stat().st_size, 'sha256': package.sha(path.read_bytes())})
    with (ROOT / 'FREEZE_MANIFEST.csv').open('x', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=['relative_path', 'size', 'sha256'])
        writer.writeheader(); writer.writerows(rows)
    for row in rows:
        package.checked_file(ROOT / row['relative_path'], row['size'], row['sha256'])
    print(json.dumps({'status': 'FROZEN_HOST_ONLY', 'files': len(rows),
                      'manifest_sha256': package.sha((ROOT / 'FREEZE_MANIFEST.csv').read_bytes()),
                      'candidate_sha256': replay['candidate_sha256']}))

if __name__ == '__main__':
    main()

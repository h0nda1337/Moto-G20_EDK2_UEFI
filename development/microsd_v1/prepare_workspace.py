"""Create the isolated, build-only microSD workspace on user-authorized G:."""
import csv
import hashlib
import json
import re
import shutil
from pathlib import Path
from integrate_workspace import integrate

HERE = Path(__file__).resolve().parent
LAB = Path(r'C:\MOTO_G20_WOA_LAB')
OLD = LAB / 'AUTOMATION/BUILD_ARTIFACTS/USB_HOST_20260929_V6'
ROOT = Path(r'G:\MOTO_G20_WOA_LAB\BUILD_ARTIFACTS\MICROSD_20261002_V1')
LINUX = '/mnt/g/MOTO_G20_WOA_LAB/BUILD_ARTIFACTS/MICROSD_20261002_V1'
EXPECTED = '467C7AC92A6EBC3798056140318458F88D930CF492567B312820372E0A78ED45'


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest().upper()


def main():
    assert not ROOT.exists(), 'Preserve prior build; this script creates a new workspace only'
    assert shutil.disk_usage(ROOT.anchor).free > 5 * 1024**3
    manifest = OLD / 'FREEZE_MANIFEST.csv'
    assert sha(manifest) == EXPECTED
    rows = list(csv.DictReader(manifest.open(encoding='utf-8')))
    for row in rows:
        path = OLD / row['relative_path']
        assert path.resolve().is_relative_to(OLD.resolve())
        assert path.stat().st_size == int(row['size']) and sha(path) == row['sha256'], path
    registry = LAB / 'AUTOMATION/STATE/candidate_execution_registry.jsonl'
    registry_before = sha(registry)
    pkg = ROOT / 'WORKSPACE/MotoG20Pkg'
    pkg.parent.mkdir(parents=True)
    shutil.copytree(OLD / 'SOURCE/MotoG20Pkg', pkg)
    (ROOT / 'CONF').mkdir()
    (ROOT / 'REPORTS').mkdir()
    for name in ('target.txt', 'tools_def.txt', 'build_rule.txt'):
        shutil.copy2(OLD / 'CONF' / name, ROOT / 'CONF' / name)
    target = ROOT / 'CONF/target.txt'
    text = target.read_text()
    for key, val in {
        'ACTIVE_PLATFORM': 'MotoG20Pkg/Foundation/MotoG20MicroSd.dsc',
        'TARGET_ARCH': 'AARCH64', 'TOOL_CHAIN_TAG': 'GCCNOLTO',
        'TOOL_CHAIN_CONF': LINUX + '/CONF/tools_def.txt',
        'BUILD_RULE_CONF': LINUX + '/CONF/build_rule.txt',
    }.items():
        text, count = re.subn(r'^' + key + r'\s*=.*$', key + ' = ' + val, text, flags=re.M)
        assert count == 1, key
    target.write_text(text, newline='\n')
    dsc = (OLD / 'WORKSPACE/MotoG20Pkg/Foundation/MotoG20UsbGeneric.dsc').read_text().split('[Components]')[0]
    dsc = re.sub(r'^  OUTPUT_DIRECTORY\s*=.*$', '  OUTPUT_DIRECTORY = ' + LINUX + '/BUILD', dsc, flags=re.M)
    dsc = re.sub(r'^  PLATFORM_NAME\s*=.*$', '  PLATFORM_NAME = MotoG20MicroSd', dsc, flags=re.M)
    dsc = re.sub(r'^  UefiUsbLib\|.*\n', '', dsc, flags=re.M)
    dsc = re.sub(r'^  gEmbeddedTokenSpaceGuid\.PcdTimerPeriod\|.*\n', '', dsc, flags=re.M)
    dsc += '''[Components]
  MotoG20Pkg/Drivers/MotoG20SdDxe/MotoG20SdDxe.inf
  MotoG20Pkg/Diagnostics/BdsDxe/BdsDxe.inf
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf
'''
    assert 'Usb' not in dsc and 'USB' not in dsc
    (pkg / 'Foundation/MotoG20MicroSd.dsc').write_text(dsc, newline='\n')
    destination = pkg / 'Drivers/MotoG20SdDxe'
    destination.mkdir()
    for folder in ('core', 'driver'):
        for source in (HERE / folder).iterdir():
            if source.is_file(): shutil.copy2(source, destination / source.name)
    assert (pkg / 'Diagnostics/BdsDxe/BdsDxe.inf').is_file()
    integrate(ROOT)
    assert sha(registry) == registry_before
    report = {'status': 'BUILD_ONLY_NO_PLATFORM_PROVIDER_NO_FLASH_IMAGE',
              'root': str(ROOT), 'frozen_v6_verified_files': len(rows),
              'frozen_manifest_sha256': EXPECTED, 'physical_registry_sha256': registry_before,
              'usb_components': [], 'device_io': 'NONE', 'sd_platform_provider': 'NOT_IMPLEMENTED',
              'hardware_read_result': 'NOT_TESTED'}
    (ROOT / 'REPORTS/PREPARATION.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__': main()

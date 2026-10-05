"""Record verified build/media facts, without touching the phone."""
import hashlib
import json
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORK = HERE.parents[1]
OUT = WORK / 'outputs/microsd_v1_20261002'
BUILD = Path(r'G:\MOTO_G20_WOA_LAB\BUILD_ARTIFACTS\MICROSD_20261002_V1')
LAB = Path(r'C:\MOTO_G20_WOA_LAB')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest().upper()


def main():
    assert not (BUILD / 'BUILD_TRANCHE.lock').exists()
    report = BUILD / 'REPORTS/BUILD_03'
    assert (report / 'exit_code.txt').read_text().strip() == '0'
    assert '- Done -' in (report / 'build.log').read_text()
    records = json.loads((report / 'sources.json').read_text())
    pkg = BUILD / 'WORKSPACE/MotoG20Pkg'
    for row in records:
        assert sha(pkg / row['path']) == row['sha256'], row['path']
    modules = []
    for name in ('MotoG20SdDxe', 'BdsDxe', 'DiskIoDxe', 'PartitionDxe', 'Fat'):
        p = BUILD / 'BUILD/DEBUG_GCCNOLTO/AARCH64' / (name + '.efi')
        data = p.read_bytes()
        assert data[:2] == b'MZ'
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        assert data[pe:pe+4] == b'PE\0\0' and struct.unpack_from('<H', data, pe+4)[0] == 0xaa64
        modules.append({'name': name, 'size': len(data), 'sha256': sha(p), 'machine': 'ARM64'})
    probe = json.loads((OUT / 'PROBE_TESTS/RESULT.json').read_text())
    assert probe['status'] == 'HOST_SIMULATION_PASSED' and probe['scenarios'] == 31
    for row in probe['sources']:
        assert sha(WORK / row['path']) == row['sha256']
    assert 'PASS: 35 host scenarios.' in (OUT / 'TESTS_02/host_tests.txt').read_text()
    payload = json.loads((OUT / 'CARD_PAYLOAD/RESULT.json').read_text())
    assert payload['files_verified'] == 681 and not payload['uefi_boot_tested']
    baseline = json.loads((OUT / 'CARD_SECTOR_BASELINE/RESULT.json').read_text())
    assert baseline['marker']['bytes'] == 31 and not baseline['uefi_read_verified']
    registry = LAB / 'AUTOMATION/STATE/candidate_execution_registry.jsonl'
    assert sha(registry) == '61BA467929B5E50CC9CEF28C482F364A401782B38ADF40B6689D98055D12884A'
    state = {'state': 'MICROSD_MEDIA_READY_PLATFORM_INITIALIZATION_PENDING',
             'update_date': '2026-10-03', 'usb_work': 'PAUSED_BY_USER',
             'phone_state_last_verified': 'ANDROID_ADB', 'rebooted_this_phase': False,
             'flashed_this_phase': False, 'sd_card': payload, 'raw_sd_reference': baseline,
             'read_core_host_tests': 35, 'probe_host_tests': 31,
             'edk2_build': str(report), 'modules': modules,
             'sd_platform_provider': 'NOT_IMPLEMENTED', 'boot_image': 'NOT_PRODUCED',
             'physical_sd_uefi_read': 'NOT_TESTED', 'winpe_boot': 'NOT_TESTED',
             'physical_registry_sha256': sha(registry),
             'next': 'Implement and test native SD Prepare/lifecycle, then integrate new FV and one-use physical candidate. Do not reformat media or rerun USB V6.'}
    (OUT / 'CURRENT_STATUS.json').write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps({k: v for k, v in state.items() if k not in ('modules', 'sd_card', 'raw_sd_reference')}, indent=2))


if __name__ == '__main__':
    main()

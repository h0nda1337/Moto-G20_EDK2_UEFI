"""Bind completed V6 host test evidence to actual source bytes, without device IO."""
import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

here = Path(__file__).resolve().parent
root = Path(r'C:\MOTO_G20_WOA_LAB\AUTOMATION\BUILD_ARTIFACTS\USB_HOST_20260929_V6')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()

def passes(path):
    return sum(line.startswith('PASS ') for line in path.read_text(encoding='utf-8-sig').splitlines())

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--hc-groups', type=int, required=True)
    parser.add_argument('--observer-groups', type=int, required=True)
    args = parser.parse_args()
    hc, plat, obs = (here / name for name in ('usb_musb_driver', 'usb_platform_driver', 'usb_console_bridge'))
    assert args.hc_groups > 59 and passes(hc / 'tests/RESULT.txt') == args.hc_groups
    assert passes(plat / 'Tests/RESULT.txt') == 35
    assert sha(plat / 'Library/MotoG20UsbPlatformLib.c') == '7217DFA385E57AEAA6C2D97D26533CD5FB48917FB8DFC2CAD3C39628E3884B73'
    assert sha(plat / 'Library/MotoG20UsbPlatformLib.h') == '87AF5C9D09F5621DE2A11DA6FD1BCEE37D262DA77981983C740BB13B5613B89C'
    status = json.loads((obs / 'STATUS_TEST.json').read_text())
    assert status['status'] == 'PASS_HOST_PRODUCER_OBSERVER_ONLY'
    assert args.observer_groups > 28 and status['regression_groups'] == args.observer_groups
    assert status['protocol_revision'] == 6
    assert status['producer_sha256'] == sha(hc / 'MotoG20MusbDxe.c')
    assert status['protocol_sha256'] == sha(hc / 'MotoG20UsbDiagnostic.h')
    assert status['observer_sha256'] == sha(obs / 'MotoG20UsbConsoleStatus.c')
    assert 'PASS: 10' in json.dumps(json.loads((obs / 'POLICY_TEST.json').read_text()))
    paths = [hc / name for name in ('MotoG20MusbDxe.c', 'MotoG20MusbRegs.h', 'MotoG20UsbDiagnostic.h', 'tests/test_musb.c', 'tests/run.ps1', 'tests/RESULT.txt')]
    paths += [plat / name for name in ('Library/MotoG20UsbPlatformLib.c', 'Library/MotoG20UsbPlatformLib.h', 'Tests/platform_tests.c', 'Tests/RESULT.txt')]
    paths += [obs / name for name in ('MotoG20UsbConsoleStatus.c', 'MotoG20UsbConsolePolicy.c', 'STATUS_TEST.json', 'STATUS_TEST.txt', 'POLICY_TEST.json', 'producer_under_test.h', 'status_test.c', 'test_status.py')]
    value = {'status': 'PASS_HOST_MODELS_ONLY_NOT_PHYSICAL_USB', 'recorded_utc': datetime.now(timezone.utc).isoformat(),
        'tests': {'platform': 35, 'hc': args.hc_groups, 'console_policy': 10, 'producer_observer': args.observer_groups},
        'protocol_revision': 6, 'platform_source_unchanged_from_v5': True,
        'v6_scope': {'causal_ep0_trace': True, 'no_new_wait_ep0_register_reads': True,
            'no_hc_mmio_after_role_loss': True, 'post_reset_cache_historical': True,
            'published_hc_detached_drp_throttled': True,
            'transfer_algorithms_phy_and_delays_unchanged': True},
        'retry_policy': {'lifetime_limit': None, 'user_requested_removal_of_exhausted': True,
            'stable_detach_ms': 500, 'qualification_deadline_ms': 1100,
            'continuous_attach_does_not_rearm': True, 'fatal_fault_handling_retained': True},
        'binding': [{'path': str(p), 'sha256': sha(p), 'size': p.stat().st_size} for p in paths],
        'phone_access_for_validation': 'NONE', 'hardware_result': 'NOT_TESTED',
        'limitations': ['Model-supplied hardware events do not prove enumeration or keyboard input.',
            'Inventory descriptor queries read UsbBus caches; counts concern interfaces, not unique physical devices.',
            'The most recent error may be historical; it is not proof of the current root cause.']}
    for path in (root / 'REPORTS/VALIDATION_V6.json', hc / 'tests/VALIDATION_V6.json'):
        path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': value['status'], 'tests': value['tests']}))

if __name__ == '__main__':
    main()

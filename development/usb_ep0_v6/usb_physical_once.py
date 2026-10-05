"""Bounded, explicitly authorized single-use USB trial. Each mutation is journaled first."""
import argparse
import csv
import hashlib
import json
import os
import re
import stat
import subprocess
import time
import zlib
from datetime import datetime, timezone
from pathlib import Path

LAB = Path(r'C:\MOTO_G20_WOA_LAB')
ROOT = Path('C:\\MOTO_G20_WOA_LAB\\AUTOMATION\\BUILD_ARTIFACTS\\USB_HOST_20260929_V6')
STATE = LAB / 'AUTOMATION/STATE'
ATTEMPT = LAB / 'AUTOMATION/RESULTS/USB_HOST_PHYSICAL_ATTEMPT1_21A0EAA08BE2'
SERIAL = 'ZY32CVND77'
NEW_SHA = '21A0EAA08BE2E1F8C9A23C17845F935B7BA48EE3B26837835E200195BAE7D690'
OLD_SHA = 'E1E431DA519E139644D1AE6C140B1FF0D0E62F58B42418ACFD299921341388F8'
SAFE_B = 'C0CFBCF4574F20F4028D7A4C5EE201FAD2C28F841201F94A0F48DD5ED76DF7F3'
REGISTRY_SHA = '61BA467929B5E50CC9CEF28C482F364A401782B38ADF40B6689D98055D12884A'
MANIFEST_SHA = '467C7AC92A6EBC3798056140318458F88D930CF492567B312820372E0A78ED45'
CANDIDATE = ROOT / 'CANDIDATE/MOTO_G20_USB_HOST_HOST_ONLY.img'
ADB = Path(r'C:\Users\Angelo\AppData\Local\Microsoft\WinGet\Packages\Genymobile.scrcpy_Microsoft.Winget.Source_8wekyb3d8bbwe\scrcpy-win64-v4.1\adb.exe')
FASTBOOT = Path(r'C:\Users\Angelo\AppData\Local\Microsoft\WinGet\Packages\Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe\platform-tools\fastboot.exe')
ADB_SHA = '957E46B8615F7AF5B7292A2DDABE98D2E61940C3FB2B0545756507F080613E71'
FASTBOOT_SHA = 'B2D9CBFF4CE9AE7EB448CFC831BAFC867935F50F5BE38F8F81057FD7EB3B8D86'
ONE = LAB / '09_AB_BOOT_CONTROL/bootctrl_A_EFFECTIVE_ONESHOT.bin'
ONE_BYTES = bytes.fromhex('5f62000042434142010200002f00ee0000000000000000000000000078381c31')
NORMAL_BYTES = bytes.fromhex('5f62000042434142010200001e00ef000000000000000000000000007c0d63f3')
ONE_SHA = '4661F61B566D51351087C8CD58C9B44822F5319C81DAE43DEC76F24257E1A00B'
REMOTE = '/data/local/tmp/usb_host_21a0eaa08be2_oneshot.bin'
CLAIM_DIR = STATE / 'USB_HOST_PHYSICAL_ATTEMPT1_21A0EAA08BE2_EXECUTION_CLAIMS'
CLAIM = CLAIM_DIR / (NEW_SHA + '.claim.json')
CONSUMED = CLAIM_DIR / (NEW_SHA + '.dispatch-consumed.json')
AUTH = 'User authorized continuing Moto G20 USB work and flashing, with the phone available over ADB, and explicitly removed the battery-charge minimum for all future trials: ignora bateria siempre. Charge is recorded without gating. New V6 EP0 causal snapshots and detached published-HC diagnostic throttle; no session exhaustion USB hub/keyboard candidate; one bounded trial, safe Android B preserved. No reuse of consumed candidates.'

def now():
    return datetime.now(timezone.utc).isoformat()

def sha(raw):
    return hashlib.sha256(raw).hexdigest().upper()

def need(ok, message):
    if not ok:
        raise RuntimeError(message)

def once(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x', encoding='utf-8') as stream:
        stream.write(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
        stream.flush()
        os.fsync(stream.fileno())

def intent(name, details):
    once(ATTEMPT / (name + '.intent.json'), {'utc': now(), **details})

def run(label, argv, timeout=30, binary=False, required=True, combined=False):
    journal = ATTEMPT / 'commands'
    journal.mkdir(parents=True, exist_ok=True)
    key = datetime.now(timezone.utc).strftime('%H%M%S%f') + '_' + label
    once(journal / (key + '.intent.json'), {'utc': now(), 'argv': [str(a) for a in argv]})
    try:
        proc = subprocess.run([str(a) for a in argv], capture_output=True, timeout=timeout)
    except Exception as error:
        once(journal / (key + '.uncertain.json'), {'utc': now(), 'error': repr(error), 'retry': 'NO_AUTOMATIC_RETRY'})
        raise
    text_out = proc.stdout.decode('utf-8', errors='replace')
    result = {'utc': now(), 'exit_code': proc.returncode, 'stderr': proc.stderr.decode('utf-8', errors='replace')}
    if binary:
        result.update({'stdout_bytes': len(proc.stdout), 'stdout_sha256': sha(proc.stdout)})
    else:
        result['stdout'] = text_out
    once(journal / (key + '.result.json'), result)
    if required:
        need(proc.returncode == 0, f'{label} failed: {result}')
    returned_text = text_out + ('\n' + result['stderr'] if combined else '')
    return proc.stdout if binary else (returned_text.strip(), proc.returncode)

def shell(label, cmd, root=False, binary=False, timeout=30):
    if root:
        need("'" not in cmd, 'Unexpected shell quoting')
        cmd = "su -c '" + cmd + "'"
    result = run(label, [ADB, '-s', SERIAL, 'exec-out' if binary else 'shell', cmd], timeout, binary)
    return result if binary else result[0]

def host():
    need(sha(ADB.read_bytes()) == ADB_SHA and sha(FASTBOOT.read_bytes()) == FASTBOOT_SHA, 'Transport executable drift')
    manifest = ROOT / 'FREEZE_MANIFEST.csv'
    need(sha(manifest.read_bytes()) == MANIFEST_SHA, 'Candidate manifest drift')
    rows = list(csv.DictReader(manifest.open(encoding='utf-8')))
    need(len(rows) == 2245, 'Manifest count')
    for row in rows:
        path = ROOT / row['relative_path']
        need(path.resolve().is_relative_to(ROOT.resolve()), 'Manifest escapes root')
        need(path.stat().st_size == int(row['size']) and sha(path.read_bytes()) == row['sha256'], 'Frozen artifact drift: ' + str(path))
    need(CANDIDATE.stat().st_size == 0x4000000 and sha(CANDIDATE.read_bytes()) == NEW_SHA, 'Candidate identity')
    need(sha((STATE / 'candidate_execution_registry.jsonl').read_bytes()) == REGISTRY_SHA, 'Execution registry changed')
    need(ONE.read_bytes() == ONE_BYTES and sha(ONE_BYTES) == ONE_SHA, 'One-shot payload mismatch')
    for raw in (NORMAL_BYTES, ONE_BYTES):
        need(len(raw) == 32 and zlib.crc32(raw[:28]) == int.from_bytes(raw[28:], 'little'), 'Boot control CRC')
    return {'manifest_sha256': MANIFEST_SHA, 'files_verified': 2245, 'candidate_sha256': NEW_SHA,
            'adb_sha256': ADB_SHA, 'fastboot_sha256': FASTBOOT_SHA, 'registry_sha256': REGISTRY_SHA}

def misc_read(label):
    raw = shell(label, 'dd if=/dev/block/by-name/misc bs=1 skip=2048 count=32 2>/dev/null', root=True, binary=True)
    need(len(raw) == 32, 'Truncated misc read')
    return raw

def live(expected_a, battery_gate=True):
    devices, _ = run('adb_devices', [ADB, 'devices', '-l'])
    lines = [x for x in devices.splitlines()[1:] if x.strip()]
    need(len(lines) == 1 and re.match('^' + SERIAL + r'\s+device\b', lines[0]), 'Expected exactly the authorized Moto G20')
    expected = {'ro.product.model': 'moto g(20)', 'ro.boot.hardware.sku': 'XT2128-1',
        'ro.product.device': 'java', 'ro.board.platform': 'ums512', 'ro.boot.slot_suffix': '_b',
        'ro.boot.verifiedbootstate': 'orange', 'ro.boot.flash.locked': '0', 'ro.boot.vbmeta.device_state': 'unlocked',
        'sys.boot_completed': '1'}
    props = {k: shell(k, 'getprop ' + k) for k in expected}
    need(props == expected, f'Device/Android B identity mismatch: {props}')
    need(shell('root', 'id', root=True).startswith('uid=0(root)'), 'Root missing')
    links = {n: shell(n + '_link', 'readlink -f /dev/block/by-name/' + n, root=True) for n in ('boot_a', 'boot_b', 'misc')}
    need(links == {'boot_a': '/dev/block/mmcblk0p43', 'boot_b': '/dev/block/mmcblk0p44', 'misc': '/dev/block/mmcblk0p3'}, 'Partition link drift')
    for name in ('boot_a', 'boot_b'):
        need(shell(name + '_size', 'blockdev --getsize64 /dev/block/by-name/' + name, root=True) == '67108864', 'Partition size drift')
    hashes = {n: shell(n + '_hash', 'sha256sum /dev/block/by-name/' + n, root=True, timeout=60).split()[0].upper() for n in ('boot_a', 'boot_b')}
    need(hashes == {'boot_a': expected_a, 'boot_b': SAFE_B}, f'Partition hash mismatch: {hashes}')
    misc = misc_read('misc_read')
    need(misc == NORMAL_BYTES, 'Boot control is not NORMAL_B')
    battery_text = shell('battery', 'dumpsys battery')
    battery = {k.strip(): v.strip() for line in battery_text.splitlines() if ':' in line for k, v in [line.split(':', 1)]}
    level, temp = int(battery['level']), int(battery['temperature'])
    if battery_gate:
        need(temp <= 420 and battery['USB powered'] == 'true', 'Phone temperature/USB gate not met')
    probes = [shell('boot_stability', 'getprop sys.boot_completed') for _ in range(3)]
    need(probes == ['1'] * 3, 'Android not stable')
    pstore = shell('pstore_list', 'ls -l /sys/fs/pstore', root=True)
    return {'utc': now(), 'serial': SERIAL, 'properties': props, 'links': links, 'boot_sha256': hashes,
            'misc_hex': misc.hex(), 'misc_sha256': sha(misc), 'battery_percent': level,
            'temperature_c': temp / 10, 'usb_powered': battery['USB powered'] == 'true', 'pstore_listing': pstore}

def claim_value():
    value = json.loads(CLAIM.read_text(encoding='utf-8'))
    need(value['candidate_sha256'] == NEW_SHA and value['attempt_root'] == str(ATTEMPT), 'Claim mismatch')
    need(value['controller_sha256'] == sha(Path(__file__).read_bytes()), 'Controller changed after claim')
    return value

def preflash_basis():
    claim = claim_value()
    need(claim.get('authorization') == AUTH, 'Claim authorization mismatch')
    prior = claim['live_before']
    current_time = datetime.now(timezone.utc)
    claim_time = datetime.fromisoformat(claim['utc'])
    live_time = datetime.fromisoformat(prior['utc'])
    need(live_time.tzinfo is not None and claim_time.tzinfo is not None, 'Preflight timestamps need timezone')
    need(live_time <= claim_time <= current_time, 'Preflight/claim timestamps out of order')
    age = (current_time - live_time).total_seconds()
    need(0 <= age <= 900, 'Preclaim reads older than 15 minutes; stop before flash')
    need(prior['serial'] == SERIAL, 'Preclaim serial mismatch')
    need(prior['boot_sha256'] == {'boot_a': OLD_SHA, 'boot_b': SAFE_B}, 'Preclaim hash mismatch')
    need(prior['properties']['ro.boot.slot_suffix'] == '_b' and
         prior['properties']['sys.boot_completed'] == '1' and
         prior['misc_hex'] == NORMAL_BYTES.hex(), 'Preclaim is not safe Android B')
    need(prior['links'] == {'boot_a': '/dev/block/mmcblk0p43',
         'boot_b': '/dev/block/mmcblk0p44', 'misc': '/dev/block/mmcblk0p3'}, 'Preclaim partition link drift')
    need(prior['temperature_c'] <= 42 and
         prior['usb_powered'] is True, 'Preclaim phone temperature/USB gate not met')
    return {'preclaim_age_seconds': age, 'claim_sha256': sha(CLAIM.read_bytes()),
            'live_utc': prior['utc'], 'claim_utc': claim['utc']}

def wait_fastboot():
    for _ in range(30):
        output, code = run('fastboot_devices', [FASTBOOT, 'devices'], required=False)
        rows = [x.split() for x in output.splitlines() if x.strip()]
        if code == 0 and rows == [[SERIAL, 'fastboot']]:
            current, _ = run('fastboot_slot', [FASTBOOT, '-s', SERIAL, 'getvar', 'current-slot'], combined=True)
            # The known response has a blank current-slot value followed
            # by a timing line. Require that exact key once; silence is not it.
            slot_lines = re.findall(r'(?m)^current-slot:[ \t]*([^\r\n]*)\r?$', current)
            need(len(slot_lines) == 1, 'Malformed or conflicting fastboot current-slot response')
            slot = slot_lines[0].strip()
            need(slot in ('', 'b'), 'Fastboot reported a slot other than B')
            basis_info = preflash_basis()
            if slot == '':
                # Exact serial plus fresh Android B/misc claim is the fallback
                # evidence. A blank value does not report a slot.
                once(ATTEMPT / 'EMPTY_GETVAR_OBSERVED.json', {
                    'utc': now(), 'serial': SERIAL, 'reply': current,
                    'slot_reported_by_fastboot': 'UNAVAILABLE',
                    'basis': 'fresh claim.live_before Android B and NORMAL_B; exact known fastboot serial',
                    **basis_info,
                    'historical_reference': 'GOP_SCALAR_PHYSICAL_ATTEMPT1_20260913/CONTINUATION_REVIEW.json'})
            return
        time.sleep(1)
    raise RuntimeError('Known fastboot device did not appear')

def wait_android():
    for _ in range(36):
        value, code = run('android_boot_probe', [ADB, '-s', SERIAL, 'shell', 'getprop sys.boot_completed'], timeout=10, required=False)
        if code == 0 and value == '1':
            return live(NEW_SHA)
        time.sleep(3)
    raise RuntimeError('Android B did not return within observation window; no retry')

def prepare():
    need(not ATTEMPT.exists(), 'Attempt already exists; inspect its evidence')
    need(not list(STATE.rglob(NEW_SHA + '*.json')), 'Candidate already claimed/consumed')
    host_info = host()
    ATTEMPT.mkdir(parents=True)
    first, second = live(OLD_SHA), live(OLD_SHA)
    for field in ('serial', 'properties', 'links', 'boot_sha256', 'misc_hex'):
        need(first[field] == second[field], 'Read-only preflight is inconsistent')
    CANDIDATE.chmod(stat.S_IREAD)
    once(ATTEMPT / '00_READY.json', {'utc': now(), 'status': 'READY_FOR_SINGLE_USE_FLASH', 'host': host_info,
        'first': first, 'second': second, 'authorization': AUTH,
        'battery_policy': {'minimum_percent': None, 'charge_gate': 'REMOVED_BY_USER_FOR_ALL_FUTURE_TRIALS', 'maximum_temperature_c': 42, 'usb_required': True},
        'controller_sha256': sha(Path(__file__).read_bytes())})
    return {'status': 'READY', 'candidate': NEW_SHA, 'battery_percent': second['battery_percent'], 'temperature_c': second['temperature_c']}

def flash():
    host()
    readiness = json.loads((ATTEMPT / '00_READY.json').read_text(encoding='utf-8'))
    need(readiness['controller_sha256'] == sha(Path(__file__).read_bytes()), 'Controller changed after preparation')
    age = (datetime.now(timezone.utc) - datetime.fromisoformat(readiness['utc'])).total_seconds()
    need(0 <= age <= 3600, 'Preparation is stale')
    need(not list(STATE.rglob(NEW_SHA + '*.json')), 'Candidate already claimed/consumed')
    fresh = live(OLD_SHA)
    once(CLAIM, {'utc': now(), 'candidate_sha256': NEW_SHA, 'attempt_root': str(ATTEMPT), 'authorization': AUTH,
        'controller_sha256': sha(Path(__file__).read_bytes()), 'live_before': fresh,
        'max_boot_a_flashes': 1, 'max_misc_writes': 1, 'max_test_reboots': 1, 'retry': 'FORBIDDEN'})
    intent('01_REBOOT_BOOTLOADER', {'from': 'ANDROID_B_NORMAL_MISC'})
    run('reboot_bootloader', [ADB, '-s', SERIAL, 'reboot', 'bootloader'])
    wait_fastboot()
    host()
    preflash_basis()
    intent('02_FLASH_BOOT_A', {'candidate_sha256': NEW_SHA, 'partition': 'boot_a', 'maximum': 1})
    output, _ = run('flash_boot_a', [FASTBOOT, '-s', SERIAL, 'flash', 'boot_a', CANDIDATE], timeout=180)
    once(ATTEMPT / '02_FLASH_COMMAND_COMPLETED.json', {'utc': now(), 'status': 'COMMAND_COMPLETED_READBACK_PENDING', 'stdout': output})
    intent('03_REBOOT_ANDROID_B', {'misc_expected': 'NORMAL_B'})
    run('fastboot_reboot_b', [FASTBOOT, '-s', SERIAL, 'reboot'])
    after = wait_android()
    once(ATTEMPT / '04_FLASH_READBACK_PASS.json', {'utc': now(), 'status': 'FLASH_VERIFIED_ANDROID_B', 'live': after,
        'candidate_execution': 'NOT_DISPATCHED', 'boot_b_writes': 0, 'misc_writes': 0})
    return {'status': 'FLASH_VERIFIED_ANDROID_B', 'candidate_sha256': NEW_SHA, 'battery_percent': after['battery_percent']}

def boot():
    host()
    claim_value()
    need((ATTEMPT / '04_FLASH_READBACK_PASS.json').is_file(), 'No verified post-flash Android B')
    need(not CONSUMED.exists() and not (ATTEMPT / '05_ARM_MISC.intent.json').exists(), 'Trial already armed/consumed; no retry')
    fresh = live(NEW_SHA)
    absent, code = run('remote_absent', [ADB, '-s', SERIAL, 'shell', "su -c 'test ! -e " + REMOTE + "'"], required=False)
    need(code == 0, 'Remote one-shot path already exists')
    run('push_one_shot', [ADB, '-s', SERIAL, 'push', ONE, REMOTE])
    need(shell('remote_one_hash', 'sha256sum ' + REMOTE, root=True).split()[0].upper() == ONE_SHA, 'Remote payload mismatch')
    need(misc_read('misc_pre_arm') == NORMAL_BYTES, 'misc changed before arm')
    once(ATTEMPT / '05_PRE_ARM_PASS.json', {'utc': now(), 'live': fresh, 'remote_payload_sha256': ONE_SHA})
    intent('05_ARM_MISC', {'offset': 2048, 'count': 32, 'sha256': ONE_SHA, 'maximum': 1})
    shell('arm_misc', 'dd if=' + REMOTE + ' of=/dev/block/by-name/misc bs=1 seek=2048 count=32 conv=notrunc', root=True)
    shell('sync_misc', 'sync', root=True)
    need(misc_read('misc_armed_readback') == ONE_BYTES, 'Armed misc mismatch; no automatic reboot')
    once(ATTEMPT / '06_ARMED_READBACK_PASS.json', {'utc': now(), 'misc_sha256': ONE_SHA, 'misc_bytes': ONE_BYTES.hex()})
    once(CONSUMED, {'consumed_utc': now(), 'candidate_sha256': NEW_SHA, 'candidate_execution_count_conservative': 1,
        'attempt_root': str(ATTEMPT), 'status': 'CONSUMED_BEFORE_TEST_REBOOT', 'retry': 'FORBIDDEN'})
    intent('07_TEST_REBOOT', {'candidate_sha256': NEW_SHA, 'maximum': 1})
    run('test_reboot', [ADB, '-s', SERIAL, 'reboot'])
    once(ATTEMPT / '08_DISPATCHED_ONCE.json', {'utc': now(), 'status': 'DISPATCHED_ONCE_AWAITING_OBSERVATION',
        'candidate_sha256': NEW_SHA, 'physical_success': 'NOT_ESTABLISHED', 'boot_a_flashes': 1, 'misc_writes': 1,
        'boot_b_writes': 0, 'vbmeta_writes': 0, 'set_active_commands': 0, 'erase_commands': 0, 'test_reboots': 1})
    return {'status': 'DISPATCHED_ONCE_AWAITING_OBSERVATION', 'candidate_sha256': NEW_SHA}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('phase', choices=['prepare', 'flash', 'boot'])
    phase = parser.parse_args().phase
    try:
        result = {'prepare': prepare, 'flash': flash, 'boot': boot}[phase]()
        print(json.dumps(result, indent=2))
    except Exception as error:
        if ATTEMPT.exists():
            failure = ATTEMPT / ('FAIL_' + phase + '_' + datetime.now(timezone.utc).strftime('%H%M%S%f') + '.json')
            once(failure, {'utc': now(), 'phase': phase, 'error': repr(error), 'retry': 'NO_AUTOMATIC_RETRY',
                'misc_may_be_armed': (ATTEMPT / '05_ARM_MISC.intent.json').exists(), 'consumed': CONSUMED.exists()})
        raise

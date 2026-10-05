"""Generate a separate one-use USB trial controller from the real frozen image.
No phase is executed and no device transport is invoked by this generator.
"""
import csv
import hashlib
import json
import re
from pathlib import Path

WORK = Path(__file__).resolve().parent
LAB = Path(r'C:\MOTO_G20_WOA_LAB')
ROOT = LAB / 'AUTOMATION/BUILD_ARTIFACTS/USB_HOST_20260929_V6'
STATE = LAB / 'AUTOMATION/STATE'
BASE_SHA = '298A3F182EA05FA5F8FACD9B1B77624BF317CC4FB086BF81C9C2B6B086E830F6'
PARENT = 'E1E431DA519E139644D1AE6C140B1FF0D0E62F58B42418ACFD299921341388F8'
SAFE_B = 'C0CFBCF4574F20F4028D7A4C5EE201FAD2C28F841201F94A0F48DD5ED76DF7F3'
REGISTRY_SHA = '61BA467929B5E50CC9CEF28C482F364A401782B38ADF40B6689D98055D12884A'
IMAGE = 'CANDIDATE/MOTO_G20_USB_HOST_HOST_ONLY.img'


def need(value, message):
    if not value:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest().upper()


def validate_identity(identity, registry):
    need(re.fullmatch('[0-9A-F]{64}', identity), 'Malformed candidate identity')
    need(identity not in (PARENT, SAFE_B), 'Candidate reuses a parent/safe image')
    need(identity not in registry.decode('utf-8').upper(), 'Candidate already in ledger')


def validate_manifest(root, rows, identity):
    seen = set()
    for row in rows:
        path = (root / row['relative_path']).resolve()
        need(path.is_relative_to(root.resolve()), 'Manifest escapes root')
        need(path not in seen, 'Duplicate manifest file')
        seen.add(path)
        need(path.stat().st_size == int(row['size']) and sha(path.read_bytes()) == row['sha256'], 'Frozen artifact drift')
    need((root / IMAGE).resolve() in seen, 'Candidate absent from manifest')
    need(sha((root / IMAGE).read_bytes()) == identity, 'Manifest candidate identity mismatch')


def replace_once(text, old, new):
    need(text.count(old) == 1, 'Source anchor not unique: ' + old[:70])
    return text.replace(old, new, 1)


def main():
    baseline = WORK.parent / 'console_physical_once.py'
    need(sha(baseline.read_bytes()) == BASE_SHA, 'Baseline controller changed')
    report = json.loads((ROOT / 'REPORTS/USB_CANDIDATE_HOST_AUDIT.json').read_text())
    need(report['status'] == 'PASS_HOST_STATIC_NOT_PHYSICALLY_TESTED' and report['physical_execution_count'] == 0, 'Not an untested host candidate')
    artifact = report['artifacts']['MOTO_G20_USB_HOST_HOST_ONLY.img']
    identity = artifact['sha256']
    registry = (STATE / 'candidate_execution_registry.jsonl').read_bytes()
    need(sha(registry) == REGISTRY_SHA and len(registry.splitlines()) == 18, 'Registry drift')
    validate_identity(identity, registry)
    need(Path(artifact['path']).resolve() == (ROOT / IMAGE).resolve(), 'Unexpected candidate path')
    need((ROOT / IMAGE).stat().st_size == artifact['size'] == 0x4000000 and sha((ROOT / IMAGE).read_bytes()) == identity, 'Candidate size/hash')
    for path in STATE.rglob('*.json'):
        need(identity not in path.name.upper() and identity not in path.read_text(encoding='utf-8-sig').upper(), 'Candidate already claimed')
    manifest = ROOT / 'FREEZE_MANIFEST.csv'
    rows = list(csv.DictReader(manifest.open(encoding='utf-8')))
    validate_manifest(ROOT, rows, identity)
    attempt_name = 'USB_HOST_PHYSICAL_ATTEMPT1_' + identity[:12]
    need(not (LAB / 'AUTOMATION/RESULTS' / attempt_name).exists(), 'Attempt exists')
    text = baseline.read_text()
    constants = {
        'ROOT': repr(str(ROOT)),
        'ATTEMPT': "LAB / 'AUTOMATION/RESULTS/" + attempt_name + "'",
        'NEW_SHA': repr(identity), 'OLD_SHA': repr(PARENT), 'REGISTRY_SHA': repr(REGISTRY_SHA),
        'MANIFEST_SHA': repr(sha(manifest.read_bytes())), 'CANDIDATE': 'ROOT / ' + repr(IMAGE),
        'REMOTE': repr('/data/local/tmp/usb_host_' + identity[:12].lower() + '_oneshot.bin'),
        'CLAIM_DIR': "STATE / '" + attempt_name + "_EXECUTION_CLAIMS'",
        'AUTH': repr('User authorized continuing Moto G20 USB work and flashing, with the phone available over ADB, and explicitly removed the battery-charge minimum for all future trials: ignora bateria siempre. Charge is recorded without gating. New V6 EP0 causal snapshots and detached published-HC diagnostic throttle; no session exhaustion USB hub/keyboard candidate; one bounded trial, safe Android B preserved. No reuse of consumed candidates.')}
    constants['ROOT'] = 'Path(' + constants['ROOT'] + ')'
    for key, value in constants.items():
        text, count = re.subn('(?m)^' + key + r' = .*$', lambda _: key + ' = ' + value, text)
        need(count == 1, 'Constant anchor: ' + key)
    text = replace_once(text, 'single-use GOP trial', 'single-use USB trial')
    text = replace_once(text, "need(len(rows) == 623, 'Manifest count')", "need(len(rows) == %d, 'Manifest count')" % len(rows))
    text = replace_once(text, "'files_verified': 623", "'files_verified': %d" % len(rows))
    text = replace_once(text, "level >= 70 and temp <= 420", "temp <= 420")
    text = replace_once(text, "prior['battery_percent'] >= 70 and prior['temperature_c'] <= 42", "prior['temperature_c'] <= 42")
    text = replace_once(text, "'minimum_percent': 70", "'minimum_percent': None, 'charge_gate': 'REMOVED_BY_USER_FOR_ALL_FUTURE_TRIALS'")
    text = replace_once(text, 'Authorized battery/USB gate not met', 'Phone temperature/USB gate not met')
    text = replace_once(text, 'Preclaim battery/USB gate not met', 'Preclaim phone temperature/USB gate not met')
    target = WORK / 'usb_physical_once.py'
    test_target = WORK / 'test_usb_physical_once.py'
    need(not target.exists() and not test_target.exists(), 'Do not replace a materialized controller/test')
    tests = (WORK.parent / 'test_console_physical_once.py').read_text()
    tests = replace_once(tests, 'import console_physical_once as c', 'import usb_physical_once as c')
    tests = replace_once(tests, '''    def test_blank_field_with_low_battery_is_rejected(self):
        self.fastboot_claim(update={'battery_percent': 69})
        with self.assertRaisesRegex(RuntimeError, 'battery/USB gate'):
            self.fastboot_reply('current-slot: \\nFinished. Total time: 0.000s\\n')''', '''    def test_blank_field_with_low_charge_is_accepted_under_user_policy(self):
        self.fastboot_claim(update={'battery_percent': 0})
        self.fastboot_reply('current-slot: \\nFinished. Total time: 0.000s\\n')
        self.assertTrue((self.attempt / 'EMPTY_GETVAR_OBSERVED.json').exists())''')
    tests = replace_once(tests, "if __name__ == '__main__':", '''    def test_phone_temperature_gate_still_applies(self):
        self.fastboot_claim(update={'battery_percent': 0, 'temperature_c': 43})
        with self.assertRaisesRegex(RuntimeError, 'temperature/USB gate'):
            self.fastboot_reply('current-slot: \\nFinished. Total time: 0.000s\\n')

    def test_usb_connection_gate_still_applies(self):
        self.fastboot_claim(update={'battery_percent': 0, 'usb_powered': False})
        with self.assertRaisesRegex(RuntimeError, 'temperature/USB gate'):
            self.fastboot_reply('current-slot: \\nFinished. Total time: 0.000s\\n')

if __name__ == '__main__':''')
    compile(text, str(target), 'exec')
    compile(tests, str(test_target), 'exec')
    with target.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(text)
    with test_target.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(tests)
    info = {'status': 'CONTROLLER_GENERATED_NO_DEVICE_IO', 'candidate_sha256': identity,
            'manifest_sha256': sha(manifest.read_bytes()), 'manifest_files': len(rows),
            'base_controller_sha256': BASE_SHA, 'controller_sha256': sha(target.read_bytes()),
            'controller_path': str(target), 'tests_sha256': sha(test_target.read_bytes()),
            'tests_path': str(test_target), 'device_calls': 0, 'physical_execution_count': 0}
    with (ROOT / 'CONTROLLER_PREPARATION.json').open('x', encoding='utf-8') as stream:
        json.dump(info, stream, indent=2)
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()

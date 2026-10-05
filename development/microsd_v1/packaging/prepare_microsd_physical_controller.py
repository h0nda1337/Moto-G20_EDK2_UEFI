"""Generate, but never execute, a controller for one frozen microSD trial.

audit/generate require a real completed candidate, successful replay and freeze.
render_controller/render_tests are pure functions used by fake-transport tests.
"""
import argparse
import csv
import hashlib
import json
import re
from pathlib import Path

import package_microsd_candidate as package

WORK = Path(__file__).resolve().parents[2]
BASE = WORK / 'console_physical_once.py'
BASE_TESTS = WORK / 'test_console_physical_once.py'
BASE_SHA = '298A3F182EA05FA5F8FACD9B1B77624BF317CC4FB086BF81C9C2B6B086E830F6'
BASE_TESTS_SHA = '58979348B63DA55764B869D5A9B58F18E97C8E9F9AB076A60F56E3F2FD209C5B'
PHYSICAL_PARENT = '21A0EAA08BE2E1F8C9A23C17845F935B7BA48EE3B26837835E200195BAE7D690'
SAFE_B = 'C0CFBCF4574F20F4028D7A4C5EE201FAD2C28F841201F94A0F48DD5ED76DF7F3'
REGISTRY_SHA = '61BA467929B5E50CC9CEF28C482F364A401782B38ADF40B6689D98055D12884A'
IMAGE = 'MOTO_G20_MICROSD_HOST_ONLY.img'
LAB = package.LAB
STATE = LAB / 'AUTOMATION/STATE'


def need(value, message):
    if not value:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest().upper()


def replace_once(text, before, after):
    need(text.count(before) == 1, 'Controller anchor not unique: ' + before[:80])
    return text.replace(before, after, 1)


def validate_identity(identity, registry):
    need(re.fullmatch('[0-9A-F]{64}', identity) is not None, 'Malformed candidate identity')
    need(identity not in (PHYSICAL_PARENT, package.PARENT_SHA, SAFE_B), 'Candidate reuses a parent/safe image')
    need(identity not in registry.decode('utf-8').upper(), 'Candidate already in ledger')


def validate_manifest(root, rows, identity):
    seen = set()
    need(rows, 'Empty freeze manifest')
    for row in rows:
        path = (root / row['relative_path']).resolve()
        need(path.is_relative_to(root.resolve()), 'Manifest escapes root')
        need(path not in seen, 'Duplicate manifest file')
        seen.add(path)
        need(path.stat().st_size == int(row['size']) and sha(path.read_bytes()) == row['sha256'], 'Frozen artifact drift')
    required = (IMAGE, 'ZERO_IDENTITY_TEMPLATE.img', 'FV_TRANSPORT.bin', 'WRAPPER_ZERO_IDENTITY.pe',
                'MOTOG20MICROSDFV.Fv', 'MICROSD_CANDIDATE_HOST_AUDIT.json', 'ACCEPTED_BUILD.json', 'PACKAGING_COMMANDS.json')
    need(all((root / name).resolve() in seen for name in required), 'Required candidate evidence absent from freeze')
    need(sha((root / IMAGE).read_bytes()) == identity, 'Frozen candidate identity mismatch')


def render_controller(baseline, root, identity, manifest_sha, manifest_count):
    """Return source only; this function never writes or contacts a device."""
    need(re.fullmatch('[0-9A-F]{64}', identity) is not None, 'Malformed candidate identity')
    need(re.fullmatch('[0-9A-F]{64}', manifest_sha) is not None and manifest_count >= 8, 'Invalid freeze metadata')
    attempt = 'MICROSD_PHYSICAL_ATTEMPT1_' + identity[:12]
    constants = {
        'ROOT': 'Path(' + repr(str(root)) + ')',
        'ATTEMPT': "LAB / 'AUTOMATION/RESULTS/" + attempt + "'",
        'NEW_SHA': repr(identity), 'OLD_SHA': repr(PHYSICAL_PARENT), 'REGISTRY_SHA': repr(REGISTRY_SHA),
        'MANIFEST_SHA': repr(manifest_sha), 'CANDIDATE': 'ROOT / ' + repr(IMAGE),
        'REMOTE': repr('/data/local/tmp/microsd_' + identity[:12].lower() + '_oneshot.bin'),
        'CLAIM_DIR': "STATE / '" + attempt + "_EXECUTION_CLAIMS'",
        'AUTH': repr('User authorized continuing Moto G20 firmware work and verified new-image trials, paused USB for microSD, supplied the microSD and phone over ADB, and removed battery-charge minimum for future trials. Charge is recorded without gating. First microSD read-only SD/FAT diagnostic; no Windows autolaunch. One bounded new-identity trial; safe Android B and SD contents preserved. No reuse of consumed candidates.')}
    text = baseline
    for key, value in constants.items():
        text, count = re.subn('(?m)^' + key + r' = .*$', lambda _: key + ' = ' + value, text)
        need(count == 1, 'Constant anchor: ' + key)
    replacements = [
        ('single-use GOP trial', 'single-use microSD trial'),
        ("need(len(rows) == 623, 'Manifest count')", "need(len(rows) == %d, 'Manifest count')" % manifest_count),
        ("'files_verified': 623", "'files_verified': %d" % manifest_count),
        ('level >= 70 and temp <= 420', 'temp <= 420'),
        ("prior['battery_percent'] >= 70 and prior['temperature_c'] <= 42", "prior['temperature_c'] <= 42"),
        ("'minimum_percent': 70", "'minimum_percent': None, 'charge_gate': 'REMOVED_BY_USER_FOR_ALL_FUTURE_TRIALS'"),
        ('Authorized battery/USB gate not met', 'Phone temperature/USB gate not met'),
        ('Preclaim battery/USB gate not met', 'Preclaim phone temperature/USB gate not met'),
    ]
    for before, after in replacements:
        text = replace_once(text, before, after)
    compile(text, 'microsd_physical_once.py', 'exec')
    return text


def render_tests(baseline):
    tests = replace_once(baseline, 'import console_physical_once as c', 'import microsd_physical_once as c')
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
    compile(tests, 'test_microsd_physical_once.py', 'exec')
    return tests


def inspect(root):
    need(sha(BASE.read_bytes()) == BASE_SHA and sha(BASE_TESTS.read_bytes()) == BASE_TESTS_SHA, 'Baseline controller/tests changed')
    need(root.is_dir(), 'No candidate package directory')
    report = json.loads((root / 'MICROSD_CANDIDATE_HOST_AUDIT.json').read_text())
    need(report['status'] == 'PASS_HOST_STATIC_NOT_PHYSICALLY_TESTED' and report['physical_execution_count'] == 0, 'Not an untested host candidate')
    artifact = report['artifacts'][IMAGE]
    identity = artifact['sha256']
    registry = (STATE / 'candidate_execution_registry.jsonl').read_bytes()
    need(sha(registry) == REGISTRY_SHA and len(registry.splitlines()) == 18, 'Registry changed')
    validate_identity(identity, registry)
    need(Path(artifact['path']).resolve() == (root / IMAGE).resolve(), 'Candidate path differs from package root')
    need((root / IMAGE).stat().st_size == artifact['size'] == 0x4000000, 'Candidate size')
    package.collision_check((identity,))
    manifest = root / 'FREEZE_MANIFEST.csv'
    rows = list(csv.DictReader(manifest.open(encoding='utf-8')))
    validate_manifest(root, rows, identity)
    replay = package.replay(root)
    need(replay['candidate_sha256'] == identity, 'Replay identity mismatch')
    attempt = LAB / 'AUTOMATION/RESULTS' / ('MICROSD_PHYSICAL_ATTEMPT1_' + identity[:12])
    need(not attempt.exists(), 'Attempt already exists')
    return {'candidate_sha256': identity, 'manifest_sha256': sha(manifest.read_bytes()),
            'manifest_files': len(rows), 'physical_parent_sha256': PHYSICAL_PARENT,
            'safe_android_b_sha256': SAFE_B, 'registry_sha256': REGISTRY_SHA,
            'replay': replay, 'device_calls': 0, 'physical_execution_count': 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('audit', 'generate'))
    parser.add_argument('--package-root', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = args.package_root.resolve()
    info = inspect(root)
    if args.command == 'generate':
        need(args.output is not None and not args.output.exists(), 'New --output directory required')
        output = args.output.resolve()
        need(not output.is_relative_to(root), 'Controller must stay outside frozen candidate root')
        text = render_controller(BASE.read_text(), root, info['candidate_sha256'], info['manifest_sha256'], info['manifest_files'])
        tests = render_tests(BASE_TESTS.read_text())
        output.mkdir(parents=True)
        target, test_target = output / 'microsd_physical_once.py', output / 'test_microsd_physical_once.py'
        with target.open('x', encoding='utf-8', newline='\n') as stream:
            stream.write(text)
        with test_target.open('x', encoding='utf-8', newline='\n') as stream:
            stream.write(tests)
        info.update({'status': 'CONTROLLER_GENERATED_NO_DEVICE_IO', 'base_controller_sha256': BASE_SHA,
                     'controller_path': str(target), 'controller_sha256': sha(target.read_bytes()),
                     'tests_path': str(test_target), 'tests_sha256': sha(test_target.read_bytes())})
        package.dump(output / 'CONTROLLER_PREPARATION.json', info)
    else:
        info['status'] = 'FROZEN_CANDIDATE_CONTROLLER_INPUTS_VALIDATED_NO_DEVICE_IO'
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()

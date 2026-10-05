"""Read-only audit of the completed one-shot command journal, not a boot result."""
import json
from pathlib import Path
import usb_physical_once as c

def read(path):
    return json.loads(path.read_text(encoding='utf-8'))

c.host()
c.claim_value()
dispatch = read(c.ATTEMPT / '08_DISPATCHED_ONCE.json')
consumed = read(c.CONSUMED)
assert dispatch['candidate_sha256'] == consumed['candidate_sha256'] == c.NEW_SHA
assert consumed['candidate_execution_count_conservative'] == 1
for name in ('04_FLASH_READBACK_PASS.json', '05_PRE_ARM_PASS.json'):
    live = read(c.ATTEMPT / name)['live']
    assert live['boot_sha256'] == {'boot_a': c.NEW_SHA, 'boot_b': c.SAFE_B}
    assert live['misc_hex'] == c.NORMAL_BYTES.hex()
    assert live['properties']['ro.boot.slot_suffix'] == '_b'
armed = read(c.ATTEMPT / '06_ARMED_READBACK_PASS.json')
assert armed['misc_sha256'] == c.ONE_SHA and armed['misc_bytes'] == c.ONE_BYTES.hex()
commands = []
for path in sorted((c.ATTEMPT / 'commands').glob('*.intent.json')):
    commands.append(read(path)['argv'])
flashes = [a for a in commands if 'flash' in a]
misc = [a for a in commands if any('of=/dev/block/by-name/misc' in x for x in a)]
trial = [a for a in commands if a == [str(c.ADB), '-s', c.SERIAL, 'reboot']]
assert len(flashes) == len(misc) == len(trial) == 1
assert flashes[0] == [str(c.FASTBOOT), '-s', c.SERIAL, 'flash', 'boot_a', str(c.CANDIDATE)]
assert not [a for a in commands if any(x in ('erase', 'set_active', 'boot_b', 'vbmeta') or 'of=/dev/block/by-name/boot_' in x for x in a)]
value = {'status': 'PASS_SINGLE_DISPATCH_JOURNAL', 'candidate_sha256': c.NEW_SHA,
    'recorded_utc': c.now(), 'dispatch_utc': dispatch['utc'], 'boot_a_flashes': 1,
    'misc_32_byte_writes': 1, 'test_reboots': 1, 'boot_b_writes': 0,
    'vbmeta_writes': 0, 'full_post_flash_readback': True, 'armed_misc_readback': True,
    'candidate_consumed': True, 'physical_shell_or_keyboard_success': 'AWAITING_USER_OBSERVATION',
    'device_calls_by_audit': 0, 'retry': 'FORBIDDEN'}
out = Path(__file__).resolve().parents[2] / 'outputs/usb_ep0_v6_20260929/DISPATCH_AUDIT.json'
with out.open('x', encoding='utf-8') as stream:
    json.dump(value, stream, indent=2)
print(json.dumps(value, indent=2))

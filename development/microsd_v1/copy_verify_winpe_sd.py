"""Copy staged media to the identity-checked FAT32 microSD and verify every hash."""
import hashlib
import json
import shlex
import subprocess
from pathlib import Path
from collect_android_sd_readonly import ADB, ROOT, SERIAL
from prepare_sd_card_android import CID
from stage_winpe_media import ROOT as STAGING, sha

OUT = ROOT / 'outputs/microsd_v1_20261002/CARD_PAYLOAD'


def main():
    OUT.mkdir(exist_ok=False)
    manifest = json.loads((STAGING / 'MANIFEST.json').read_text())
    media = Path(manifest['media'])
    assert media.resolve().is_relative_to(STAGING.resolve())
    assert manifest['count'] == 681 and manifest['bytes'] == 660079829
    for row in manifest['entries']:
        source = media / row['path']
        assert source.resolve().is_relative_to(media.resolve())
        assert source.stat().st_size == row['size'] and sha(source) == row['sha256']
    prep = json.loads((ROOT / 'outputs/microsd_v1_20261002/CARD_PREPARATION/FORMAT_STATE.json').read_text())
    assert prep['state'] == 'FAT32_FORMAT_AND_MOUNT_VERIFIED' and prep['cid'] == CID
    uuid = prep['volumes'].split('public:179,25 mounted ')[1].splitlines()[0]
    assert uuid == 'C003-08EC'
    destination = '/storage/' + uuid
    events = []

    def run(name, arguments, timeout=30):
        p = subprocess.run([str(ADB), '-s', SERIAL, *arguments], capture_output=True, timeout=timeout)
        (OUT / name).write_bytes(p.stdout + p.stderr)
        events.append({'name': name, 'args': arguments, 'exit_code': p.returncode,
                       'sha256': hashlib.sha256(p.stdout + p.stderr).hexdigest()})
        (OUT / 'COMMANDS.json').write_text(json.dumps(events, indent=2) + '\n')
        assert p.returncode == 0, name
        return p.stdout.decode(errors='replace')

    def rooted(name, command, timeout=30):
        command += '\nrc=$?; printf "\\n__SD_RC__%s\\n" "$rc"'
        text = run(name, ['exec-out', 'su -c ' + shlex.quote(command)], timeout)
        assert text.rstrip().endswith('__SD_RC__0'), name
        return text.rsplit('\n__SD_RC__', 1)[0].strip()

    def verify_identity(name):
        text = rooted(name, 'cat /sys/block/mmcblk1/device/cid; readlink -f /sys/block/mmcblk1/device; sm list-volumes all; blkid /dev/block/mmcblk1p1')
        assert text.splitlines()[0] == CID and '/71100000.sdio/' in text
        assert 'public:179,25 mounted ' + uuid in text and 'TYPE="vfat"' in text

    verify_identity('identity_before.txt')
    for index, source in enumerate(sorted(media.iterdir())):
        print('Copying ' + source.name, flush=True)
        verify_identity('identity_' + str(index) + '.txt')
        run('push_' + str(index) + '.txt', ['push', str(source), destination + '/'], 300)
    run('push_hashes.txt', ['push', str(STAGING / 'SHA256SUMS'), destination + '/G20-SHA256SUMS'])
    rooted('sync.txt', 'sync')
    verify_identity('identity_after.txt')
    print('Checking SHA256 of all 681 files on the microSD', flush=True)
    text = rooted('sha256_check.txt', 'cd ' + destination + ' && sha256sum -c G20-SHA256SUMS', 300)
    successes = [line for line in text.splitlines() if line.endswith(': OK')]
    assert len(successes) == 681 and len(text.splitlines()) == 681, 'Hash verification incomplete'
    marker = rooted('marker.txt', 'cat ' + destination + '/G20SD.TXT')
    assert marker == 'MOTO G20 MICROSD READ TEST V1'
    report = {'state': 'FAT32_MEDIA_COPIED_AND_681_HASHES_VERIFIED_IN_ANDROID', 'card_cid': CID,
              'volume_uuid': uuid, 'files_verified': len(successes), 'payload_bytes': manifest['bytes'],
              'manifest_sha256': sha(STAGING / 'MANIFEST.json'), 'uefi_boot_tested': False,
              'phone_rebooted': False, 'scope': 'microSD only', 'entry_path': '/EFI/BOOT/BOOTAA64.EFI'}
    (OUT / 'RESULT.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

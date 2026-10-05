"""Format only the user-authorized, identity-checked removable microSD via vold.

Run once. Never retry a completed/uncertain format automatically. No boot/eMMC I/O.
"""
import hashlib
import json
import shlex
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path

from collect_android_sd_readonly import ADB, ROOT, SERIAL

OUT = ROOT / 'outputs/microsd_v1_20261002/CARD_PREPARATION'
CID = '02544d53413332473030965a1b013200'
SECTORS = 60456960


def main():
    OUT.mkdir(exist_ok=False)
    events = []

    def run(name, command, binary=False, timeout=30):
        original = command
        if not binary:
            command += '\nrc=$?; printf "\\n__SD_RC__%s\\n" "$rc"'
        p = subprocess.run([str(ADB), '-s', SERIAL, 'exec-out', 'su -c ' + shlex.quote(command)],
                           capture_output=True, timeout=timeout)
        (OUT / name).write_bytes(p.stdout)
        events.append({'utc': datetime.now(timezone.utc).isoformat(), 'command': original,
                       'file': name, 'process_exit': p.returncode,
                       'sha256': hashlib.sha256(p.stdout).hexdigest(), 'stderr': p.stderr.decode(errors='replace')})
        (OUT / 'COMMANDS.json').write_text(json.dumps(events, indent=2) + '\n')
        assert p.returncode == 0, name
        if not binary:
            assert p.stdout.rstrip().endswith(b'__SD_RC__0'), name
            return p.stdout.rsplit(b'\n__SD_RC__', 1)[0].decode().strip()
        return p.stdout

    identity = run('identity.txt', 'getprop ro.product.device; getprop ro.boot.hardware.sku; id')
    assert 'java' in identity and 'XT2128-1' in identity and 'uid=0(root)' in identity
    device = run('card_identity.txt', 'cat /sys/block/mmcblk1/device/cid /sys/block/mmcblk1/device/type /sys/block/mmcblk1/size; readlink -f /sys/block/mmcblk1/device')
    lines = device.splitlines()
    assert lines[:3] == [CID, 'SD', str(SECTORS)], device
    assert '/71100000.sdio/' in lines[3] and lines[3].endswith('/mmc1/mmc1:1234'), device
    disks = run('disks.txt', 'sm list-disks').splitlines()
    assert 'disk:179,24' in disks, disks
    assert run('device_numbers.txt', 'cat /sys/block/mmcblk1/dev') == '179:24'
    run('before_volumes.txt', 'sm list-volumes all; cat /proc/partitions; cat /proc/mounts')
    # Retain partition metadata as evidence; this is not a backup of card files.
    first = run('before_first_mib.bin', 'dd if=/dev/block/mmcblk1 bs=512 count=2048 2>/dev/null', True)
    last = run('before_last_33_sectors.bin', 'dd if=/dev/block/mmcblk1 bs=512 skip=60456927 count=33 2>/dev/null', True)
    assert len(first) == 1048576 and len(last) == 16896
    authorization = {'scope': 'Only this removable microSD', 'cid': CID, 'sectors': SECTORS,
                     'user_authorization': 'tienes acceso a borrar todo y formatearla por si lo necesitas',
                     'purpose': 'FAT32 media for verified ARM64 WinPE; existing main partition is NTFS',
                     'state': 'FORMAT_DISPATCHED_DO_NOT_REPEAT_AUTOMATICALLY'}
    (OUT / 'FORMAT_STATE.json').write_text(json.dumps(authorization, indent=2) + '\n')
    run('format.txt', 'sm partition disk:179,24 public', timeout=55)
    for attempt in range(20):
        volumes = run('after_volumes.txt', 'sm list-volumes all')
        if 'public:179,25 mounted ' in volumes:
            break
        time.sleep(2)
    else:
        raise RuntimeError('Format sent, but mounted public volume not verified; do not repeat format')
    fs = run('after_filesystem.txt', 'blkid /dev/block/mmcblk1p1')
    assert 'TYPE="vfat"' in fs, fs
    run('after_partitions.txt', 'cat /proc/partitions; cat /proc/mounts')
    sector = run('fat_boot_sector.bin', 'dd if=/dev/block/mmcblk1p1 bs=512 count=1 2>/dev/null', True)
    assert len(sector) == 512 and sector[510:] == b'\x55\xaa' and sector[82:90] == b'FAT32   '
    assert int.from_bytes(sector[11:13], 'little') == 512
    assert run('after_cid.txt', 'cat /sys/block/mmcblk1/device/cid') == CID
    authorization.update(state='FAT32_FORMAT_AND_MOUNT_VERIFIED', filesystem=fs, volumes=volumes,
                         rebooted=False, internal_storage_formatted=False,
                         internal_boot_partitions_written=False)
    (OUT / 'FORMAT_STATE.json').write_text(json.dumps(authorization, indent=2) + '\n')
    print(json.dumps(authorization, indent=2))


if __name__ == '__main__':
    main()

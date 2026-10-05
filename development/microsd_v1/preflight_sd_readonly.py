"""Fresh read-only identity and payload check before a physical UEFI attempt."""
import argparse
import hashlib
import json
import shlex
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from collect_android_sd_readonly import ADB, SERIAL


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists()
    commands = []

    def read(command):
        wrapped = command + '\nrc=$?; printf "\\n__SD_RC__%s\\n" "$rc"'
        proc = subprocess.run([str(ADB), '-s', SERIAL, 'exec-out',
                               'su -c ' + shlex.quote(wrapped)],
                              capture_output=True, timeout=40)
        commands.append({'command': command, 'exit': proc.returncode,
                         'stdout': proc.stdout.decode(errors='replace'),
                         'stderr': proc.stderr.decode(errors='replace')})
        assert proc.returncode == 0 and proc.stdout.endswith(b'\n__SD_RC__0\n'), commands[-1]
        return proc.stdout[:-len(b'\n__SD_RC__0\n')]

    cid = read('cat /sys/block/mmcblk1/device/cid').decode().strip()
    assert cid == '02544d53413332473030965a1b013200'
    assert read('cat /sys/block/mmcblk1/size').decode().strip() == '60456960'
    assert '/71100000.sdio/' in read('readlink -f /sys/block/mmcblk1/device').decode()
    assert read('cat /storage/C003-08EC/G20SD.TXT') == b'MOTO G20 MICROSD READ TEST V1\r\n'
    loader = read('sha256sum /storage/C003-08EC/EFI/BOOT/BOOTAA64.EFI').decode().split()[0].upper()
    assert loader == '501FBC16120C43027D8ECAB4482F6571ADBDC9DD75048472FFC3031662BB1BE1'
    stats = read('stat -c %s /storage/C003-08EC/sources/boot.wim').decode().strip()
    assert stats == '562366404'
    result = {'utc': datetime.now(timezone.utc).isoformat(),
              'status': 'PASS_ANDROID_SD_IDENTITY_AND_BOOT_FILES',
              'cid': cid, 'loader_sha256': loader, 'wim_bytes': int(stats),
              'device_writes': 0, 'physical_uefi_result': 'NOT_TESTED', 'commands': commands}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
    print(result['status'])


if __name__ == '__main__':
    main()

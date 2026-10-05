"""Read SD configuration from the identified Moto G20; no mounts or device writes."""
import hashlib
import json
import re
import shlex
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ADB = Path(r'C:\Users\Angelo\AppData\Local\Microsoft\WinGet\Packages\Genymobile.scrcpy_Microsoft.Winget.Source_8wekyb3d8bbwe\scrcpy-win64-v4.1\adb.exe')
SERIAL = 'ZY32CVND77'
ROOT = Path(__file__).resolve().parents[2]


def main():
    assert hashlib.sha256(ADB.read_bytes()).hexdigest().upper() == '957E46B8615F7AF5B7292A2DDABE98D2E61940C3FB2B0545756507F080613E71'
    out = ROOT / 'outputs/microsd_v1_20261002' / ('ANDROID_' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    out.mkdir(exist_ok=False)
    records = []

    def read(name, command, required=False, binary=False):
        original = command
        if not binary:
            command += '\nrc=$?; printf "\\n__SD_RC__%s\\n" "$rc"'
        try:
            p = subprocess.run([str(ADB), '-s', SERIAL, 'exec-out', 'su -c ' + shlex.quote(command)], capture_output=True, timeout=30)
            raw, err, code = p.stdout, p.stderr, p.returncode
            if not binary:
                match = re.search(rb'\n__SD_RC__(\d+)\s*$', raw)
                assert match, name
                code = int(match.group(1)) if code == 0 else code
                raw = raw[:match.start()]
        except subprocess.TimeoutExpired as e:
            raw, err, code = e.stdout or b'', e.stderr or b'', 'TIMEOUT'
        assert len(raw) < 8 * 1024 * 1024
        (out / name).write_bytes(raw)
        records.append({'name': name, 'command': original, 'exit_code': code, 'size': len(raw),
                        'sha256': hashlib.sha256(raw).hexdigest(), 'stderr': err.decode(errors='replace')})
        (out / 'COMMANDS.json').write_text(json.dumps(records, indent=2) + '\n')
        if required:
            assert code == 0, name
        return raw

    identity = read('identity.txt', 'getprop ro.product.device; getprop ro.boot.hardware.sku; getprop ro.boot.slot_suffix; id', True)
    assert b'java' in identity and b'XT2128-1' in identity and b'uid=0(root)' in identity
    read('mmc_paths.txt', 'ls -ld /sys/class/mmc_host/* /sys/bus/mmc/devices/*; readlink -f /sys/class/mmc_host/mmc1; readlink -f /sys/class/mmc_host/mmc1/device')
    card = read('sd_card.txt', 'for d in /sys/bus/mmc/devices/mmc1:*; do echo "$d"; for n in type name cid csd ocr scr date manfid oemid serial hwrev fwrev erase_size preferred_erase_size; do if [ -f "$d/$n" ]; then echo "$n:"; cat "$d/$n"; fi; done; done')
    read('sd_block.txt', 'for d in /sys/bus/mmc/devices/mmc1:*/block/*; do echo "$d"; for n in dev size ro removable logical_block_size; do if [ -f "$d/$n" ]; then echo "$n:"; cat "$d/$n"; fi; done; ls -ld "$d"/*; done')
    read('sd_ios.txt', 'cat /sys/kernel/debug/mmc1/ios')
    read('volumes.txt', 'sm list-disks; sm list-volumes all; cat /proc/partitions; cat /proc/mounts')
    read('sd_filesystem.txt', 'for d in /sys/bus/mmc/devices/mmc1:*/block/*; do n=${d##*/}; for p in /dev/block/$n /dev/block/${n}p*; do if [ -b "$p" ]; then blkid "$p"; fi; done; done')
    read('regulator_summary.txt', 'cat /sys/kernel/debug/regulator/regulator_summary')
    read('sd_regulators_sysfs.txt', 'for d in /sys/class/regulator/*; do n=$(cat "$d/name" 2>/dev/null); case "$n" in vddsdcore|vddsdio) echo "$d $n"; for f in microvolts state num_users type; do echo "$f"; cat "$d/$f"; done;; esac; done')
    read('clk_summary.txt', 'cat /sys/kernel/debug/clk/clk_summary')
    read('gpio.txt', 'cat /sys/kernel/debug/gpio')
    read('pinctrl.txt', 'ls -l /sys/kernel/debug/pinctrl; for d in /sys/kernel/debug/pinctrl/*; do if [ -d "$d" ]; then echo "$d"; for n in pinmux-pins pinconf-pins gpio-ranges; do if [ -f "$d/$n" ]; then echo "$n"; cat "$d/$n"; fi; done; fi; done')
    read('sd_driver.txt', 'readlink -f /sys/bus/platform/devices/71100000.sdio/driver; ls -ld /sys/bus/platform/devices/711*; cat /sys/bus/platform/devices/711*/uevent')
    read('sd_dmesg.txt', 'dmesg | grep -i -E "mmc1|71100000|sdhci|sdhcr|sdcard|vddsd|mmcblk1|card detect"')
    read('filesystems.txt', 'cat /proc/filesystems')
    fdt = read('live_fdt.dtb', 'cat /sys/firmware/fdt', binary=True)
    assert not fdt or fdt[:4] == b'\xd0\x0d\xfe\xed'
    summary = {'status': 'READ_ONLY_SD_SNAPSHOT', 'directory': str(out), 'device_writes': 0, 'reboots': 0,
               'sd_sysfs_present': bool(re.search(rb'mmc1:[0-9a-fA-F]+', card)),
               'failures': [r['name'] for r in records if r['exit_code'] != 0]}
    (out / 'SUMMARY.json').write_text(json.dumps(summary, indent=2) + '\n')
    (out / 'COLLECTOR.py').write_bytes(Path(__file__).read_bytes())
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()

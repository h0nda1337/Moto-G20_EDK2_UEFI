"""Read bounded SD sectors to establish independent first-UEFI-read references."""
import hashlib
import json
import shlex
import struct
import subprocess
from collect_android_sd_readonly import ADB, ROOT, SERIAL
from prepare_sd_card_android import CID, SECTORS

OUT = ROOT / 'outputs/microsd_v1_20261002/CARD_SECTOR_BASELINE'


def main():
    OUT.mkdir(exist_ok=False)
    def shell(command):
        p = subprocess.run([str(ADB), '-s', SERIAL, 'exec-out', 'su -c ' + shlex.quote(command)], capture_output=True, timeout=30)
        assert p.returncode == 0
        return p.stdout
    identity = shell('cat /sys/block/mmcblk1/device/cid /sys/block/mmcblk1/size; readlink -f /sys/block/mmcblk1/device').decode().splitlines()
    assert identity[0] == CID and identity[1] == str(SECTORS) and '/71100000.sdio/' in identity[2]
    rows = []
    def read(name, lba, count=1):
        assert 0 <= lba < SECTORS and 1 <= count <= 128 and lba + count <= SECTORS
        command = f'dd if=/dev/block/mmcblk1 bs=512 skip={lba} count={count} 2>/dev/null'
        data = shell(command)
        assert len(data) == count * 512
        (OUT / name).write_bytes(data)
        rows.append({'file': name, 'lba': lba, 'sectors': count, 'sha256': hashlib.sha256(data).hexdigest(), 'command': command})
        return data
    mbr = read('mbr.bin', 0)
    assert mbr[510:] == b'\x55\xaa'
    ptype, start, length = mbr[450], *struct.unpack_from('<II', mbr, 454)
    assert ptype in (0x0b, 0x0c) and 0 < start < SECTORS and start + length <= SECTORS
    assert all(mbr[446 + 16*i + 4] == 0 for i in (1, 2, 3))
    boot = read('fat32_boot.bin', start)
    bps, spc = struct.unpack_from('<HB', boot, 11)
    reserved, fats = struct.unpack_from('<HB', boot, 14)
    total, spf, root_cluster = struct.unpack_from('<I', boot, 32)[0], struct.unpack_from('<I', boot, 36)[0], struct.unpack_from('<I', boot, 44)[0]
    assert bps == 512 and spc in (1,2,4,8,16,32,64,128) and fats in (1,2)
    assert boot[82:90] == b'FAT32   ' and boot[510:] == b'\x55\xaa' and total <= length
    data_start = start + reserved + fats * spf
    root_lba = data_start + (root_cluster - 2) * spc
    assert root_cluster >= 2 and root_lba + spc <= start + total
    root = read('root_cluster.bin', root_lba, spc)
    found = []
    for offset in range(0, len(root), 32):
        entry = root[offset:offset+32]
        if entry[0] == 0:
            break
        if entry[:11] != b'G20SD   TXT' or entry[11] & 0x18:
            continue
        cluster = (struct.unpack_from('<H', entry, 20)[0] << 16) | struct.unpack_from('<H', entry, 26)[0]
        size = struct.unpack_from('<I', entry, 28)[0]
        assert cluster >= 2 and size == 31
        marker_lba = data_start + (cluster - 2) * spc
        assert marker_lba < start + total
        marker = read('marker_sector.bin', marker_lba)
        assert marker[:size] == b'MOTO G20 MICROSD READ TEST V1\r\n'
        found.append({'lba': marker_lba, 'cluster': cluster, 'bytes': size})
    assert len(found) == 1
    assert shell('cat /sys/block/mmcblk1/device/cid').decode().strip() == CID
    report = {'status': 'RAW_SD_MBR_FAT32_AND_MARKER_VERIFIED_IN_ANDROID', 'cid': CID,
              'capacity_sectors': SECTORS, 'partition_start_lba': start, 'partition_sectors': length,
              'sectors_per_cluster': spc, 'marker': found[0], 'reads': rows,
              'writes': 0, 'uefi_read_verified': False}
    (OUT / 'RESULT.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

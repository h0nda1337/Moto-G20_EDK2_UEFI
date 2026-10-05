"""Embed exact Android reference sectors and revalidate them on the real card."""
import hashlib
import json
import shlex
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'microsd_v1'))
from collect_android_sd_readonly import ADB, SERIAL


def main():
    source = HERE.parents[1] / 'outputs/microsd_v1_20261002/CARD_SECTOR_BASELINE'
    rows = json.loads((source / 'RESULT.json').read_text())['reads']
    sources = [next(row for row in rows if row['file'] == name)
               for name in ('mbr.bin', 'fat32_boot.bin', 'marker_sector.bin')]
    arrays = []
    for row in sources:
        raw = (source / row['file']).read_bytes()
        assert len(raw) == 512 and hashlib.sha256(raw).hexdigest() == row['sha256']
        proc = subprocess.run([str(ADB), '-s', SERIAL, 'exec-out',
                               'su -c ' + shlex.quote(row['command'])],
                              capture_output=True, timeout=30)
        assert proc.returncode == 0 and proc.stdout == raw, row['file']
        arrays.append('  {\n' + '\n'.join('    ' + ','.join(f'0x{x:02x}' for x in raw[i:i+16]) + ','
                                         for i in range(0, 512, 16)) + '\n  },')
    target = HERE / 'bds/MotoG20SdSectorReferences.h'
    with target.open('x', encoding='utf-8') as stream:
        stream.write('/* Exact read-only references captured in Android; no filesystem parser assumptions. */\n'
                     'STATIC CONST UINT8 mSdSectorReferences[3][512] = {\n' + '\n'.join(arrays) + '\n};\n')
    print('Three Android reference sectors still match; generated C reference arrays.')


if __name__ == '__main__':
    main()

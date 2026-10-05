"""Static cross-check of the extracted WinPE chain; never executes PE files."""
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'outputs/winpe_internal_20261002'


def inspect_pe(path):
    raw = path.read_bytes()
    assert raw[:2] == b'MZ', path
    offset = struct.unpack_from('<I', raw, 0x3c)[0]
    assert raw[offset:offset + 4] == b'PE\0\0', path
    machine = struct.unpack_from('<H', raw, offset + 4)[0]
    subsystem = struct.unpack_from('<H', raw, offset + 24 + 68)[0]
    assert machine == 0xaa64, (path, machine)
    return {'path': str(path.relative_to(OUT)), 'size': len(raw),
            'machine': '0xAA64', 'architecture': 'ARM64', 'subsystem': subsystem,
            'sha256': hashlib.sha256(raw).hexdigest().upper()}


def main():
    raw_bcd = json.loads((OUT / 'BCD_RAW.json').read_text(encoding='utf-8-sig'))
    loader_id = '{7619dcc9-fafe-11d9-b411-000476eba25f}'
    ramdisk_id = '{7619dcc8-fafe-11d9-b411-000476eba25f}'

    def value(object_id, element):
        suffix = f'\\Objects\\{object_id}\\Elements\\{element}'
        matches = [row for row in raw_bcd if row['key'].endswith(suffix)]
        assert len(matches) == 1, suffix
        return matches[0]['value']

    loader_path = value(loader_id, '12000002')
    sdi_path = value(ramdisk_id, '32000004')
    assert loader_path.lower() == r'\windows\system32\boot\winload.efi'
    assert sdi_path.lower() == r'\boot\boot.sdi'
    device = bytes.fromhex(value(loader_id, '11000001'))
    osdevice = bytes.fromhex(value(loader_id, '21000001'))
    assert device == osdevice
    wim_path = r'\sources\boot.wim'
    assert (wim_path + '\0').encode('utf-16le') in device
    assert (OUT / 'media' / sdi_path.lstrip('\\')).is_file()
    assert (OUT / 'media' / wim_path.lstrip('\\')).is_file()
    exact_loader = OUT / 'wim_boot_exact' / loader_path.lstrip('\\')
    assert exact_loader.is_file()

    names = [
        'media/efi/boot/bootaa64.efi',
        'media/bootmgr.efi',
        'wim_boot_exact/Windows/Boot/EFI/bootmgfw.efi',
        'wim_boot_exact/Windows/System32/Boot/winload.efi',
        'wim_selected/Windows/System32/ntoskrnl.exe',
        'wim_selected/Windows/System32/hal.dll',
    ]
    images = [inspect_pe(OUT / name) for name in names]
    assert images[0]['subsystem'] == 10  # EFI_APPLICATION
    assert images[1]['subsystem'] == 16  # WINDOWS_BOOT_APPLICATION
    result = {
        'status': 'STATIC_BOOT_PATHS_AND_ARM64_CONFIRMED',
        'efi_entry': r'\EFI\BOOT\BOOTAA64.EFI',
        'bcd': r'\EFI\Microsoft\Boot\BCD',
        'loader_object': loader_id,
        'ramdisk_object': ramdisk_id,
        'loader_path_inside_wim': loader_path,
        'wim_path_on_boot_volume': wim_path,
        'sdi_path_on_boot_volume': sdi_path,
        'device_osdevice_equal': True,
        'all_referenced_files_found': True,
        'images': images,
        'device_io': 'NONE',
        'binary_execution': False,
        'physical_boot_result': 'NOT_TESTED',
        'limits': 'Valid static paths and ARM64 do not establish firmware or board compatibility.'
    }
    (OUT / 'BOOT_CHAIN_AUDIT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

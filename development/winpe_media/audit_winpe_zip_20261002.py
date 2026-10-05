"""Read and CRC-check the supplied ZIP; extract boot data for static inspection."""
import hashlib
import json
import stat
import struct
import zipfile
from pathlib import Path, PurePosixPath

SOURCE = Path(r'C:\Users\Angelo\Downloads\Windows20h2pe_new.zip')
OUT = Path(__file__).resolve().parents[1] / 'outputs/winpe_internal_20261002'
EXPECTED = '317F4D0218A3F150026D1DDE1ED9D5EE022228E372CA6EB9F052E38DE8172721'

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''): h.update(chunk)
    return h.hexdigest().upper()

def pe(raw):
    if raw[:2] != b'MZ': return None
    off = struct.unpack_from('<I', raw, 0x3c)[0]
    if raw[off:off+4] != b'PE\0\0': return None
    machine = struct.unpack_from('<H', raw, off + 4)[0]
    opt = off + 24
    return {'machine_hex': hex(machine), 'architecture': {0xaa64: 'ARM64', 0x8664: 'AMD64', 0x14c: 'I386', 0x1c4: 'ARM32'}.get(machine, 'OTHER'),
            'optional_magic': hex(struct.unpack_from('<H', raw, opt)[0]),
            'subsystem': struct.unpack_from('<H', raw, opt + 68)[0]}

def main():
    assert digest(SOURCE) == EXPECTED
    OUT.mkdir(exist_ok=True)
    data = OUT / 'media'
    assert not data.exists(), 'Preserve original extraction; do not overwrite'
    data.mkdir()
    rows, seen, selected = [], set(), []
    with zipfile.ZipFile(SOURCE) as z:
        assert sum(i.file_size for i in z.infolist()) < 2 * 1024**3
        for i in z.infolist():
            p = PurePosixPath(i.filename)
            assert not p.is_absolute() and '..' not in p.parts and '\\' not in i.filename
            assert not any(':' in part for part in p.parts)
            assert p.parts[0] == '20h2pe_new'
            assert not stat.S_ISLNK(i.external_attr >> 16) and not i.flag_bits & 1
            key = i.filename.casefold()
            assert key not in seen
            seen.add(key)
            if i.is_dir(): continue
            rel = PurePosixPath(*p.parts[1:])
            keep = key.endswith(('.wim', '.efi', '/bcd', '/boot.sdi')) or '/efi/' in key
            target = data / Path(*rel.parts)
            assert target.resolve().is_relative_to(data.resolve())
            if keep:
                target.parent.mkdir(parents=True, exist_ok=True)
                dest = target.open('xb')
            else: dest = None
            h, size = hashlib.sha256(), 0
            try:
                with z.open(i) as f:
                    for chunk in iter(lambda: f.read(1024 * 1024), b''):
                        h.update(chunk); size += len(chunk)
                        if dest: dest.write(chunk)
            finally:
                if dest: dest.close()
            assert size == i.file_size
            row = {'path': rel.as_posix(), 'size': size, 'sha256': h.hexdigest().upper(),
                   'crc32': f'{i.CRC:08X}', 'extracted': keep}
            if keep and key.endswith('.efi'):
                row['pe'] = pe(target.read_bytes())
                selected.append(row)
            rows.append(row)
    result = {'status': 'PASS_ZIP_CRC_AND_STATIC_INVENTORY', 'source': str(SOURCE),
              'zip_sha256': EXPECTED, 'zip_size': SOURCE.stat().st_size,
              'files': len(rows), 'uncompressed_bytes': sum(r['size'] for r in rows),
              'efi': selected, 'entries': rows,
              'limits': 'Integrity and architecture check, not malware or physical boot certification.',
              'device_io': 'NONE', 'archive_binaries_executed': False}
    (OUT / 'ZIP_AUDIT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k:v for k,v in result.items() if k != 'entries'}, indent=2))

if __name__ == '__main__': main()

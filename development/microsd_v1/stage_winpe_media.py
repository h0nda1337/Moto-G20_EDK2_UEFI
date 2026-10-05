"""Safely stage the previously audited ARM64 ZIP on G:; never execute its files."""
import hashlib
import json
import shutil
import stat
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(r'G:\MOTO_G20_WOA_LAB\SDCARD_PAYLOAD_20261002')
ZIP = Path(r'C:\Users\Angelo\Downloads\Windows20h2pe_new.zip')
EXPECTED = '317F4D0218A3F150026D1DDE1ED9D5EE022228E372CA6EB9F052E38DE8172721'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest().upper()


def main():
    assert sha(ZIP) == EXPECTED
    assert shutil.disk_usage(ROOT.anchor).free > 2 * 1024**3
    extracted = ROOT / 'media'
    media = extracted / '20h2pe_new'
    media.mkdir(parents=True, exist_ok=True)
    rows, seen = [], set()
    with zipfile.ZipFile(ZIP) as archive:
        for entry in archive.infolist():
            name = entry.filename.replace('\\', '/')
            rel = PurePosixPath(name)
            assert not rel.is_absolute() and all(p not in ('..', '.', '') and ':' not in p for p in rel.parts)
            assert rel.parts[0] == '20h2pe_new'
            assert all(not p.endswith(('.', ' ')) for p in rel.parts)
            assert not stat.S_ISLNK(entry.external_attr >> 16)
            target = extracted.joinpath(*rel.parts)
            assert target.resolve().is_relative_to(media.resolve()) or (target == media and entry.is_dir())
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            assert name.casefold() not in seen
            seen.add(name.casefold())
            assert entry.file_size < 0x100000000
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                with archive.open(entry) as source:
                    expected_hash = hashlib.file_digest(source, 'sha256').hexdigest().upper()
                assert sha(target) == expected_hash, 'Never overwrite differing staged content'
            else:
                with archive.open(entry) as source, target.open('xb') as dest:
                    shutil.copyfileobj(source, dest, 1024 * 1024)
            assert target.stat().st_size == entry.file_size
            rows.append({'path': PurePosixPath(*rel.parts[1:]).as_posix(), 'size': entry.file_size, 'sha256': sha(target), 'source': 'AUDITED_ZIP'})
    assert len(rows) == 680 and sum(r['size'] for r in rows) == 660079798
    assert sha(media / 'efi/boot/bootaa64.efi') == '501FBC16120C43027D8ECAB4482F6571ADBDC9DD75048472FFC3031662BB1BE1'
    assert (media / 'sources/boot.wim').stat().st_size == 562366404
    marker = media / 'G20SD.TXT'
    if marker.exists():
        assert marker.read_bytes() == b'MOTO G20 MICROSD READ TEST V1\r\n'
    else:
        marker.write_bytes(b'MOTO G20 MICROSD READ TEST V1\r\n')
    rows.append({'path': marker.name, 'size': marker.stat().st_size, 'sha256': sha(marker), 'source': 'SD_READ_TEST_MARKER'})
    report = {'state': 'STAGED_NOT_BOOT_TESTED', 'source_zip_sha256': EXPECTED,
              'media': str(media), 'count': len(rows), 'bytes': sum(r['size'] for r in rows),
              'entries': rows}
    (ROOT / 'MANIFEST.json').write_text(json.dumps(report, indent=2) + '\n')
    # Simple safe paths also allow a single Android sha256sum verification run.
    for row in rows:
        assert '\n' not in row['path'] and '\\' not in row['path']
    (ROOT / 'SHA256SUMS').write_text(''.join(r['sha256'].lower() + '  ' + r['path'] + '\n' for r in rows), newline='\n')
    (ROOT / 'STAGER.py').write_bytes(Path(__file__).read_bytes())
    print(json.dumps({k: v for k, v in report.items() if k != 'entries'}, indent=2))


if __name__ == '__main__':
    main()

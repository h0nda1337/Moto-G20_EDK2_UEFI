"""Freeze a completed candidate and exact recorded build inputs; no device I/O."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import package_microsd_candidate as package


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    manifest = root / 'FREEZE_MANIFEST.csv'
    package.need(not manifest.exists(), 'Already frozen')
    replay = package.replay(root)
    accepted = json.loads((root / 'ACCEPTED_BUILD.json').read_text())
    records = [*accepted['files'], accepted['registry']]
    for module in accepted['modules']:
        records.extend(module['evidence'])
    captured = {}
    for row in records:
        source = Path(row['path']).resolve()
        raw = package.checked(row)
        relative = Path('EVIDENCE') / source.drive.rstrip(':') / Path(*source.parts[1:])
        destination = root / relative
        package.need(destination.resolve().is_relative_to(root), 'Source capture escapes candidate')
        if destination in captured:
            package.need(captured[destination] == row['sha256'], 'Conflicting source records')
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open('xb') as stream:
            stream.write(raw)
        captured[destination] = row['sha256']
    package.dump(root / 'PRE_FREEZE_REPLAY.json', replay)
    rows = []
    for path in sorted(root.rglob('*')):
        if not path.is_file():
            continue
        rows.append({'relative_path': path.relative_to(root).as_posix(),
                     'size': path.stat().st_size,
                     'sha256': hashlib.sha256(path.read_bytes()).hexdigest().upper()})
    with manifest.open('x', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=('relative_path', 'size', 'sha256'))
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps({'status': 'FROZEN_HOST_CANDIDATE_NOT_DEVICE_TESTED',
                      'files': len(rows), 'candidate': replay['candidate_sha256'],
                      'manifest_sha256': package.sha(manifest.read_bytes())}, indent=2))


if __name__ == '__main__':
    main()

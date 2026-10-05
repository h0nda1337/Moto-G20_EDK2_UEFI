#!/usr/bin/env python3
"""Download only the pinned project baseline, with a SHA256 check."""
import hashlib
import json
import urllib.request
from pathlib import Path

root = Path(__file__).resolve().parent.parent
baseline = json.loads((root / 'dependencies.lock.json').read_text())['baseline']
target = root / 'artifacts' / baseline['asset']
target.parent.mkdir(exist_ok=True)
url = 'https://github.com/h0nda1337/Moto-G20_EDK2_UEFI/releases/download/' + baseline['release_tag'] + '/' + baseline['asset']
if not target.exists():
    temporary = target.with_suffix('.partial')
    urllib.request.urlretrieve(url, temporary)
    if hashlib.sha256(temporary.read_bytes()).hexdigest().upper() != baseline['sha256']:
        raise SystemExit('Downloaded baseline checksum mismatch; partial file retained')
    temporary.replace(target)
if hashlib.sha256(target.read_bytes()).hexdigest().upper() != baseline['sha256']:
    raise SystemExit('Existing baseline checksum mismatch')
print(target)

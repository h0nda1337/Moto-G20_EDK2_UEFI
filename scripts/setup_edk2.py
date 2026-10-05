#!/usr/bin/env python3
"""Validate the pinned EDK2 checkout and apply the Moto G20 patch once."""
import hashlib
import json
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent.parent
lock = json.loads((root / 'dependencies.lock.json').read_text())['edk2']
edk = root / 'edk2'
patch = root / lock['patch']

def git(*args, check=True):
    return subprocess.run(['git', '-C', str(edk), *args], check=check,
                          text=True, capture_output=True)

if git('rev-parse', 'HEAD').stdout.strip() != lock['commit']:
    raise SystemExit('EDK2 is not at the commit in dependencies.lock.json')
if hashlib.sha256(patch.read_bytes()).hexdigest().upper() != lock['patch_sha256']:
    raise SystemExit('EDK2 patch checksum mismatch')
if git('apply', '--check', str(patch), check=False).returncode == 0:
    git('apply', str(patch))
    print('Moto G20 EDK2 patch applied')
elif git('apply', '--reverse', '--check', str(patch), check=False).returncode == 0:
    print('Moto G20 EDK2 patch already applied')
else:
    raise SystemExit('EDK2 differs from both the pinned original and patched source')

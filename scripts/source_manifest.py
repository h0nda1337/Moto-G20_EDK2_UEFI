#!/usr/bin/env python3
"""Hash build inputs using repository-relative paths."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent
paths = [root / 'dependencies.lock.json', root / 'patches/edk2-motog20.patch']
for folder in ('MotoG20Pkg', 'config'):
    paths += [p for p in (root / folder).rglob('*') if p.is_file()]
print(json.dumps([{'path': p.relative_to(root).as_posix(),
                   'sha256': hashlib.sha256(p.read_bytes()).hexdigest().upper()}
                  for p in sorted(paths)], indent=2))

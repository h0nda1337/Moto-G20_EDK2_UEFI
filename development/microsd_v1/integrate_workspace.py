"""Idempotently sync the microSD development sources into the isolated G: build.

No compilation, firmware packaging or device access is performed. The lock is
shared with build_microsd.sh so source files cannot change during compilation.
"""
import hashlib
import json
import os
import re
import tempfile
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = Path(r'G:\MOTO_G20_WOA_LAB\BUILD_ARTIFACTS\MICROSD_20261002_V1')
BASE = Path(r'C:\MOTO_G20_WOA_LAB\AUTOMATION\BUILD_ARTIFACTS\USB_HOST_20260929_V6\SOURCE\MotoG20Pkg')


def sd_diagnostic_boot_policy():
    """First hardware image holds the result before a Shell can clear it."""
    relative = 'Library/MotoG20PlatformBootManagerLib/MotoG20PlatformBootManagerLib.c'
    text = (BASE / relative).read_text(encoding='utf-8')
    declaration = 'STATIC BOOLEAN  mShellLaunchAttempted;'
    assert text.count(declaration) == 1
    text = text.replace(declaration, 'VOID MotoG20SdProbe (VOID);\n\n' + declaration, 1)
    after = text.index('PlatformBootManagerAfterConsole (')
    pos = text.index('  Status = LaunchBuiltInShell ();', after)
    inserted = '''  // MICROSD_DIAGNOSTIC_HOLD: run before StartImage(Shell), which normally
  // never returns and clears the console. This image proves SD reading only.
  Status = gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Print (L"\\r\\nMICROSD V1: hardware read test; watchdog=%r\\r\\n", Status);
  MotoG20SdProbe ();
  Print (L"\\r\\nMICROSD: test finished; capture this screen.\\r\\n");
  Print (L"Windows PE has NOT been started. Forced restart returns to Android.\\r\\n");
  for (;;) {
    gBS->Stall (100000);
  }

'''
    text = text[:pos] + inserted + text[pos:]
    assert text.count('  MotoG20SdProbe ();') == 1
    return relative, text


def ensure_inf_entries(text, section, entries):
    lines = text.splitlines()
    headers = [i for i, line in enumerate(lines) if line.strip() == f'[{section}]']
    if len(headers) != 1:
        raise ValueError(f'Expected exactly one [{section}] section')
    start = headers[0] + 1
    end = next((i for i in range(start, len(lines)) if lines[i].startswith('[')), len(lines))
    current = [line.strip().split()[0] for line in lines[start:end]
               if line.strip() and not line.lstrip().startswith('#')]
    additions = []
    for entry in entries:
        if current.count(entry) > 1:
            raise ValueError(f'Duplicate INF entry: {entry}')
        if entry not in current:
            additions.append('  ' + entry)
    lines[start:start] = additions
    return '\n'.join(lines) + '\n'


def insert_once_after(text, anchor, inserted):
    if text.count(anchor) != 1:
        raise ValueError(f'Expected exactly one insertion anchor: {anchor}')
    count = text.count(inserted)
    if count == 0:
        return text.replace(anchor, anchor + '\n' + inserted, 1)
    if count != 1 or anchor + '\n' + inserted not in text:
        raise ValueError(f'Unexpected or duplicate integration: {inserted}')
    return text


def plan_sources(root):
    """Validate every edit before writing; also usable against a test fixture."""
    package = root / 'WORKSPACE/MotoG20Pkg'
    bds = package / 'Diagnostics/BdsDxe'
    driver = package / 'Drivers/MotoG20SdDxe'
    platform = package / 'Drivers/MotoG20SdPlatformDxe'
    plan = {}
    for folder, names in (
        ('core', ('sdhci_ro.c', 'sdhci_ro.h')),
        ('driver', ('MotoG20SdDxe.c', 'MotoG20SdDxe.inf',
                    'MotoG20SdDiagnostic.h', 'MotoG20SdPlatform.h')),
    ):
        for name in names:
            plan[driver / name] = (HERE / folder / name).read_bytes()
    for name in ('MotoG20SdPlatformDxe.c', 'MotoG20SdPlatformDxe.inf',
                 'MotoG20SdPlatformDiagnostic.h', 'sd_platform_engine.c',
                 'sd_platform_engine.h'):
        plan[platform / name] = (HERE / 'platform' / name).read_bytes()
    plan[platform / 'MotoG20SdPlatform.h'] = (HERE / 'driver/MotoG20SdPlatform.h').read_bytes()
    plan[platform / 'sdhci_ro.h'] = (HERE / 'core/sdhci_ro.h').read_bytes()
    for name in ('MotoG20SdProbe.c', 'MotoG20SdProbe.h'):
        plan[bds / name] = (HERE / 'bds' / name).read_bytes()
    plan[bds / 'MotoG20SdDiagnostic.h'] = (HERE / 'driver/MotoG20SdDiagnostic.h').read_bytes()
    for name in ('MotoG20SdPlatformDiagnostic.h', 'sd_platform_engine.h'):
        plan[bds / name] = (HERE / 'platform' / name).read_bytes()
    plan[bds / 'sdhci_ro.h'] = (HERE / 'core/sdhci_ro.h').read_bytes()
    inf = bds / 'BdsDxe.inf'
    text = inf.read_text(encoding='utf-8')
    text = ensure_inf_entries(text, 'Sources',
                              ('MotoG20SdProbe.c', 'MotoG20SdProbe.h', 'MotoG20SdDiagnostic.h',
                               'MotoG20SdPlatformDiagnostic.h', 'sd_platform_engine.h', 'sdhci_ro.h'))
    text = ensure_inf_entries(text, 'Protocols',
                              ('gEfiSimpleFileSystemProtocolGuid', 'gEfiDevicePathProtocolGuid'))
    option = '  GCC:*_*_AARCH64_CC_FLAGS = -DSDRO_EDK2'
    if '[BuildOptions]' not in text:
        text += '\n[BuildOptions]\n' + option + '\n'
    elif option not in text:
        raise ValueError('Unexpected BDS build options')
    plan[inf] = text.encode('utf-8')
    entry = bds / 'BdsEntry.c'
    text = entry.read_text(encoding='utf-8')
    text = text.replace('#include "MotoG20SdProbe.h"\n', '')
    anchor = '  PERF_INMODULE_END ("PlatformBootManagerAfterConsole");'
    if len(re.findall(r'^\s*PlatformBootManagerAfterConsole\s*\(\s*\);\s*$', text, re.M)) != 1:
        raise ValueError('Expected one PlatformBootManagerAfterConsole invocation')
    # Remove the old unreachable call after AfterConsole; that callback launches
    # Shell synchronously. Put the probe in the platform policy before launch.
    if text.count('  MotoG20SdProbe ();') > 1:
        raise ValueError('Duplicate obsolete probe calls')
    text = text.replace(anchor + '\n  MotoG20SdProbe ();', anchor)
    if 'MotoG20SdProbe ();' in text:
        raise ValueError('Unexpected BDS probe call location')
    plan[entry] = text.encode('utf-8')
    relative, policy = sd_diagnostic_boot_policy()
    plan[package / relative] = policy.encode('utf-8')
    dsc = package / 'Foundation/MotoG20MicroSd.dsc'
    text = dsc.read_text(encoding='utf-8')
    text = ensure_inf_entries(text, 'Components',
                             ('MotoG20Pkg/Drivers/MotoG20SdPlatformDxe/MotoG20SdPlatformDxe.inf',))
    plan[dsc] = text.encode('utf-8')
    for destination in plan:
        if not destination.resolve().is_relative_to(package.resolve()):
            raise ValueError(f'Destination escaped isolated package: {destination}')
    return plan


def atomic_write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=path.name + '.', suffix='.tmp',
                                     dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(content)
    try:
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def integrate(root=ROOT):
    root = root.resolve()
    if root != ROOT.resolve():
        raise ValueError('Only the isolated microSD G: build is a live integration target')
    if not (root / 'WORKSPACE/MotoG20Pkg').is_dir():
        raise ValueError('Prepare the isolated workspace first')
    lock = root / 'BUILD_TRANCHE.lock'
    lock.mkdir()  # Fail if a compilation or another integration owns the lock.
    try:
        plan = plan_sources(root)
        changed = []
        files = []
        for destination, content in plan.items():
            relative = destination.relative_to(root).as_posix()
            if not destination.exists() or destination.read_bytes() != content:
                atomic_write(destination, content)
                changed.append(relative)
            if destination.read_bytes() != content:
                raise RuntimeError(f'Read-back mismatch: {destination}')
            files.append({'path': relative, 'size': len(content),
                          'sha256': hashlib.sha256(content).hexdigest().upper()})
        report = {'utc': datetime.now(timezone.utc).isoformat(),
                  'status': 'SOURCES_INTEGRATED_NOT_COMPILED_NOT_DEVICE_TESTED',
                  'root': str(root), 'changed': changed, 'files': files,
                  'probe_marker': 'MOTO G20 MICROSD READ TEST V1\r\n',
                  'probe_marker_bytes': len(b'MOTO G20 MICROSD READ TEST V1\r\n'),
                  'probe_max_bytes_read': 128,
                  'probe_open_mode': 'EFI_FILE_MODE_READ',
                  'probe_path_prefix': 'VenHw(45766337-a136-4b88-93e6-162167e94d0d)/SD(0)',
                  'automatic_image_start': False, 'device_access': False,
                  'probe_execution': 'Before Shell; diagnostic hold until external reset',
                  'pending': ['EDK2 compile of changed sources',
                              'Host mock tests for BDS filesystem/error cases',
                              'Physical SD read with real platform provider']}
        atomic_write(root / 'REPORTS/SOURCE_INTEGRATION.json',
                     (json.dumps(report, indent=2) + '\n').encode('utf-8'))
        return report
    finally:
        lock.rmdir()


if __name__ == '__main__':
    print(json.dumps(integrate(), indent=2))

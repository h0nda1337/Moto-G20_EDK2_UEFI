#!/usr/bin/env python3
"""Check that host fixtures exercise the default V4 firmware sources.

Only line endings are normalized. This is a source/boot-policy check, not a
compiler or hardware test. No laboratory paths or external files are required.
"""
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEV = Path('development/microsd_sdma_v4')
PKG = Path('MotoG20Pkg')


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8')


def need(condition, message):
    if not condition:
        raise ValueError(message)


def pairs():
    driver = PKG / 'Drivers/MotoG20SdDxe'
    platform = PKG / 'Drivers/MotoG20SdPlatformDxe'
    bds = PKG / 'Diagnostics/BdsDxe'
    for folder, names in (
        ('core', ('sdhci_ro.c', 'sdhci_ro.h')),
        ('driver', ('MotoG20SdDxe.c', 'MotoG20SdDxe.inf',
                    'MotoG20SdDiagnostic.h', 'MotoG20SdPlatform.h',
                    'sd_dma_buffer.c', 'sd_dma_buffer.h')),
    ):
        for name in names:
            yield DEV / folder / name, driver / name
    for name in ('MotoG20SdPlatformDxe.c', 'MotoG20SdPlatformDxe.inf',
                 'MotoG20SdPlatformDiagnostic.h', 'sd_platform_engine.c',
                 'sd_platform_engine.h'):
        yield DEV / 'platform' / name, platform / name
    yield DEV / 'driver/MotoG20SdPlatform.h', platform / 'MotoG20SdPlatform.h'
    yield DEV / 'core/sdhci_ro.h', platform / 'sdhci_ro.h'
    for name in ('MotoG20SdProbe.c', 'MotoG20SdProbe.h',
                 'MotoG20RawSdProbe.c', 'MotoG20RawSdProbe.h',
                 'MotoG20SdSectorReferences.h',
                 'MotoG20StorageConnect.c', 'MotoG20StorageConnect.h'):
        yield DEV / 'bds' / name, bds / name
    yield DEV / 'driver/MotoG20SdDiagnostic.h', bds / 'MotoG20SdDiagnostic.h'
    for name in ('MotoG20SdPlatformDiagnostic.h', 'sd_platform_engine.h'):
        yield DEV / 'platform' / name, bds / name
    yield DEV / 'core/sdhci_ro.h', bds / 'sdhci_ro.h'
    for name in ('sd_platform_engine.c', 'sd_platform_engine.h'):
        yield DEV / 'platform' / name, Path('development/microsd_v1/platform') / name


def main():
    checked = []
    for source, active in pairs():
        text = read(source)
        need(text == read(active), f'Source mismatch: {source} != {active}')
        checked.append({'source': source.as_posix(), 'matches': active.as_posix(),
                        'lf_sha256': hashlib.sha256(text.encode('utf-8')).hexdigest()})

    policy = read(PKG / 'Library/MotoG20PlatformBootManagerLib/MotoG20PlatformBootManagerLib.c')
    callback = policy[policy.index('PlatformBootManagerAfterConsole ('):]
    need(policy.count('  MotoG20SdProbe ();') == 1, 'Expected one SD probe invocation')
    watchdog = callback.index('SetWatchdogTimer (0, 0, 0, NULL)')
    banner = callback.index('MICROSD V4: SDMA sector and filesystem test')
    probe = callback.index('  MotoG20SdProbe ();')
    finished = callback.index('test finished; capture this screen.')
    pending = callback.index('Windows PE has NOT been started.')
    hold = callback.index('for (;;)', probe)
    shell = callback.index('  Status = LaunchBuiltInShell ();')
    need(watchdog < banner < probe < finished < pending < hold < shell,
         'V4 diagnostic must disable watchdog, run, report and hold before Shell')
    need('MotoG20SdProbe ();' not in read(PKG / 'Diagnostics/BdsDxe/BdsEntry.c'),
         'Unexpected duplicate or unreachable BDS probe')
    probe_source = read(DEV / 'bds/MotoG20SdProbe.c')
    need(probe_source.index('RawPassed = MotoG20RawSdProbe ();') <
         probe_source.index('if (!RawPassed)') <
         probe_source.index('Status = MotoG20StorageConnect ();'),
         'Exact raw-sector proof must precede filesystem connection')
    need('EfiBootManagerConnectAll' not in probe_source, 'Unexpected global storage connection')

    dsc = read(PKG / 'Foundation/MotoG20MicroSd.dsc')
    sections = re.findall(r'^\[Components\]\s*\n(.*?)(?=^\[|\Z)', dsc, re.M | re.S)
    need(len(sections) == 1, 'Expected one component section')
    components = [line.strip() for line in sections[0].splitlines()
                  if line.strip() and not line.lstrip().startswith('#')]
    expected = [
        'MotoG20Pkg/Drivers/MotoG20SdPlatformDxe/MotoG20SdPlatformDxe.inf',
        'MotoG20Pkg/Drivers/MotoG20SdDxe/MotoG20SdDxe.inf',
        'MotoG20Pkg/Diagnostics/BdsDxe/BdsDxe.inf',
        'MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf',
        'MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf',
        'FatPkg/EnhancedFatDxe/Fat.inf',
    ]
    need(sorted(components) == sorted(expected), 'Default build must contain exactly six SD modules')
    print(json.dumps({'status': 'PASS_SOURCE_AND_BOOT_POLICY_ONLY',
                      'compared_pairs': len(checked), 'pairs': checked,
                      'boot_policy': 'V4_READ_PROOF_AND_HOLD_BEFORE_SHELL',
                      'physical_success': False}, indent=2))


if __name__ == '__main__':
    main()

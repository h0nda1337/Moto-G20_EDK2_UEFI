"""Offline microSD FV packaging. No ADB, fastboot, device or partition operations.

Only a complete successful build containing the platform provider can be sealed.
The 23-file console FV is the baseline, never the USB FV. All 22 non-BDS files
remain identical. Packaging proves structure and provenance, not hardware function.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import uuid
from pathlib import Path

sys.dont_write_bytecode = True
LAB = Path(r'C:\MOTO_G20_WOA_LAB')
DEFAULT_BUILD = Path(r'G:\MOTO_G20_WOA_LAB\BUILD_ARTIFACTS\MICROSD_20261002_V1')
EDK = LAB / '05_EDK2/edk2'
PARENT = LAB / 'AUTOMATION/BUILD_ARTIFACTS/CONSOLE_DIAGNOSTIC_20260914_V2'
V6 = LAB / 'AUTOMATION/BUILD_ARTIFACTS/USB_HOST_20260929_V6'
PARENT_SHA = 'B158F348BAF58A12660EDF15FA5B6CAD7C41312BC3E32C07AC68509070E9E1F4'
FV_SHA = 'AB9E218C74F6A0A9B579C1879AC59CA86220F02C0175D2691CE8F04F3E2A689D'
V6_MANIFEST_SHA = '467C7AC92A6EBC3798056140318458F88D930CF492567B312820372E0A78ED45'
FV_SIZE, IMAGE_SIZE, TRANSPORT_OFFSET, TRANSPORT_SIZE = 0x400000, 0x4000000, 0x1224000, 0x8C2314
IDENTITY, DIGEST = slice(0x2BDA0, 0x2BDC0), slice(0x3A1A8, 0x3A1C8)
BDS = '6D33944A-EC75-4855-A54D-809C75241F6C'
SD = '9FDB62D3-FA35-4E77-9025-9291880F2C58'
PLATFORM_GUID = '6311F7E2-0881-4D02-9DDE-1FE254B62090'
USB_GUIDS = {'6181AF65-A2CF-494F-BCB1-9127784B8258', '240612B7-A063-11D4-9A3A-0090273FC14D',
             '2D2E62CF-9ECF-43B7-8219-94E7FC713DFE'}
FIXED = [
    ('BdsDxe', BDS, 'MotoG20Pkg/Diagnostics/BdsDxe/BdsDxe.inf', 'DXE_DRIVER'),
    ('MotoG20SdDxe', SD, 'MotoG20Pkg/Drivers/MotoG20SdDxe/MotoG20SdDxe.inf', 'DXE_DRIVER'),
    ('DiskIoDxe', '6B38F7B4-AD98-40E9-9093-ACA2B5A253C4', 'MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf', 'UEFI_DRIVER'),
    ('PartitionDxe', '1FA1F39E-FEFF-4AAE-BD7B-38A070A3B609', 'MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf', 'UEFI_DRIVER'),
    ('Fat', '961578FE-B6B7-44C3-AF35-6BC705CD2B1F', 'FatPkg/EnhancedFatDxe/Fat.inf', 'UEFI_DRIVER'),
]


def need(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest().upper()


def align(value, boundary):
    return (value + boundary - 1) & -boundary


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def record(path):
    data = path.read_bytes()
    return {'path': str(path.resolve()), 'size': len(data), 'sha256': sha(data)}


def checked(row):
    data = Path(row['path']).read_bytes()
    need(len(data) == row['size'] and sha(data) == row['sha256'], 'File changed: ' + row['path'])
    return data


def wsl(path):
    path = path.resolve()
    need(re.fullmatch('[A-Za-z]:', path.drive) is not None, 'Expected Windows drive path')
    need('\n' not in str(path) and '\r' not in str(path), 'Invalid path')
    return '/mnt/' + path.drive[0].lower() + '/' + path.as_posix()[3:]


def pe_audit(data):
    need(len(data) >= 0x40 and data[:2] == b'MZ', 'PE DOS signature')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    need(pe + 24 <= len(data) and data[pe:pe + 4] == b'PE\0\0', 'PE signature/bounds')
    machine, nsections = struct.unpack_from('<HH', data, pe + 4)
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    need(machine == 0xaa64 and 0 < nsections <= 96, 'PE must be ARM64 with bounded sections')
    need(optional_size >= 112 and opt + optional_size + nsections * 40 <= len(data), 'PE optional header/section table')
    need(struct.unpack_from('<H', data, opt)[0] == 0x20b, 'PE32+ required')
    entry, image_size, header_size = (struct.unpack_from('<I', data, opt + offset)[0] for offset in (16, 56, 60))
    subsystem = struct.unpack_from('<H', data, opt + 68)[0]
    need(subsystem == 11 and 0 < entry < image_size and header_size <= len(data), 'EFI boot service driver header')
    executable_entry, spans = False, []
    for index in range(nsections):
        start = opt + optional_size + index * 40
        vsize, va, rawsize, rawoff = struct.unpack_from('<IIII', data, start + 8)
        flags = struct.unpack_from('<I', data, start + 36)[0]
        need(va + max(vsize, rawsize) <= image_size, 'PE virtual section bounds')
        if rawsize:
            need(rawoff >= header_size and rawoff + rawsize <= len(data), 'PE raw section bounds')
            need(all(rawoff + rawsize <= a or rawoff >= b for a, b in spans), 'PE overlapping raw sections')
            spans.append((rawoff, rawoff + rawsize))
        executable_entry |= bool(flags & 0x20000000 and va <= entry < va + max(vsize, rawsize))
    need(executable_entry, 'PE entry outside executable section')
    return {'machine': 'AA64', 'subsystem': subsystem, 'entry_rva': entry, 'image_size': image_size, 'sections': nsections}


def parse_fv(raw, expected_count=None):
    raw = bytes(raw)
    need(len(raw) == FV_SIZE and raw[40:44] == b'_FVH', 'FV size/signature')
    need(struct.unpack_from('<Q', raw, 32)[0] == len(raw), 'FV declared size')
    header = struct.unpack_from('<H', raw, 48)[0]
    need(header == 0x48 and raw[55] == 2, 'FV header version/size')
    need(sum(struct.unpack_from('<36H', raw)) & 0xffff == 0, 'FV header checksum')
    cursor, result = align(header, 8), []
    while cursor + 24 <= len(raw):
        if raw[cursor:cursor + 24] == b'\xff' * 24:
            need(raw[cursor:] == b'\xff' * (len(raw) - cursor), 'FV free-space content')
            break
        header_data = raw[cursor:cursor + 24]
        size = int.from_bytes(header_data[20:23], 'little')
        need(not header_data[19] & 1 and 24 <= size <= len(raw) - cursor, 'FFS bounds/large-file flag')
        need((sum(header_data) - header_data[17] - header_data[23]) & 0xff == 0, 'FFS header checksum')
        need(header_data[23] == 0xf8, 'FFS data-valid state')
        data = raw[cursor + 24:cursor + size]
        need(((sum(data) + header_data[17]) & 0xff == 0) if header_data[19] & 0x40 else header_data[17] == 0xaa, 'FFS data checksum')
        if header_data[18] != 0xf0:
            sections, pos = [], 0
            while pos < len(data):
                need(pos + 4 <= len(data), 'Truncated section')
                count = int.from_bytes(data[pos:pos + 3], 'little')
                need(4 <= count < 0xffffff and pos + count <= len(data), 'Section bounds')
                sections.append({'type': data[pos + 3], 'data': data[pos + 4:pos + count]})
                end = pos + count
                pos = align(end, 4)
                if pos > len(data):
                    need(end == len(data), 'Section tail')
                    break
            result.append({'guid': str(uuid.UUID(bytes_le=header_data[:16])).upper(), 'type': header_data[18],
                           'offset': cursor, 'raw': raw[cursor:cursor + size], 'sections': sections})
        cursor = align(cursor + size, 8)
    need(expected_count is None or len(result) == expected_count, 'FV file count')
    need(len({row['guid'] for row in result}) == len(result), 'Duplicate FFS GUID')
    need(not USB_GUIDS.intersection(row['guid'] for row in result), 'USB module forbidden')
    return result


def baseline_inputs():
    need(sha((V6 / 'FREEZE_MANIFEST.csv').read_bytes()) == V6_MANIFEST_SHA, 'Frozen V6 manifest drift')
    parent = (PARENT / 'CANDIDATE/MOTO_G20_CONSOLE_FLAT_GOP_HOST_ONLY.img').read_bytes()
    baseline = (PARENT / 'FV/MOTOG20FOUNDATIONFV.Fv').read_bytes()
    need(sha(parent) == PARENT_SHA and sha(baseline) == FV_SHA, 'Verified console baseline drift')
    need(len(parent) == IMAGE_SIZE and parent[:8] == b'ANDROID!', 'Baseline Android header')
    need(parent[TRANSPORT_OFFSET:TRANSPORT_OFFSET + FV_SIZE] == baseline, 'Baseline FV placement')
    need(struct.unpack_from('<I', parent, 16)[0] == TRANSPORT_SIZE, 'Baseline transport header')
    need(parent[DIGEST] == hashlib.sha256(parent[TRANSPORT_OFFSET:TRANSPORT_OFFSET + TRANSPORT_SIZE]).digest(), 'Baseline digest')
    zero = bytearray(parent)
    zero[IDENTITY] = bytes(32)
    need(parent[IDENTITY] == hashlib.sha256(zero).digest(), 'Baseline canonical identity')
    parse_fv(baseline, 23)
    return parent, baseline


def inf_definitions(path):
    text = path.read_text(encoding='utf-8-sig')
    pairs = dict(re.findall(r'^\s*(BASE_NAME|FILE_GUID|MODULE_TYPE)\s*=\s*([^\s#]+)', text, re.M))
    need(len(pairs) == 3, 'Incomplete INF definitions: ' + str(path))
    pairs['FILE_GUID'] = str(uuid.UUID(pairs['FILE_GUID'])).upper()
    return pairs


def build_inputs(root, tag, provider_inf):
    root = root.resolve()
    need(not (root / 'BUILD_TRANCHE.lock').exists(), 'Build running')
    need(re.fullmatch('[A-Za-z0-9_-]+', tag) is not None, 'Invalid build tag')
    workspace, package = root / 'WORKSPACE', root / 'WORKSPACE/MotoG20Pkg'
    provider = (workspace / provider_inf).resolve()
    need(provider.is_relative_to(package.resolve()) and provider.suffix.lower() == '.inf', 'Platform INF outside MotoG20Pkg')
    need(provider.is_file(), 'Missing platform provider: no microSD candidate can be packaged')
    platform = inf_definitions(provider)
    need(platform == {'BASE_NAME': 'MotoG20SdPlatformDxe', 'FILE_GUID': PLATFORM_GUID,
                      'MODULE_TYPE': 'DXE_DRIVER'}, 'Invalid SD platform provider identity')
    platform_row = (platform['BASE_NAME'], platform['FILE_GUID'], provider.relative_to(workspace).as_posix(), 'DXE_DRIVER')
    specs = [*FIXED[:2], platform_row, *FIXED[2:]]
    need(len({row[1] for row in specs}) == 6 and not USB_GUIDS.intersection(row[1] for row in specs), 'Module GUID collision')
    run = root / 'REPORTS' / ('BUILD_' + tag)
    need((run / 'exit_code.txt').read_text().strip() == '0', 'Build did not succeed')
    log = (run / 'build.log').read_text(errors='replace')
    need('- Done -' in log and '- Failed -' not in log, 'No completed EDK2 build')
    recorded = json.loads((run / 'sources.json').read_text())
    actual = [{'path': p.relative_to(package).as_posix(), 'sha256': sha(p.read_bytes())}
              for p in sorted(package.rglob('*')) if p.is_file()]
    need(len({r['path'] for r in recorded}) == len(recorded), 'Duplicate build source path')
    need(sorted(recorded, key=lambda r: r['path']) == sorted(actual, key=lambda r: r['path']),
         'Workspace differs from build-start snapshot; rebuild required')
    dsc = package / 'Foundation/MotoG20MicroSd.dsc'
    dsc_text = dsc.read_text()
    blocks = re.findall(r'^\[Components\]\s*$(.*?)(?=^\[|\Z)', dsc_text, re.M | re.S)
    need(len(blocks) == 1, 'One explicit Components block required')
    components = [line.split('#', 1)[0].strip() for line in blocks[0].splitlines()]
    components = [line for line in components if line]
    need(sorted(components) == sorted(row[2] for row in specs), 'DSC component set differs from reviewed six modules')
    need('Usb' not in dsc_text and 'USB' not in dsc_text, 'USB included in SD DSC')
    build = root / 'BUILD/DEBUG_GCCNOLTO/AARCH64'
    modules = []
    for name, guid, relative_inf, kind in specs:
        origin = workspace if relative_inf.startswith('MotoG20Pkg/') else EDK
        inf = origin / relative_inf
        need(inf_definitions(inf) == {'BASE_NAME': name, 'FILE_GUID': guid, 'MODULE_TYPE': kind}, 'INF identity changed: ' + name)
        module_dir = build / Path(relative_inf).parent / Path(relative_inf).stem
        pe = build / (name + '.efi')
        data = pe.read_bytes()
        need(data == (module_dir / 'OUTPUT' / (name + '.efi')).read_bytes(), 'Top-level PE differs from module output')
        depex_path = module_dir / 'OUTPUT' / (name + '.depex')
        if kind == 'DXE_DRIVER':
            depex = depex_path.read_bytes()
            need(0 < len(depex) <= 4096 and depex[-1] == 8, 'DXE DEPEX missing/invalid end')
        else:
            need(not depex_path.exists(), 'Stock UEFI_DRIVER unexpected DEPEX')
        evidence = [record(inf), record(pe), record(module_dir / 'OUTPUT' / (name + '.efi'))]
        for path in sorted(module_dir.rglob('*')):
            if path.is_file() and (path.suffix in ('.map', '.depex', '.deps') or path.name in ('AutoGen.c', 'AutoGen.h', 'Makefile')):
                evidence.append(record(path))
        modules.append({'name': name, 'guid': guid, 'kind': kind, 'pe': record(pe),
                        'depex': record(depex_path) if kind == 'DXE_DRIVER' else None,
                        'pe_audit': pe_audit(data), 'evidence': evidence})
    protocol_bytes = uuid.UUID('0ad366d1-94dd-47c2-bd56-530e0187764d').bytes_le
    for index in (1, 2):
        need(protocol_bytes in checked(modules[index]['pe']), 'Platform protocol GUID absent from provider or consumer PE')
    files = [record(run / name) for name in ('exit_code.txt', 'build.log', 'sources.json', 'toolchain.txt')]
    files += [record(p) for p in sorted(package.rglob('*')) if p.is_file()]
    files += [record(root / 'CONF' / name) for name in ('target.txt', 'tools_def.txt', 'build_rule.txt')]
    registry = LAB / 'AUTOMATION/STATE/candidate_execution_registry.jsonl'
    result = {'status': 'PASS_OFFLINE_COMPLETE_MODULE_INPUTS', 'root': str(root), 'tag': tag,
              'provider_inf': provider.relative_to(workspace).as_posix(), 'modules': modules,
              'files': files, 'registry': record(registry), 'parent_image_sha256': PARENT_SHA,
              'baseline_fv_sha256': FV_SHA, 'physical_result': 'NOT_TESTED',
              'source_binding_limit': 'Custom workspace bound to build-start snapshot; shared libraries/configuration captured at seal time, not an independently reproduced toolchain build.'}
    need(not (root / 'BUILD_TRANCHE.lock').exists(), 'Build started during audit')
    return result


def check_acceptance(path):
    report = json.loads(path.read_text())
    need(report['status'] == 'PASS_OFFLINE_COMPLETE_MODULE_INPUTS', 'Acceptance status')
    for row in [*report['files'], report['registry']]:
        checked(row)
    current = build_inputs(Path(report['root']), report['tag'], report['provider_inf'])
    need(current == report, 'Accepted build changed')
    return report


def module_bytes(report):
    return {row['guid']: {**row, 'data': checked(row['pe']),
                         'depex_data': checked(row['depex']) if row['depex'] else None}
            for row in report['modules']}


def expected_sections(module):
    result = []
    if module['depex_data'] is not None:
        result.append({'type': 0x13, 'data': module['depex_data']})
    return result + [{'type': 0x10, 'data': module['data']}, {'type': 0x15, 'data': (module['name'] + '\0').encode('utf-16-le')}]


def verify_fv(raw, baseline, modules):
    need(sha(baseline) == FV_SHA, 'Baseline FV digest')
    before = parse_fv(baseline, 23)
    additions = [g for g in modules if g != BDS]
    need(len(modules) == 6 and SD in additions and len(additions) == 5, 'Required six modules')
    after = parse_fv(raw, 28)
    need([r['guid'] for r in after] == [r['guid'] for r in before] + additions, 'FV GUID order/set')
    need(after[0]['offset'] == before[0]['offset'] and raw[:after[0]['offset']] == baseline[:before[0]['offset']], 'FV prefix changed')
    preserved = []
    for old, new in zip(before, after[:23]):
        if old['guid'] != BDS:
            need(old['raw'] == new['raw'], 'Unapproved baseline FFS change: ' + old['guid'])
            preserved.append({'guid': old['guid'], 'sha256': sha(old['raw'])})
        else:
            need(old['type'] == new['type'] == 7, 'BDS FFS type')
            desired = [{'type': s['type'], 'data': modules[BDS]['data'] if s['type'] == 0x10 else modules[BDS]['depex_data'] if s['type'] == 0x13 else s['data']} for s in old['sections']]
            need(new['sections'] == desired, 'BDS sections mismatch')
    for new in after[23:]:
        need(new['type'] == 7 and new['sections'] == expected_sections(modules[new['guid']]), 'Added FFS mismatch')
    need(len(preserved) == 22, 'Preserved FFS count')
    used = max(align(r['offset'] + len(r['raw']), 8) for r in after)
    return {'ffs_count': 28, 'preserved_ffs': preserved, 'added_guids': additions,
            'fv_sha256': sha(raw), 'fv_size': len(raw), 'free_tail_bytes': len(raw) - used}


def verify_image(image, parent, transport, template, fv):
    need(len(image) == len(parent) == len(template) == IMAGE_SIZE, 'Boot image 64 MiB')
    need(image[:8] == b'ANDROID!' and struct.unpack_from('<I', image, 16)[0] == TRANSPORT_SIZE, 'Android transport header')
    need(len(transport) == TRANSPORT_SIZE and len(fv) == FV_SIZE, 'Transport/FV bounds')
    need(transport[:FV_SIZE] == fv and not any(transport[FV_SIZE:]), 'FV transport zero tail')
    need(image[TRANSPORT_OFFSET:TRANSPORT_OFFSET + TRANSPORT_SIZE] == transport, 'FV transport placement')
    need(image[DIGEST] == hashlib.sha256(transport).digest(), 'Transport SHA256 binding')
    normalized = bytearray(image)
    normalized[IDENTITY] = bytes(32)
    need(normalized == template and image[IDENTITY] == hashlib.sha256(template).digest(), 'Canonical candidate identity')
    cursor = 0
    for start, end in [(IDENTITY.start, IDENTITY.stop), (DIGEST.start, DIGEST.stop), (TRANSPORT_OFFSET, TRANSPORT_OFFSET + FV_SIZE), (IMAGE_SIZE, IMAGE_SIZE)]:
        need(image[cursor:start] == parent[cursor:start], 'Unapproved boot image delta')
        cursor = end


def collision_check(identities):
    state = LAB / 'AUTOMATION/STATE'
    for path in (state / 'candidate_execution_registry.jsonl', *state.rglob('*.json')):
        content = path.read_text(encoding='utf-8-sig').upper()
        need(not any(value in content or value in path.name.upper() for value in identities), 'Candidate identity already claimed: ' + str(path))


def package(acceptance, output):
    report = check_acceptance(acceptance)
    parent, baseline = baseline_inputs()
    modules = module_bytes(report)
    need(not output.exists(), 'Refusing existing output directory')
    output.mkdir(parents=True)
    folder = output / 'FVINPUT'
    folder.mkdir()
    commands = []

    def run(tool, args):
        exe = EDK / 'BaseTools/Source/C/bin' / tool
        proc = subprocess.run(['wsl.exe', '-d', 'Debian', '--exec', wsl(exe), *args], capture_output=True, text=True, timeout=90)
        commands.append({'tool': record(exe), 'args': args, 'exit_code': proc.returncode, 'stdout': proc.stdout, 'stderr': proc.stderr})
        dump(output / 'PACKAGING_COMMANDS.json', commands)
        need(proc.returncode == 0, tool + ' failed: ' + proc.stderr)

    original_inf = (PARENT / 'FVINPUT/console_new.inf').read_text()
    need(original_inf.count('[files]') == 1, 'Baseline INF files block')
    prefix = original_inf.split('[files]', 1)[0]
    ext = folder / 'MOTOG20FOUNDATIONFV.ext'
    ext.write_bytes((PARENT / 'FVINPUT/MOTOG20FOUNDATIONFV.ext').read_bytes())
    prefix, count = re.subn(r'(?m)^EFI_FV_EXT_HEADER_FILE_NAME\s*=.*$', 'EFI_FV_EXT_HEADER_FILE_NAME = ' + wsl(ext), prefix)
    need(count == 1, 'Baseline extended header reference')
    before = parse_fv(baseline, 23)
    original_paths = []
    for row in before:
        data = bytearray(row['raw'])
        data[23] ^= 0xff  # GenFv complements input state for erase polarity 1.
        path = folder / (row['guid'] + '.ffs')
        path.write_bytes(data)
        original_paths.append(path)

    def make_fv(name, paths):
        inf = folder / (name + '.inf')
        inf.write_text(prefix + '[files]\n' + ''.join('EFI_FILE_NAME = ' + wsl(p) + '\n' for p in paths), newline='\n')
        dest = output / (name + '.Fv')
        run('GenFv', ['-i', wsl(inf), '-o', wsl(dest)])
        return dest.read_bytes()

    need(make_fv('baseline_reproduced', original_paths) == baseline, 'Baseline reproduction not byte-identical')
    final_paths = list(original_paths)
    for guid, module in modules.items():
        section_folder = folder / module['name']
        section_folder.mkdir()
        if guid == BDS:
            old = next(r for r in before if r['guid'] == BDS)
            sections = [{'type': s['type'], 'data': module['data'] if s['type'] == 0x10 else module['depex_data'] if s['type'] == 0x13 else s['data']} for s in old['sections']]
        else:
            sections = expected_sections(module)
        args = []
        for index, section in enumerate(sections):
            dest = section_folder / (str(index) + '.section')
            if section['type'] in (0x10, 0x13):
                payload = section_folder / (str(index) + '.payload')
                payload.write_bytes(section['data'])
                kind = 'EFI_SECTION_PE32' if section['type'] == 0x10 else 'EFI_SECTION_DXE_DEPEX'
                run('GenSec', ['-s', kind, '-o', wsl(dest), wsl(payload)])
            else:
                dest.write_bytes((len(section['data']) + 4).to_bytes(3, 'little') + bytes([section['type']]) + section['data'])
            args += ['-oi', wsl(dest)]
        ffs = section_folder / (guid + '.ffs')
        run('GenFfs', ['-t', 'EFI_FV_FILETYPE_DRIVER', '-g', guid, '-o', wsl(ffs), *args])
        if guid == BDS:
            final_paths[[r['guid'] for r in before].index(BDS)] = ffs
        else:
            final_paths.append(ffs)
    fv = make_fv('MOTOG20MICROSDFV', final_paths)
    fv_report = verify_fv(fv, baseline, modules)
    transport = fv + bytes(TRANSPORT_SIZE - len(fv))
    template = bytearray(parent)
    template[IDENTITY] = bytes(32)
    template[TRANSPORT_OFFSET:TRANSPORT_OFFSET + TRANSPORT_SIZE] = transport
    template[DIGEST] = hashlib.sha256(transport).digest()
    image = bytearray(template)
    image[IDENTITY] = hashlib.sha256(template).digest()
    verify_image(image, parent, transport, template, fv)
    collision_check((sha(image), sha(template)))
    need(check_acceptance(acceptance) == report, 'Inputs changed during packaging')
    artifacts = {}
    for name, data in [('MOTO_G20_MICROSD_HOST_ONLY.img', image), ('ZERO_IDENTITY_TEMPLATE.img', template),
                       ('FV_TRANSPORT.bin', transport), ('WRAPPER_ZERO_IDENTITY.pe', template[0x20800:0x3E800])]:
        path = output / name
        path.write_bytes(data)
        artifacts[name] = record(path)
        need(checked(artifacts[name]) == data, 'Saved image mismatch')
    dump(output / 'ACCEPTED_BUILD.json', report)
    result = {'status': 'PASS_HOST_STATIC_NOT_PHYSICALLY_TESTED', 'acceptance': record(output / 'ACCEPTED_BUILD.json'),
              'fv': fv_report, 'fv_file': record(output / 'MOTOG20MICROSDFV.Fv'), 'artifacts': artifacts,
              'physical_execution_count': 0, 'device_io': 'NONE', 'usb_modules': [],
              'limits': ['No proof of card detection, sector reads, FAT access, or Windows boot.',
                         'Existing 66 MiB memory map and ACPI limitations remain.',
                         'A separate verified physical controller is required; this script cannot flash.']}
    dump(output / 'MICROSD_CANDIDATE_HOST_AUDIT.json', result)
    return result


def replay(output):
    result = json.loads((output / 'MICROSD_CANDIDATE_HOST_AUDIT.json').read_text())
    need(result['status'] == 'PASS_HOST_STATIC_NOT_PHYSICALLY_TESTED', 'Candidate report status')
    checked(result['acceptance'])
    report = check_acceptance(Path(result['acceptance']['path']))
    parent, baseline = baseline_inputs()
    modules = module_bytes(report)
    artifacts = {name: checked(row) for name, row in result['artifacts'].items()}
    fv = checked(result['fv_file'])
    verify_image(artifacts['MOTO_G20_MICROSD_HOST_ONLY.img'], parent, artifacts['FV_TRANSPORT.bin'], artifacts['ZERO_IDENTITY_TEMPLATE.img'], fv)
    need(verify_fv(fv, baseline, modules) == result['fv'], 'FV report changed')
    need(artifacts['WRAPPER_ZERO_IDENTITY.pe'] == artifacts['ZERO_IDENTITY_TEMPLATE.img'][0x20800:0x3E800], 'Wrapper mismatch')
    collision_check((sha(artifacts['MOTO_G20_MICROSD_HOST_ONLY.img']), sha(artifacts['ZERO_IDENTITY_TEMPLATE.img'])))
    return {'status': 'PASS_SAVED_MICROSD_REPLAY', 'candidate_sha256': sha(artifacts['MOTO_G20_MICROSD_HOST_ONLY.img']),
            'ffs_count': 28, 'preserved_ffs': 22, 'physical_result': 'NOT_TESTED'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('audit-inputs', 'seal-build', 'package', 'replay'))
    parser.add_argument('--build-root', type=Path, default=DEFAULT_BUILD)
    parser.add_argument('--tag')
    parser.add_argument('--provider-inf', default='MotoG20Pkg/Drivers/MotoG20SdPlatformDxe/MotoG20SdPlatformDxe.inf')
    parser.add_argument('--acceptance', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.command in ('audit-inputs', 'seal-build'):
        need(args.tag is not None, '--tag required')
        baseline_inputs()
        result = build_inputs(args.build_root, args.tag, args.provider_inf)
        if args.command == 'seal-build':
            need(args.acceptance is not None and not args.acceptance.exists(), 'New --acceptance path required')
            dump(args.acceptance, result)
        print(json.dumps({'status': result['status'], 'modules': [r['name'] for r in result['modules']], 'physical_result': 'NOT_TESTED'}, indent=2))
    elif args.command == 'package':
        need(args.acceptance is not None and args.output is not None, '--acceptance and --output required')
        print(json.dumps(package(args.acceptance, args.output), indent=2))
    else:
        need(args.output is not None, '--output required')
        print(json.dumps(replay(args.output), indent=2))


if __name__ == '__main__':
    main()

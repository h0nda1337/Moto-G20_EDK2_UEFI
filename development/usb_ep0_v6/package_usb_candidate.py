"""Package a NEW host-only USB candidate from accepted builds; no device I/O.

Commands are explicit: audit-inputs, package, replay. BDS and HC tags are required.
package creates new FVINPUT/FV/CANDIDATE directories and refuses to overwrite them.
Compilation, packaging and replay never imply successful USB hardware operation.
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
from gop_candidate import pe_audit

LAB = Path(r'C:\MOTO_G20_WOA_LAB')
ROOT = LAB / 'AUTOMATION/BUILD_ARTIFACTS/USB_HOST_20260929_V6'
GENERIC_ROOT = LAB / 'AUTOMATION/BUILD_ARTIFACTS/USB_HOST_20260917'
PARENT = LAB / 'AUTOMATION/BUILD_ARTIFACTS/CONSOLE_DIAGNOSTIC_20260914_V2'
EDK = LAB / '05_EDK2/edk2'
WORK = Path(__file__).resolve().parent
BUILD = ROOT / 'BUILD/DEBUG_GCCNOLTO/AARCH64'
PARENT_SHA = 'B158F348BAF58A12660EDF15FA5B6CAD7C41312BC3E32C07AC68509070E9E1F4'
PARENT_FV_SHA = 'AB9E218C74F6A0A9B579C1879AC59CA86220F02C0175D2691CE8F04F3E2A689D'
REGISTRY_SHA = '61BA467929B5E50CC9CEF28C482F364A401782B38ADF40B6689D98055D12884A'
KB_DEPEX_SHA = 'D3DC186D75F1D824ECCE8EA4BC7F3A148D008951AAE4FCBA5AE7EE16936612C7'
BDS = '6D33944A-EC75-4855-A54D-809C75241F6C'
HC = '6181AF65-A2CF-494F-BCB1-9127784B8258'
BUS = '240612B7-A063-11D4-9A3A-0090273FC14D'
KB = '2D2E62CF-9ECF-43B7-8219-94E7FC713DFE'
ADDITIONS = [HC, BUS, KB]
MODULES = {BDS: 'BdsDxe', HC: 'MotoG20MusbDxe', BUS: 'UsbBusDxe', KB: 'UsbKbDxe'}
FV_SIZE = 0x400000
IMAGE_SIZE = 0x4000000
TRANSPORT_OFFSET = 0x1224000
TRANSPORT_SIZE = 0x8C2314
IDENTITY = slice(0x2BDA0, 0x2BDC0)
DIGEST = slice(0x3A1A8, 0x3A1C8)


def need(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest().upper()


def align(value, alignment):
    return (value + alignment - 1) & -alignment


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def wsl(path):
    path = path.resolve()
    need(path.drive.upper() == 'C:', 'Packaging paths must be on verified C: workspace')
    return '/mnt/c/' + path.as_posix()[3:]


def parse_fv(raw, expected_count, expected_guids=None):
    """Local strict FV parser; validates 23-parent or 26-USB explicitly."""
    raw = bytes(raw)
    need(len(raw) == FV_SIZE and raw[40:44] == b'_FVH', 'FV size/signature')
    length = struct.unpack_from('<Q', raw, 32)[0]
    header = struct.unpack_from('<H', raw, 48)[0]
    need(length == len(raw) and header == 0x48 and raw[55] == 2, 'FV header contract')
    need(sum(struct.unpack_from('<36H', raw)) & 0xffff == 0, 'FV checksum')
    cursor, result = align(header, 8), []
    while cursor + 24 <= len(raw):
        if raw[cursor:cursor + 24] == b'\xff' * 24:
            need(raw[cursor:] == b'\xff' * (len(raw) - cursor), 'Unexpected FV free-space content')
            break
        h = raw[cursor:cursor + 24]
        size = int.from_bytes(h[20:23], 'little')
        need(not h[19] & 1 and 24 <= size <= len(raw) - cursor, 'Invalid FFS bounds/large-file flag')
        need((sum(h) - h[17] - h[23]) & 0xff == 0, 'FFS header checksum')
        need(h[23] == 0xf8, 'FFS data-valid state')
        data = raw[cursor + 24:cursor + size]
        need(((sum(data) + h[17]) & 0xff == 0) if h[19] & 0x40 else h[17] == 0xaa, 'FFS data checksum')
        if h[18] != 0xf0:
            sections, pos = [], 0
            while pos < len(data):
                need(pos + 4 <= len(data), 'Truncated section header')
                section_size = int.from_bytes(data[pos:pos + 3], 'little')
                need(4 <= section_size < 0xffffff and pos + section_size <= len(data), 'Section bounds')
                sections.append({'type': data[pos + 3], 'data': data[pos + 4:pos + section_size]})
                next_pos = align(pos + section_size, 4)
                if next_pos > len(data):
                    need(pos + section_size == len(data), 'Section tail')
                    break
                pos = next_pos
            result.append({'guid': str(uuid.UUID(bytes_le=h[:16])).upper(), 'type': h[18],
                           'offset': cursor, 'raw': raw[cursor:cursor + size], 'sections': sections})
        cursor = align(cursor + size, 8)
    need(len(result) == expected_count, 'FV file count')
    need(len({row['guid'] for row in result}) == expected_count, 'Duplicate FFS GUID')
    if expected_guids is not None:
        need([row['guid'] for row in result] == list(expected_guids), 'FFS GUID order/set')
    return result


def checked_file(path, size, digest):
    data = path.read_bytes()
    need(len(data) == size and sha(data) == digest, 'Artifact drift: ' + str(path))
    return data


def genfv_input_ffs(on_flash):
    # GenFvInternalLib.c UpdateFfsFileState complements State for erase
    # polarity 1. Extracted FV files already have that on-flash polarity;
    # convert back to the GenFfs input convention to avoid complementing twice.
    need(len(on_flash) >= 24 and on_flash[23] == 0xf8, 'Expected on-flash FFS state')
    raw = bytearray(on_flash)
    raw[23] ^= 0xff
    return bytes(raw)


def overlay_manifest():
    root = ROOT / 'WORKSPACE/MotoG20Pkg'
    return {path.relative_to(root).as_posix(): {'size': path.stat().st_size, 'sha256': sha(path.read_bytes())}
            for path in sorted(root.rglob('*')) if path.is_file()}


def manifest_map(rows):
    result = {row['path']: {'size': row['size'], 'sha256': row['sha256']} for row in rows}
    need(len(result) == len(rows), 'Duplicate source manifest paths')
    return result


def accepted_module(guid, tag, current_overlay):
    name = MODULES[guid]
    custom = guid in (BDS, HC)
    origin = ROOT if custom else GENERIC_ROOT
    run = origin / 'REPORTS' / ((('BDS' if guid == BDS else 'HC') + '_' + tag) if custom else (name + '_' + tag))
    audit_path = run / 'BUILD_AUDIT.json'
    audit_bytes = audit_path.read_bytes()
    audit = json.loads(audit_bytes)
    need(audit.get('status') == 'PASS_HOST_COMPILATION_AND_PE_STATIC_CHECKS' and audit.get('module') == name,
         'Not an accepted build for ' + name)
    need(audit.get('build_exit_code') == 0 and (run / 'exit_code.txt').read_text().strip() == '0', 'Build exit failure')
    overlay = (run / 'overlay_sha256.json').read_bytes()
    need(sha(overlay) == audit['source_overlay_manifest_sha256'], 'Build source manifest drift')
    need(sha((run / 'build.log').read_bytes()) == audit['build_log_sha256'], 'Build log drift')
    need(sha((run / 'toolchain.txt').read_bytes()) == audit['toolchain_manifest_sha256'], 'Toolchain evidence drift')
    artifacts = {}
    for row in audit['artifacts']:
        source, saved = Path(row['path']), Path(row['evidence_path'])
        need(saved.resolve().is_relative_to((run / 'OUTPUT_EVIDENCE').resolve()), 'Unexpected evidence path')
        data = checked_file(saved, row['size'], row['sha256'])
        # Other module builds can change shared .lib caches; PE/map/dll/AutoGen/depex
        # output evidence is the accepted product. Do not take a newer .lib as proof.
        artifacts[source.name] = data
    raw = artifacts[name + '.efi']
    need((BUILD / (name + '.efi')).read_bytes() == raw, 'Current PE differs from accepted product')
    pe = pe_audit(raw)
    need(struct.unpack_from('<H', raw, struct.unpack_from('<I', raw, 0x3c)[0] + 4)[0] == 0xaa64, 'Not AARCH64')
    if custom:
        need(manifest_map(json.loads(overlay)) == current_overlay, name + ': final overlay differs from build')
        closure_bytes = (run / 'SOURCE_CLOSURE.json').read_bytes()
        need(sha(closure_bytes) == audit['source_closure_sha256'], 'Source closure manifest drift')
        closure = json.loads(closure_bytes)
        for category in ('shared_sources', 'overlay', 'configuration', 'generated_dependency_evidence'):
            for row in closure[category]:
                saved = Path(row['evidence_path'])
                need(saved.resolve().is_relative_to(run.resolve()), 'Source evidence path escapes run')
                checked_file(saved, row['size'], row['sha256'])
                if category in ('shared_sources', 'overlay'):
                    checked_file(Path(row['path']), row['size'], row['sha256'])
        depex = artifacts[name + '.depex']
        need(depex.hex() == audit['depex']['hex'] and sha(depex) == audit['depex']['sha256'], 'Linked DEPEX drift')
    else:
        need(audit['module_type'] == 'UEFI_DRIVER' and audit['depex'] == 'ABSENT_AS_EXPECTED_FOR_STOCK_UEFI_DRIVER',
             'Generic UEFI_DRIVER contract drift')
        need(name + '.depex' not in artifacts, 'Unexpected generic build DEPEX')
        need('/AARCH64/MotoG20Pkg/' not in artifacts[name + '.map'].decode(),
             'Generic build unexpectedly consumes a MotoG20 library; overlay exception needs re-review')
        prep = json.loads((GENERIC_ROOT / 'REPORTS/PREPARATION.json').read_text())
        for base, rows in prep['generic_source_snapshot'].items():
            for row in rows:
                for source in (EDK / base / row['path'], GENERIC_ROOT / 'SOURCE/EDK2' / base / row['path']):
                    checked_file(source, row['size'], row['sha256'])
        shared = json.loads((GENERIC_ROOT / 'REPORTS/SHARED_DEPENDENCIES.json').read_text())
        for row in shared['files']:
            for source in (EDK / row['path'], GENERIC_ROOT / 'SOURCE/EDK2_BUILD_DEPENDENCIES' / row['path']):
                checked_file(source, row['size'], row['sha256'])
        depex = None
        if guid == KB:
            autogen = artifacts['AutoGen.h'].decode()
            need(re.search(r'^#define\s+_PCD_VALUE_PcdDisableDefaultKeyboardLayoutInUsbKbDriver\s+\(\(BOOLEAN\)0U\)', autogen, re.M),
                 'USB default keyboard layout is disabled')
            depex = (WORK.parent / 'usb_generic_build/depex_review/UsbKbDxe.arch_and_hii.depex').read_bytes()
            need(len(depex) == 288 and sha(depex) == KB_DEPEX_SHA, 'Keyboard architecture/HII DEPEX drift')
            proposal = json.loads((WORK.parent / 'usb_generic_build/depex_review/DEPEX_PROPOSAL.json').read_text())
            for row in proposal['sources']:
                need(sha(Path(row['path']).read_bytes()) == row['sha256'], 'DEPEX source contract drift')
    info = {'module': name, 'tag': tag, 'audit_path': str(audit_path), 'audit_sha256': sha(audit_bytes),
            'pe_sha256': sha(raw), 'pe_size': len(raw), 'pe': pe,
            'depex_sha256': sha(depex) if depex is not None else None,
            'source_binding': 'FULL_CURRENT_OVERLAY_AND_PRESERVED_CLOSURE' if custom else
            'GENERIC_SOURCE_AND_SHARED_DEPENDENCY_CLOSURE_PRESERVED; unrelated current MotoG20 overlay excluded',
            'recorded_artifacts': audit['artifacts']}
    return {'pe': raw, 'depex': depex, 'info': info}


def registry_and_collision(identities=()):
    registry = LAB / 'AUTOMATION/STATE/candidate_execution_registry.jsonl'
    raw = registry.read_bytes()
    need(sha(raw) == REGISTRY_SHA, 'Execution registry changed; re-review before packaging')
    lines = [json.loads(line) for line in raw.decode('utf-8').splitlines() if line.strip()]
    need(len(lines) == 18, 'Execution registry event count')
    identities = {identity.upper() for identity in identities}
    if identities:
        paths = [registry, *(LAB / 'AUTOMATION/STATE').rglob('*.json')]
        for path in paths:
            content = path.read_text(encoding='utf-8-sig').upper()
            need(not any(value in path.name.upper() or value in content for value in identities), 'Identity already claimed: ' + str(path))
    return {'sha256': sha(raw), 'events': len(lines)}


def inputs(bds_tag, hc_tag):
    need(not (ROOT / 'BUILD_TRANCHE.lock').exists(), 'Build still running')
    need(all(re.fullmatch(r'[A-Za-z0-9_-]+', tag) for tag in (bds_tag, hc_tag)), 'Invalid build tag')
    parent = (PARENT / 'CANDIDATE/MOTO_G20_CONSOLE_FLAT_GOP_HOST_ONLY.img').read_bytes()
    need(sha(parent) == PARENT_SHA, 'Consumed B158 parent drift')
    baseline = (PARENT / 'FV/MOTOG20FOUNDATIONFV.Fv').read_bytes()
    need(sha(baseline) == PARENT_FV_SHA, 'Baseline FV drift')
    parse_fv(baseline, 23)
    need(parent[TRANSPORT_OFFSET:TRANSPORT_OFFSET + FV_SIZE] == baseline, 'Parent FV mismatch')
    old_transport = parent[TRANSPORT_OFFSET:TRANSPORT_OFFSET + TRANSPORT_SIZE]
    need(parent[DIGEST] == hashlib.sha256(old_transport).digest(), 'Parent transport digest')
    old_zero = bytearray(parent)
    old_zero[IDENTITY] = bytes(32)
    need(parent[IDENTITY] == hashlib.sha256(old_zero).digest(), 'Parent canonical identity')
    current = overlay_manifest()
    modules = {guid: accepted_module(guid, tag, current)
               for guid, tag in ((BDS, bds_tag), (HC, hc_tag), (BUS, 'bus01'), (KB, 'kb01'))}
    need(overlay_manifest() == current, 'Sources changed during input audit')
    registry = registry_and_collision()
    return parent, baseline, modules, registry


def expected_sections(guid, module):
    result = []
    if module['depex'] is not None:
        result.append({'type': 0x13, 'data': module['depex']})
    result.extend([{'type': 0x10, 'data': module['pe']},
                   {'type': 0x15, 'data': (MODULES[guid] + '\0').encode('utf-16-le')}])
    return result


def verify_fv(candidate, baseline, modules):
    need(sha(baseline) == PARENT_FV_SHA, 'Baseline FV drift')
    before = parse_fv(baseline, 23)
    after = parse_fv(candidate, 26, [row['guid'] for row in before] + ADDITIONS)
    need(after[0]['offset'] == before[0]['offset'] and
         candidate[:after[0]['offset']] == baseline[:before[0]['offset']],
         'FV attributes/extended-header prefix changed')
    preserved, changed = [], []
    for old, new in zip(before, after[:23]):
        guid = old['guid']
        if guid != BDS:
            need(old['raw'] == new['raw'], 'Unapproved parent FFS change: ' + guid)
            preserved.append({'guid': guid, 'sha256': sha(old['raw'])})
            continue
        need(old['type'] == new['type'] == 7, 'BDS FFS type')
        need([row['type'] for row in old['sections']] == [row['type'] for row in new['sections']], 'BDS section order/type changed')
        for left, right in zip(old['sections'], new['sections']):
            expected = modules[BDS]['pe'] if right['type'] == 0x10 else modules[BDS]['depex'] if right['type'] == 0x13 else left['data']
            need(right['data'] == expected, 'BDS section payload mismatch')
        need(sum(row['type'] == 0x10 for row in new['sections']) == 1, 'BDS PE missing/duplicate')
        need(sum(row['type'] == 0x13 for row in new['sections']) == 1, 'BDS DEPEX missing/duplicate')
        changed.append({'guid': guid, 'old_sha256': sha(old['raw']), 'new_sha256': sha(new['raw']),
                        'depex_changed': next(row['data'] for row in old['sections'] if row['type'] == 0x13) != modules[BDS]['depex']})
    for new in after[23:]:
        need(new['type'] == 7, 'USB addition must be FFS DRIVER')
        need(new['sections'] == expected_sections(new['guid'], modules[new['guid']]), 'USB PE/DEPEX/UI section mismatch: ' + new['guid'])
    need(len(preserved) == 22 and len(changed) == 1, 'Parent delta count')
    # Every named FFS is fixed above; no unreviewed APRIORI or extra USB driver can enter.
    used = max(align(row['offset'] + len(row['raw']), 8) for row in after)
    return {'ffs_count': 26, 'preserved_ffs': preserved, 'replaced_ffs': changed,
            'added_ffs': ADDITIONS, 'fv_sha256': sha(candidate), 'fv_size': len(candidate), 'free_tail_bytes': len(candidate) - used}


def verify_image(candidate, parent, transport, template, fv, baseline, modules):
    need(len(candidate) == len(parent) == len(template) == IMAGE_SIZE, 'Image size')
    need(candidate[:8] == b'ANDROID!' and struct.unpack_from('<I', candidate, 16)[0] == TRANSPORT_SIZE, 'Android header contract')
    need(len(transport) == TRANSPORT_SIZE and len(fv) == FV_SIZE, 'Transport/FV size')
    need(transport[:FV_SIZE] == fv and not any(transport[FV_SIZE:]), 'Transport FV/zero tail')
    need(candidate[TRANSPORT_OFFSET:TRANSPORT_OFFSET + TRANSPORT_SIZE] == transport, 'Transport placement')
    need(candidate[DIGEST] == hashlib.sha256(transport).digest(), 'Transport digest')
    normalized = bytearray(candidate)
    normalized[IDENTITY] = bytes(32)
    need(normalized == template and candidate[IDENTITY] == hashlib.sha256(template).digest(), 'Canonical identity binding')
    cursor = 0
    for start, end in [(IDENTITY.start, IDENTITY.stop), (DIGEST.start, DIGEST.stop),
                       (TRANSPORT_OFFSET, TRANSPORT_OFFSET + FV_SIZE), (IMAGE_SIZE, IMAGE_SIZE)]:
        need(candidate[cursor:start] == parent[cursor:start], 'Unapproved boot-image delta')
        cursor = end
    return verify_fv(fv, baseline, modules)


def package(bds_tag, hc_tag):
    parent, baseline, modules, registry = inputs(bds_tag, hc_tag)
    for directory in ('FVINPUT', 'FV', 'CANDIDATE'):
        need(not (ROOT / directory).exists(), 'Refusing existing packaging directory: ' + directory)
    fvinput, fvdir = ROOT / 'FVINPUT', ROOT / 'FV'
    fvinput.mkdir()
    fvdir.mkdir()
    commands = []

    def run(tool, args):
        path = EDK / 'BaseTools/Source/C/bin' / tool
        proc = subprocess.run(['wsl', '-d', 'Debian', '--', wsl(path), *args], capture_output=True, text=True, timeout=60)
        commands.append({'tool': str(path), 'sha256': sha(path.read_bytes()), 'args': args,
                         'exit_code': proc.returncode, 'stdout': proc.stdout, 'stderr': proc.stderr})
        dump(ROOT / 'REPORTS/USB_PACKAGING_COMMANDS.json', commands)
        need(proc.returncode == 0, tool + ' failed: ' + proc.stderr)

    before = parse_fv(baseline, 23)
    old_inf = (PARENT / 'FVINPUT/console_new.inf').read_text()
    need(old_inf.count('[files]') == 1, 'Baseline INF files block')
    prefix = old_inf.split('[files]', 1)[0]
    ext = (PARENT / 'FVINPUT/MOTOG20FOUNDATIONFV.ext').read_bytes()
    ext_path = fvinput / 'MOTOG20FOUNDATIONFV.ext'
    ext_path.write_bytes(ext)
    prefix, count = re.subn(r'(?m)^EFI_FV_EXT_HEADER_FILE_NAME\s*=.*$', 'EFI_FV_EXT_HEADER_FILE_NAME = ' + wsl(ext_path), prefix)
    need(count == 1, 'Baseline INF extended header')
    original_inputs = []
    for row in before:
        path = fvinput / (row['guid'] + '.ffs')
        path.write_bytes(genfv_input_ffs(row['raw']))
        original_inputs.append(path)

    def make_inf(name, paths):
        path = fvinput / name
        path.write_text(prefix + '[files]\n' + ''.join('EFI_FILE_NAME = ' + wsl(p) + '\n' for p in paths), newline='\n')
        return path

    baseline_inf = make_inf('baseline_b158.inf', original_inputs)
    reproduced = fvdir / 'baseline_reproduced.Fv'
    run('GenFv', ['-i', wsl(baseline_inf), '-o', wsl(reproduced)])
    need(reproduced.read_bytes() == baseline, 'Baseline reproduction differs byte-for-byte')
    final_inputs = list(original_inputs)
    for guid in (BDS, *ADDITIONS):
        module, folder = modules[guid], fvinput / ('NEW_' + MODULES[guid])
        folder.mkdir()
        if guid == BDS:
            old = next(row for row in before if row['guid'] == BDS)
            sections = [{'type': row['type'], 'data': module['pe'] if row['type'] == 0x10 else module['depex'] if row['type'] == 0x13 else row['data']}
                        for row in old['sections']]
        else:
            sections = expected_sections(guid, module)
        section_args = []
        for index, section in enumerate(sections):
            payload, kind = section['data'], section['type']
            out = folder / (str(index) + '.section')
            if kind in (0x10, 0x13):
                raw_path = folder / (str(index) + '.payload')
                raw_path.write_bytes(payload)
                section_type = 'EFI_SECTION_PE32' if kind == 0x10 else 'EFI_SECTION_DXE_DEPEX'
                run('GenSec', ['-s', section_type, '-o', wsl(out), wsl(raw_path)])
            else:
                out.write_bytes((len(payload) + 4).to_bytes(3, 'little') + bytes([kind]) + payload)
            section_args.extend(['-oi', wsl(out)])
        ffs = folder / (guid + '.ffs')
        run('GenFfs', ['-t', 'EFI_FV_FILETYPE_DRIVER', '-g', guid, '-o', wsl(ffs), *section_args])
        if guid == BDS:
            final_inputs[[row['guid'] for row in before].index(BDS)] = ffs
        else:
            final_inputs.append(ffs)
    target_inf = make_inf('usb_candidate.inf', final_inputs)
    target = fvdir / 'MOTOG20USBHOSTFV.Fv'
    run('GenFv', ['-i', wsl(target_inf), '-o', wsl(target)])
    fv = target.read_bytes()
    fv_report = verify_fv(fv, baseline, modules)
    transport = fv + bytes(TRANSPORT_SIZE - len(fv))
    template = bytearray(parent)
    template[IDENTITY] = bytes(32)
    template[TRANSPORT_OFFSET:TRANSPORT_OFFSET + TRANSPORT_SIZE] = transport
    template[DIGEST] = hashlib.sha256(transport).digest()
    image = bytearray(template)
    image[IDENTITY] = hashlib.sha256(template).digest()
    verify_image(image, parent, transport, template, fv, baseline, modules)
    registry_and_collision((sha(image), sha(template)))
    # Re-audit current source/artifact/registry state immediately before writing.
    _, _, checked_modules, _ = inputs(bds_tag, hc_tag)
    need({g: m['info'] for g, m in checked_modules.items()} == {g: m['info'] for g, m in modules.items()}, 'Inputs changed during packaging')
    dest = ROOT / 'CANDIDATE'
    dest.mkdir()
    artifacts = {}
    for name, data in [('MOTO_G20_USB_HOST_HOST_ONLY.img', image), ('ZERO_IDENTITY_TEMPLATE.img', template),
                       ('FV_TRANSPORT.bin', transport), ('WRAPPER_ZERO_IDENTITY.pe', template[0x20800:0x3E800])]:
        path = dest / name
        path.write_bytes(data)
        need(path.read_bytes() == data, 'Saved candidate differs: ' + name)
        artifacts[name] = {'path': str(path), 'size': len(data), 'sha256': sha(data)}
    report = {'status': 'PASS_HOST_STATIC_NOT_PHYSICALLY_TESTED', 'parent_candidate_sha256': PARENT_SHA,
              'parent_fv_sha256': PARENT_FV_SHA, 'registry': registry, 'fv': fv_report,
              'bds_tag': bds_tag, 'hc_tag': hc_tag, 'modules': {g: m['info'] for g, m in modules.items()},
              'artifacts': artifacts, 'physical_execution_count': 0, 'device_io': 'NONE',
              'limits': ['Not proof of VBUS, hub enumeration, or keyboard input.', 'Execution requires a separate new-identity physical controller.']}
    dump(ROOT / 'REPORTS/USB_CANDIDATE_HOST_AUDIT.json', report)
    return report


def replay(bds_tag, hc_tag):
    parent, baseline, modules, _ = inputs(bds_tag, hc_tag)
    report = json.loads((ROOT / 'REPORTS/USB_CANDIDATE_HOST_AUDIT.json').read_text())
    need(report['status'] == 'PASS_HOST_STATIC_NOT_PHYSICALLY_TESTED', 'Candidate audit status')
    need(report['bds_tag'] == bds_tag and report['hc_tag'] == hc_tag, 'Candidate audit build tags')
    need(report['modules'] == {g: m['info'] for g, m in modules.items()}, 'Candidate module evidence changed')
    need(report['parent_candidate_sha256'] == PARENT_SHA and report['parent_fv_sha256'] == PARENT_FV_SHA, 'Candidate parent changed')
    saved = {}
    for name, row in report['artifacts'].items():
        need(Path(row['path']).resolve() == (ROOT / 'CANDIDATE' / name).resolve(), 'Candidate path mismatch')
        saved[name] = checked_file(Path(row['path']), row['size'], row['sha256'])
    fv = (ROOT / 'FV/MOTOG20USBHOSTFV.Fv').read_bytes()
    fv_report = verify_image(saved['MOTO_G20_USB_HOST_HOST_ONLY.img'], parent, saved['FV_TRANSPORT.bin'],
                             saved['ZERO_IDENTITY_TEMPLATE.img'], fv, baseline, modules)
    need(fv_report == report['fv'], 'FV audit mismatch')
    need(saved['WRAPPER_ZERO_IDENTITY.pe'] == saved['ZERO_IDENTITY_TEMPLATE.img'][0x20800:0x3E800], 'Wrapper mismatch')
    registry_and_collision((sha(saved['MOTO_G20_USB_HOST_HOST_ONLY.img']), sha(saved['ZERO_IDENTITY_TEMPLATE.img'])))
    return {'status': 'PASS_SAVED_USB_IMAGE_REPLAY', 'candidate_sha256': sha(saved['MOTO_G20_USB_HOST_HOST_ONLY.img']),
            'preserved_parent_ffs': 22, 'ffs_count': 26, 'physical_result': 'NOT_TESTED'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('audit-inputs', 'package', 'replay'))
    parser.add_argument('--bds-tag', required=True)
    parser.add_argument('--hc-tag', required=True)
    args = parser.parse_args()
    if args.command == 'audit-inputs':
        _, _, modules, registry = inputs(args.bds_tag, args.hc_tag)
        result = {'status': 'PASS_INPUTS_ONLY_NO_IMAGE_CREATED', 'registry': registry,
                  'modules': {guid: module['info'] for guid, module in modules.items()}}
    elif args.command == 'package':
        result = package(args.bds_tag, args.hc_tag)
    else:
        result = replay(args.bds_tag, args.hc_tag)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

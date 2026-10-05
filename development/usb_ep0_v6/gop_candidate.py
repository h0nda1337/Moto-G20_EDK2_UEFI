"""Offline GOP/FV/image validation and construction. No device operations."""
import argparse
import hashlib
import json
import re
import struct
import uuid
from pathlib import Path
from gop_photo_forensics import pe_sections, sha

LAB = Path(r'C:\MOTO_G20_WOA_LAB')
ROOT = LAB / 'AUTOMATION/BUILD_ARTIFACTS/GOP_DEVICE_MEMORY_SAFE_20260909'
FROZEN = LAB / '05_EDK2/EDK2_FOUNDATION_DXE_POC_PUBLICATION_SURGICAL_CANDIDATE_HOST_STATIC_PASS'
BUILD = ROOT / 'BUILD/DEBUG_GCCNOLTO/AARCH64'
MODULE = Path('MotoG20Pkg/Drivers/MotoG20FramebufferGopDxe/MotoG20FramebufferGopDxe')
GOP = BUILD / 'MotoG20FramebufferGopDxe.efi'
GUID = '50F9E65A-1C38-4D77-AB1D-830EA33AA1C0'
OLD_IMAGE_SHA = 'A626BF57917E67BCE8049BAA34B49D6C0EEBF16A6D0CAD9F00531E11D8F444B5'
OLD_FV_SHA = '9BC125BED052D3190241498F4A13A25DC0F78714DD1B503F552A19E3A4879800'
OLD_GOP_SHA = 'E7CBDF99688331184CC2E7F4A2E8DD680D56E0EC58ACC8F42D8ECD05C1B7D885'

def need(ok, why):
    if not ok:
        raise ValueError(why)

def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

def align(n, a):
    return (n + a - 1) & -a

def parse_fv(raw):
    raw = bytes(raw)
    need(len(raw) == 0x400000 and raw[40:44] == b'_FVH', 'FV size/signature')
    length = struct.unpack_from('<Q', raw, 32)[0]
    header = struct.unpack_from('<H', raw, 48)[0]
    need(length == len(raw) and header == 0x48 and raw[55] == 2, 'FV header contract')
    need(sum(struct.unpack_from('<36H', raw)) & 0xffff == 0, 'FV checksum')
    cursor = align(header, 8)
    result = []
    while cursor + 24 <= len(raw):
        if raw[cursor:cursor + 24] == b'\xff' * 24:
            need(raw[cursor:] == b'\xff' * (len(raw) - cursor), 'Unexpected data in FV free space')
            break
        h = raw[cursor:cursor + 24]
        size = int.from_bytes(h[20:23], 'little')
        need(not h[19] & 1 and 24 <= size <= len(raw) - cursor, 'Unsupported or invalid FFS length')
        need((sum(h) - h[17] - h[23]) & 0xff == 0, 'FFS header checksum')
        need(h[23] == 0xF8, 'FFS not in data-valid state')
        data = raw[cursor + 24:cursor + size]
        need((sum(data) + h[17]) & 0xff == 0 if h[19] & 0x40 else h[17] == 0xaa, 'FFS data checksum')
        if h[18] != 0xF0:
            sections = []
            pos = 0
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
    need(len(result) == 23 and len({f['guid'] for f in result}) == 23, 'FV file count/uniqueness')
    return result

def pe_audit(raw):
    need(raw[:2] == b'MZ', 'DOS signature')
    sections = pe_sections(raw)
    pe = struct.unpack_from('<I', raw, 0x3c)[0]
    opt = pe + 24
    need(struct.unpack_from('<H', raw, opt)[0] == 0x20b, 'PE32+ magic')
    entry = struct.unpack_from('<I', raw, opt + 16)[0]
    size_image, size_headers = struct.unpack_from('<II', raw, opt + 56)
    subsystem = struct.unpack_from('<H', raw, opt + 68)[0]
    section_align, file_align = struct.unpack_from('<II', raw, opt + 32)
    need(subsystem == 11 and section_align == file_align == 0x1000, 'GOP PE contract')
    need(any(s['executable'] and s['rva'] <= entry < s['rva'] + s['size'] for s in sections), 'Executable entry')
    need(all(s['rva'] + max(s['size'], s['virtual_size']) <= size_image for s in sections), 'PE image bounds')
    need(size_headers <= min(s['offset'] for s in sections if s['size']), 'PE header overlap')
    image = bytearray(size_image)
    image[:size_headers] = raw[:size_headers]
    zva = []
    for s in sections:
        image[s['rva']:s['rva'] + s['size']] = raw[s['offset']:s['offset'] + s['size']]
        if s['executable']:
            for i in range(0, s['size'] - 3, 4):
                word = struct.unpack_from('<I', raw, s['offset'] + i)[0]
                if word & 0xFFFFFFE0 == 0xD50B7420:
                    zva.append(hex(s['rva'] + i))
    reloc_rva, reloc_size = struct.unpack_from('<II', raw, opt + 112 + 5 * 8)
    need(0 < reloc_size and reloc_rva + reloc_size <= len(image), 'Relocation directory bounds')
    pos, end, targets = reloc_rva, reloc_rva + reloc_size, []
    while pos < end:
        page, block = struct.unpack_from('<II', image, pos)
        need(block >= 8 and block % 2 == 0 and pos + block <= end, 'Relocation block')
        for at in range(pos + 8, pos + block, 2):
            word = struct.unpack_from('<H', image, at)[0]
            typ, target = word >> 12, page + (word & 0xfff)
            need(typ in (0, 10), 'Unsupported relocation')
            if typ:
                need(any(s['rva'] <= target and target + 8 <= s['rva'] + s['size'] for s in sections), 'Relocation target bounds')
                targets.append(target)
        pos += block
    need(targets and len(set(targets)) == len(targets), 'Missing/duplicate relocations')
    return {'sha256': sha(raw), 'size': len(raw), 'entry_rva': hex(entry), 'size_of_image': hex(size_image),
            'sections': sections, 'dc_zva_rvas': zva, 'dir64_targets': [hex(t) for t in targets]}

def audit_gop(write_report=True):
    need((ROOT / 'REPORTS/build_exit_code.txt').read_text().strip() == '0', 'Build has not succeeded')
    raw = GOP.read_bytes()
    info = pe_audit(raw)
    need(not info['dc_zva_rvas'], 'DC ZVA remains in new GOP')
    map_path = BUILD / MODULE / 'DEBUG/MotoG20FramebufferGopDxe.map'
    mapping = map_path.read_text()
    need('BaseMemoryLibOptDxe' not in mapping, 'Optimized library still linked')
    need('/BaseMemoryLib/BaseMemoryLib/' in mapping, 'Scalar library not in link map')
    new_h = (BUILD / MODULE / 'DEBUG/AutoGen.h').read_text()
    old_h = (LAB / '05_EDK2/Build/MotoG20FoundationFv/DEBUG_GCCNOLTO/AARCH64' / MODULE / 'DEBUG/AutoGen.h').read_text()
    defs = lambda s: dict(re.findall(r'^#define\s+(_PCD_VALUE_\S+)\s+([^\r\n]+)', s, re.M))
    need(defs(new_h) == defs(old_h), 'GOP PCD values changed')
    info.update({'status': 'PASS_HOST_MODULE_STATIC', 'pcd_values': defs(new_h),
                 'generic_base_memory_linked': True, 'physical_result': 'NOT_TESTED', 'device_io': 'NONE'})
    if write_report:
        write_json(ROOT / 'REPORTS/GOP_BINARY_AUDIT.json', info)
    return info

def compare_fv(new):
    old = (FROZEN / 'BIN/MOTOG20FOUNDATIONFV.Fv').read_bytes()
    need(sha(old) == OLD_FV_SHA, 'Frozen FV drift')
    need((ROOT / 'FV/baseline_reproduced.Fv').read_bytes() == old, 'Baseline reproduction failed')
    a, b = parse_fv(old), parse_fv(new)
    need([f['guid'] for f in a] == [f['guid'] for f in b], 'FV ordering changed')
    changed, preserved = [], []
    for x, y in zip(a, b):
        if x['raw'] != y['raw']:
            changed.append(x['guid'])
        else:
            preserved.append({'guid': x['guid'], 'ffs_sha256': sha(x['raw'])})
        if x['guid'] == GUID:
            need(x['type'] == y['type'] == 7, 'GOP FFS type')
            need([s['type'] for s in x['sections']] == [s['type'] for s in y['sections']], 'GOP section layout')
            for p, q in zip(x['sections'], y['sections']):
                if p['type'] == 0x10:
                    need(sha(p['data']) == OLD_GOP_SHA and q['data'] == GOP.read_bytes(), 'GOP payload mismatch')
                else:
                    need(p['data'] == q['data'], 'GOP DEPEX/UI changed')
    need(changed == [GUID] and len(preserved) == 22, 'Unintended FV module changes')
    return {'changed_ffs_guids': changed, 'unchanged_ffs': preserved, 'baseline_reproduction': 'BYTE_IDENTICAL',
            'old_fv_sha256': sha(old), 'new_fv_sha256': sha(new), 'fv_header_and_ffs_checksums': 'PASS'}

def build_candidate():
    gop = audit_gop()
    fv = (ROOT / 'FV/MOTOG20FOUNDATIONFV.Fv').read_bytes()
    fv_report = compare_fv(fv)
    old = (FROZEN / 'BIN/MOTO_G20_BOOT_A_EDK2_DXE_POC_PUBLICATION_BOUND_SURGICAL.img').read_bytes()
    old_transport = (FROZEN / 'BIN/MOTO_G20_FOUNDATION_DXE_POC_PUBLICATION_FV_TRANSPORT.bin').read_bytes()
    need(sha(old) == OLD_IMAGE_SHA and len(old) == 0x4000000, 'Baseline image drift')
    need(len(old_transport) == 0x8C2314 and old_transport[:0x400000] == (FROZEN / 'BIN/MOTOG20FOUNDATIONFV.Fv').read_bytes(), 'Old transport FV')
    need(not any(old_transport[0x400000:]), 'Old transport tail')
    ram, ident, digest, pe_offset = 0x1224000, 0x2BDA0, 0x3A1A8, 0x20800
    need(old[ram:ram + len(old_transport)] == old_transport, 'Baseline ramdisk transport')
    need(old[digest:digest + 32] == hashlib.sha256(old_transport).digest(), 'Baseline embedded digest')
    old_template = bytearray(old)
    old_template[ident:ident + 32] = bytes(32)
    need(old[ident:ident + 32] == hashlib.sha256(old_template).digest(), 'Baseline identity binding')
    need(old[:8] == b'ANDROID!' and struct.unpack_from('<I', old, 16)[0] == len(old_transport), 'Android header')
    transport = fv + bytes(len(old_transport) - len(fv))
    template = bytearray(old_template)
    template[ram:ram + len(transport)] = transport
    template[digest:digest + 32] = hashlib.sha256(transport).digest()
    canonical = hashlib.sha256(template).digest()
    candidate = bytearray(template)
    candidate[ident:ident + 32] = canonical
    # Compare complete complements of every intentionally mutable region.
    regions = [(ident, ident + 32), (digest, digest + 32), (ram, ram + 0x400000)]
    cursor = 0
    for start, end in regions + [(len(old), len(old))]:
        need(candidate[cursor:start] == old[cursor:start], f'Unexpected change before {start:#x}')
        cursor = end
    need(candidate[0x1AE6800:0x1B0C721] == old[0x1AE6800:0x1B0C721], 'DTB changed')
    need(candidate[0x3FFFFC0:] == old[0x3FFFFC0:], 'AVB footer changed')
    normalized = bytearray(candidate)
    normalized[ident:ident + 32] = bytes(32)
    need(hashlib.sha256(normalized).digest() == canonical, 'New identity binding')
    registry = LAB / 'AUTOMATION/STATE/candidate_execution_registry.jsonl'
    need(sha(registry.read_bytes()) == '6DC53DF812B12FACDB2FCF1A03446B69571D39DE8896A719EAFB647A85BB026A', 'Execution registry drift')
    identities_to_check = {sha(candidate), sha(template)}
    claims = list((LAB / 'AUTOMATION/STATE').rglob('*.json'))
    collision_inputs = [registry, *claims]
    collisions = [str(p) for p in collision_inputs if any(h in p.name.upper() or h in p.read_text(encoding='utf-8-sig').upper() for h in identities_to_check)]
    need(not collisions, f'Candidate identity already present in state: {collisions}')
    out = ROOT / 'CANDIDATE'
    out.mkdir(exist_ok=False)
    paths = {'candidate': out / 'MOTO_G20_GOP_SCALAR_MEMORY_HOST_ONLY.img',
             'template': out / 'GOP_SCALAR_ZERO_IDENTITY_TEMPLATE.img',
             'transport': out / 'GOP_SCALAR_FV_TRANSPORT.bin',
             'wrapper_pe': out / 'MotoG20FoundationGopScalar.pe'}
    payloads = {'candidate': candidate, 'template': template, 'transport': transport,
                'wrapper_pe': template[pe_offset:pe_offset + 0x1E000]}
    identities = {}
    for name, path in paths.items():
        path.write_bytes(payloads[name])
        need(path.read_bytes() == payloads[name], 'Write verification failed')
        identities[name] = {'path': str(path), 'sha256': sha(payloads[name]), 'size': len(payloads[name])}
    report = {'schema': 'moto-g20-gop-scalar-host-candidate-v1', 'status': 'PASS_HOST_STATIC_ONLY',
              'gop': gop, 'fv': fv_report, 'artifacts': identities,
              'parent_candidate_sha256': OLD_IMAGE_SHA, 'parent_retry': 'PERMANENTLY_FORBIDDEN',
              'allowed_image_delta_regions': [[hex(a), hex(b)] for a, b in regions],
              'template_binding': canonical.hex().upper(), 'physical_execution_in_this_task': 0,
              'registry_and_state_collision_scan': {'files': len(collision_inputs), 'collisions': collisions},
              'device_io': 'NONE', 'physical_result': 'NOT_TESTED',
              'next_gate': 'Phone observation and read-only Android B preflight required before any physical plan'}
    write_json(ROOT / 'REPORTS/CANDIDATE_HOST_AUDIT.json', report)
    return {'status': report['status'], 'artifacts': identities, 'preserved_ffs': 22}

def verify_candidate():
    report = json.loads((ROOT / 'REPORTS/CANDIDATE_HOST_AUDIT.json').read_text())
    for name, item in report['artifacts'].items():
        data = Path(item['path']).read_bytes()
        need(len(data) == item['size'] and sha(data) == item['sha256'], f'Artifact drift: {name}')
    need(sha(GOP.read_bytes()) == report['gop']['sha256'], 'GOP drift')
    current_gop = audit_gop(write_report=False)
    candidate = Path(report['artifacts']['candidate']['path']).read_bytes()
    template = Path(report['artifacts']['template']['path']).read_bytes()
    transport = Path(report['artifacts']['transport']['path']).read_bytes()
    need(candidate[0x1224000:0x1224000 + len(transport)] == transport, 'Transport not embedded')
    need(candidate[0x3A1A8:0x3A1C8] == hashlib.sha256(transport).digest(), 'Embedded digest')
    need(candidate[0x2BDA0:0x2BDC0] == hashlib.sha256(template).digest(), 'Embedded identity')
    normalized = bytearray(candidate)
    normalized[0x2BDA0:0x2BDC0] = bytes(32)
    need(normalized == template, 'Canonical template differs')
    fv_report = compare_fv(transport[:0x400000])
    need(not any(transport[0x400000:]), 'Nonzero transport tail')
    old = (FROZEN / 'BIN/MOTO_G20_BOOT_A_EDK2_DXE_POC_PUBLICATION_BOUND_SURGICAL.img').read_bytes()
    need(sha(old) == OLD_IMAGE_SHA, 'Frozen parent image drift')
    cursor = 0
    for start, end in [(0x2BDA0, 0x2BDC0), (0x3A1A8, 0x3A1C8), (0x1224000, 0x1624000), (len(old), len(old))]:
        need(candidate[cursor:start] == old[cursor:start], 'Unexpected candidate change')
        cursor = end
    return {'status': 'PASS_SAVED_ARTIFACT_REPLAY', 'candidate_sha256': sha(candidate),
            'preserved_ffs': len(fv_report['unchanged_ffs']), 'new_gop_dc_zva_count': len(current_gop['dc_zva_rvas']),
            'physical_result': 'NOT_TESTED', 'device_io': 'NONE'}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['gop', 'candidate', 'baseline', 'verify'])
    action = parser.parse_args().action
    if action == 'gop':
        result = audit_gop()
    elif action == 'candidate':
        result = build_candidate()
    elif action == 'verify':
        result = verify_candidate()
    else:
        raw = (ROOT / 'FV/baseline_reproduced.Fv').read_bytes()
        need(sha(raw) == OLD_FV_SHA, 'Baseline digest')
        result = {'baseline_files': len(parse_fv(raw)), 'sha256': sha(raw)}
    print(json.dumps(result, indent=2))

"""Host-only transcription and conditional binary crosswalk for the supplied photo."""
import hashlib
import json
import shutil
import struct
from pathlib import Path

LAB = Path(r'C:\MOTO_G20_WOA_LAB')
FREEZE = LAB / '05_EDK2/EDK2_FOUNDATION_DXE_POC_PUBLICATION_SURGICAL_CANDIDATE_HOST_STATIC_PASS'

def sha(data):
    return hashlib.sha256(data).hexdigest().upper()

def pe_sections(raw):
    pe = struct.unpack_from('<I', raw, 0x3c)[0]
    if raw[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Invalid PE signature')
    machine, count = struct.unpack_from('<HH', raw, pe + 4)
    if machine != 0xAA64:
        raise ValueError('Not AArch64')
    opt_size = struct.unpack_from('<H', raw, pe + 20)[0]
    result = []
    for i in range(count):
        pos = pe + 24 + opt_size + 40 * i
        name = raw[pos:pos + 8].rstrip(b'\0').decode('ascii')
        virtual_size, rva, raw_size, offset = struct.unpack_from('<IIII', raw, pos + 8)
        flags = struct.unpack_from('<I', raw, pos + 36)[0]
        if offset + raw_size > len(raw):
            raise ValueError('PE section out of file bounds')
        result.append({'name': name, 'rva': rva, 'offset': offset, 'size': raw_size,
                       'virtual_size': virtual_size, 'executable': bool(flags & 0x20000000)})
    return result

def word_at(raw, sections, rva):
    for section in sections:
        if section['rva'] <= rva and rva + 4 <= section['rva'] + section['size']:
            return struct.unpack_from('<I', raw, section['offset'] + rva - section['rva'])[0]
    raise ValueError('RVA is not backed by a section')

def decode_esr(esr):
    return {'ec': (esr >> 26) & 0x3f, 'il': (esr >> 25) & 1,
            'isv': (esr >> 24) & 1, 'wnr': (esr >> 6) & 1,
            'dfsc': esr & 0x3f, 'far_not_valid': (esr >> 10) & 1,
            'cache_maintenance': (esr >> 8) & 1}

def crosswalk(raw, elr, lr):
    sections = pe_sections(raw)
    matches = []
    for section in sections:
        if not section['executable']:
            continue
        for rel in range(0, section['size'] - 3, 4):
            word = struct.unpack_from('<I', raw, section['offset'] + rel)[0]
            if word != 0xD50B7423:  # DC ZVA, X3 seen in this exact SetMem implementation
                continue
            rva = section['rva'] + rel
            base = elr - rva
            if base < 0 or base % 4096:
                continue
            try:
                call = word_at(raw, sections, lr - base - 4)
            except ValueError:
                continue
            if call & 0xFC000000 != 0x94000000:
                continue
            displacement = call & 0x3ffffff
            if displacement & 0x2000000:
                displacement -= 0x4000000
            matches.append({'inferred_base': hex(base), 'fault_rva': hex(rva),
                            'return_rva': hex(lr - base), 'call_instruction': hex(call),
                            'call_target_rva': hex(lr - base - 4 + displacement * 4)})
    return matches

def main():
    out = Path(r'C:\Users\Angelo\Documents\Codex\2026-09-08\ana\outputs\diagnostico_gop_20260909')
    out.mkdir(exist_ok=False)
    photo = Path(r'D:\Temp\codex-clipboard-cb75efb5-09ae-4084-b1bd-e29c41bcc6e7.jpg')
    shutil.copy2(photo, out / 'pantalla_original.jpg')
    catalog = json.loads((FREEZE / 'REPORTS/DXE_ENTRY_GATE_POC_PUBLICATION.json').read_text())['module_catalog']
    matches = []
    for module in catalog:
        path = Path(module['path'])
        raw = path.read_bytes()
        assert sha(raw) == module['sha256'], f"Baseline changed: {path}"
        for match in crosswalk(raw, 0x871FAB88, 0x871FCA8C):
            matches.append({'module': module['name'], 'module_sha256': sha(raw), **match})
    gop_matches = [x for x in matches if x['module'] == 'MotoG20FramebufferGopDxe.efi']
    assert len(gop_matches) == 1
    assert gop_matches[0]['inferred_base'] == '0x871f9000'
    assert gop_matches[0]['call_target_rva'] == '0x1a6c'
    decoded = decode_esr(0x96000061)
    assert decoded == {'ec': 37, 'il': 1, 'isv': 0, 'wnr': 1, 'dfsc': 33, 'far_not_valid': 0, 'cache_maintenance': 0}
    report = {
        'schema': 'moto-g20-gop-photo-binary-crosswalk-v1',
        'photo': {'original_path': str(photo), 'sha256': sha(photo.read_bytes()), 'transcription': 'Manual from the supplied full-resolution image'},
        'visible': {'stage': 'DXE EXCEPTION', 'status': '0x8000000000000015', 'detail0': '0x0', 'detail1': '0x96000061',
                    'current_el_register': '0x8', 'sctlr_el2': '0x30C50835', 'tcr_el2': '0x80823518', 'ttbr0_el2': '0x81981000',
                    'mair_el2': '0xFFBB4400', 'vbar_el2': '0x87318000', 'esr_el2': '0x96000061',
                    'elr_el2': '0x871FAB88', 'far_el2': '0x9E000080', 'sp': '0x874B6BD0', 'lr': '0x871FCA8C',
                    'image_id': '0x0', 'image_base': '0x0', 'image_size': '0x0', 'entry_point': '0x0',
                    'entry_code_pre': '0x0', 'entry_code_post': '0x0', 'fault_code': '0x0', 'entry_flags': '0x0',
                    'entry_sequence': '0x0', 'pstore_code': '0x0', 'footer': 'AUTO RECOVERY IN 20 SECONDS'},
        'decoded_esr': decoded,
        'esr_meaning': 'Data Abort at current EL; write; alignment fault; FAR valid. ISV=0 so access size is not decoded from ISS.',
        'all_catalog_matches': matches,
        'gop_crosswalk': gop_matches[0],
        'framebuffer': {'base': '0x9E000000', 'fault_offset': '0x80', 'length': '0x465000',
                        'fault_address_aligned_to_64_bytes': True,
                        'source_mapping': 'RegisterFramebufferMemory requests EFI_MEMORY_UC; ArmCpuDxe translates that policy to Device + XN.'},
        'interpretation': 'Strongly consistent with DC ZVA in the GOP black-fill path on Device-mapped framebuffer; this is not evidence of a misaligned framebuffer pointer.',
        'attribution_limit': 'The photo has no candidate SHA or nonzero image identity. Association with latest PoC dispatch is based on user testimony and matching binary/source evidence, not a cryptographic identity read from the panel.',
        'zero_entry_fields': 'Consistent with CpuDxe owning the exception vector and a separate statically linked telemetry instance; they do not show that no driver ran.',
        'unproven': ['Automatic recovery actually occurred', 'Android B is currently safe', 'GOP protocol installed successfully', 'UEFI console/Shell works', 'Windows boots', 'Any proposed repair works physically'],
        'reference': 'https://documentation-service.arm.com/static/69aac74fe79f9a1d642aa91f?token=',
        'device_io': 'NONE', 'retry_latest_candidate': 'PERMANENTLY_FORBIDDEN',
    }
    (out / 'PHOTO_CROSSWALK.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps({'gop_match': gop_matches[0], 'all_matches': len(matches), 'esr': decoded, 'out': str(out)}, indent=2))

if __name__ == '__main__':
    main()

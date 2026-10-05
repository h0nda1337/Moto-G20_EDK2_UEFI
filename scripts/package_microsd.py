#!/usr/bin/env python3
"""Package six compiled microSD modules into the pinned UEFI baseline, offline."""
import argparse
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('sd_pack', ROOT / 'development/microsd_v1/packaging/package_microsd_candidate.py')
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'artifacts/MOTO_G20_CONSOLE_FLAT_GOP_HOST_ONLY.img')
    parser.add_argument('--output', type=Path, default=ROOT / '.build/packaged')
    args = parser.parse_args()
    output = args.output.resolve()
    reports = ROOT / '.build/reports'
    p.need((reports / 'exit_code.txt').read_text().strip() == '0', 'Build must succeed first')
    log = (reports / 'build.log').read_text(errors='replace')
    p.need('- Done -' in log and '- Failed -' not in log, 'EDK2 completion missing')
    current = subprocess.check_output([sys.executable, str(ROOT / 'scripts/source_manifest.py')], text=True)
    p.need(json.loads(current) == json.loads((reports / 'sources.json').read_text()), 'Build sources changed: rebuild required')
    parent = args.baseline.read_bytes()
    p.need(p.sha(parent) == p.PARENT_SHA, 'Pinned baseline SHA256 mismatch')
    baseline = parent[p.TRANSPORT_OFFSET:p.TRANSPORT_OFFSET + p.FV_SIZE]
    p.need(p.sha(baseline) == p.FV_SHA, 'Pinned baseline FV mismatch')
    before = p.parse_fv(baseline, 23)
    specs = [*p.FIXED[:2], ('MotoG20SdPlatformDxe', p.PLATFORM_GUID,
             'MotoG20Pkg/Drivers/MotoG20SdPlatformDxe/MotoG20SdPlatformDxe.inf', 'DXE_DRIVER'), *p.FIXED[2:]]
    modules = {}
    arch = ROOT / '.build/BUILD/DEBUG_GCCNOLTO/AARCH64'
    for name, guid, relative, kind in specs:
        origin = ROOT if relative.startswith('MotoG20Pkg/') else ROOT / 'edk2'
        p.need(p.inf_definitions(origin / relative) == {'BASE_NAME': name, 'FILE_GUID': guid, 'MODULE_TYPE': kind}, 'INF identity changed')
        data = (arch / (name + '.efi')).read_bytes()
        p.pe_audit(data)
        module_dir = arch / Path(relative).parent / Path(relative).stem / 'OUTPUT'
        p.need(data == (module_dir / (name + '.efi')).read_bytes(), 'Module output mismatch')
        depex = (module_dir / (name + '.depex')).read_bytes() if kind == 'DXE_DRIVER' else None
        if depex is not None:
            p.need(0 < len(depex) <= 4096 and depex[-1] == 8, 'Invalid DEPEX')
        modules[guid] = {'name': name, 'data': data, 'depex_data': depex}
    p.need(not output.exists(), 'Select a new output directory; existing output is retained')
    output.mkdir(parents=True)
    folder = output / 'FVINPUT'
    folder.mkdir()
    commands = []
    ext = folder / 'MOTOG20FOUNDATIONFV.ext'
    ext.write_bytes((ROOT / 'config/MOTOG20FOUNDATIONFV.ext').read_bytes())
    prefix = (ROOT / 'config/fv-prefix.inf').read_text() + 'EFI_FV_EXT_HEADER_FILE_NAME = ' + str(ext) + '\n'

    def run(tool, args):
        exe = ROOT / 'edk2/BaseTools/Source/C/bin' / tool
        subprocess.run([str(exe), *args], check=True, capture_output=True)
        commands.append({'tool': tool, 'args': args, 'tool_sha256': p.sha(exe.read_bytes())})

    def make_fv(name, paths):
        inf = folder / (name + '.inf')
        inf.write_text(prefix + '[files]\n' + ''.join('EFI_FILE_NAME = ' + str(path) + '\n' for path in paths))
        dest = output / (name + '.Fv')
        run('GenFv', ['-i', str(inf), '-o', str(dest)])
        return dest.read_bytes()

    paths = []
    for row in before:
        data = bytearray(row['raw'])
        data[23] ^= 0xff
        path = folder / (row['guid'] + '.ffs')
        path.write_bytes(data)
        paths.append(path)
    p.need(make_fv('baseline_reproduced', paths) == baseline, 'Baseline reconstruction differs')
    for guid, module in modules.items():
        sections = p.expected_sections(module)
        if guid == p.BDS:
            old = next(row for row in before if row['guid'] == guid)
            sections = [{'type': s['type'], 'data': module['data'] if s['type'] == 0x10 else
                         module['depex_data'] if s['type'] == 0x13 else s['data']} for s in old['sections']]
        section_folder = folder / module['name']
        section_folder.mkdir()
        flags = []
        for index, section in enumerate(sections):
            dest = section_folder / (str(index) + '.section')
            if section['type'] in (0x10, 0x13):
                raw = section_folder / (str(index) + '.payload')
                raw.write_bytes(section['data'])
                kind = 'EFI_SECTION_PE32' if section['type'] == 0x10 else 'EFI_SECTION_DXE_DEPEX'
                run('GenSec', ['-s', kind, '-o', str(dest), str(raw)])
            else:
                dest.write_bytes((len(section['data']) + 4).to_bytes(3, 'little') + bytes([section['type']]) + section['data'])
            flags += ['-oi', str(dest)]
        ffs = section_folder / (guid + '.ffs')
        run('GenFfs', ['-t', 'EFI_FV_FILETYPE_DRIVER', '-g', guid, '-o', str(ffs), *flags])
        if guid == p.BDS:
            paths[[r['guid'] for r in before].index(guid)] = ffs
        else:
            paths.append(ffs)
    fv = make_fv('MOTOG20MICROSDFV', paths)
    result = p.verify_fv(fv, baseline, modules)
    transport = fv + bytes(p.TRANSPORT_SIZE - len(fv))
    template = bytearray(parent)
    template[p.IDENTITY] = bytes(32)
    template[p.TRANSPORT_OFFSET:p.TRANSPORT_OFFSET + p.TRANSPORT_SIZE] = transport
    template[p.DIGEST] = hashlib.sha256(transport).digest()
    image = bytearray(template)
    image[p.IDENTITY] = hashlib.sha256(template).digest()
    p.verify_image(image, parent, transport, template, fv)
    dest = output / 'MOTO_G20_MICROSD_HOST_ONLY.img'
    dest.write_bytes(image)
    result.update({'status': 'PASS_OFFLINE_STRUCTURE_ONLY', 'image_sha256': p.sha(image),
                   'baseline_image_sha256': p.PARENT_SHA, 'physical_result': 'NOT_TESTED',
                   'module_sha256': {g: p.sha(m['data']) for g, m in modules.items()},
                   'commands': commands, 'device_operations': []})
    (output / 'PACKAGING_REPORT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'status': result['status'], 'image': str(dest), 'sha256': p.sha(image),
                      'ffs_count': 28, 'preserved_ffs': 22}, indent=2))

if __name__ == '__main__':
    main()

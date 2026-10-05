"""Preserve a completed BDS/USB-HC build; never creates an image or uses a device.

The overlay is compared with the manifest captured by the build launcher. Shared
dependencies are copied as they exist at recording time (not falsely attested as
a pre-build snapshot). Build products and generated dependency lists are copied
so subsequent compilations cannot overwrite this evidence.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import struct
import sys
from datetime import datetime, timezone

sys.dont_write_bytecode = True
from gop_candidate import pe_audit

ROOT = pathlib.Path(r'C:\MOTO_G20_WOA_LAB\AUTOMATION\BUILD_ARTIFACTS\USB_HOST_20260929_V6')
EDK = pathlib.Path(r'C:\MOTO_G20_WOA_LAB\05_EDK2\edk2')
BUILD = ROOT / 'BUILD/DEBUG_GCCNOLTO/AARCH64'
OVERLAY = ROOT / 'WORKSPACE/MotoG20Pkg'
MODULES = {
    'BDS': ('BdsDxe', 'MotoG20Pkg/Diagnostics/BdsDxe/BdsDxe'),
    'HC': ('MotoG20MusbDxe', 'MotoG20Pkg/Drivers/MotoG20MusbDxe/MotoG20MusbDxe'),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def inventory(root):
    return [{'path': p.relative_to(root).as_posix(), 'size': p.stat().st_size,
             'sha256': sha(p)} for p in sorted(root.rglob('*')) if p.is_file()]


def by_path(rows):
    result = {row['path']: row for row in rows}
    assert len(result) == len(rows), 'Duplicate paths in manifest'
    return result


def windows_path(raw):
    match = re.fullmatch(r'/mnt/([a-z])/(.+)', raw)
    assert match, 'Unexpected WSL path: ' + raw
    return pathlib.Path(match[1].upper() + ':/' + match[2])


def save_json(path, obj):
    path.write_text(json.dumps(obj, indent=2) + '\n', encoding='utf-8')


def copy_record(source, destination):
    assert source.is_file(), 'Missing artifact: ' + str(source)
    before = sha(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    assert before == sha(source) == sha(destination), 'Changed during capture: ' + str(source)
    return {'path': str(source), 'evidence_path': str(destination),
            'size': destination.stat().st_size, 'sha256': before}


def verify_existing(run):
    report = json.loads((run / 'BUILD_AUDIT.json').read_text())
    assert report['status'] == 'PASS_HOST_COMPILATION_AND_PE_STATIC_CHECKS'
    rows = json.loads((run / 'EVIDENCE_MANIFEST.json').read_text())
    for row in rows:
        path = run / row['path']
        assert path.stat().st_size == row['size'] and sha(path) == row['sha256'], str(path)
    print(json.dumps({'status': 'PASS_PRESERVED_EVIDENCE', 'files': len(rows), 'run': str(run)}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', choices=MODULES)
    parser.add_argument('tag')
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    assert re.fullmatch(r'[A-Za-z0-9_-]+', args.tag), 'Invalid run tag'
    name, relative = MODULES[args.module]
    run = ROOT / 'REPORTS' / (args.module + '_' + args.tag)
    if args.verify_only:
        verify_existing(run)
        return
    assert not (ROOT / 'BUILD_TRANCHE.lock').exists(), 'Concurrent build present'
    assert (run / 'exit_code.txt').read_text().strip() == '0', 'Build did not pass'
    assert '- Done -' in (run / 'build.log').read_text(), 'Missing build completion marker'
    expected = json.loads((run / 'overlay_sha256.json').read_text())
    actual = inventory(OVERLAY)
    assert by_path(actual) == by_path(expected), 'Overlay changed since build started'
    module = BUILD / relative
    inf = OVERLAY / pathlib.PurePosixPath(relative).relative_to('MotoG20Pkg').parent / (name + '.inf')
    inf_text = inf.read_text()
    assert re.search(r'^\s*MODULE_TYPE\s*=\s*DXE_DRIVER\s*$', inf_text, re.M), 'Unexpected module type'
    pe_path = BUILD / (name + '.efi')
    raw = pe_path.read_bytes()
    pe = pe_audit(raw)
    header = struct.unpack_from('<I', raw, 0x3c)[0]
    assert struct.unpack_from('<H', raw, header + 4)[0] == 0xaa64, 'PE is not AARCH64'
    assert struct.unpack_from('<H', raw, header + 24)[0] == 0x20b, 'PE is not PE32+'
    dll = module / 'DEBUG' / (name + '.dll')
    elf = dll.read_bytes()
    assert elf[:6] == b'\x7fELF\x02\x01', 'DLL is not ELF64 LE'
    assert struct.unpack_from('<H', elf, 18)[0] == 183, 'ELF is not AARCH64'
    depex = module / 'OUTPUT' / (name + '.depex')
    assert depex.is_file() and depex.stat().st_size > 0, 'Missing linked DXE DEPEX'
    evidence = run / 'OUTPUT_EVIDENCE'
    source_snapshot = run / 'SOURCE_SNAPSHOT'
    assert not evidence.exists() and not source_snapshot.exists(), 'Evidence already exists'
    evidence.mkdir()
    source_snapshot.mkdir()
    outputs = [pe_path, dll, module / 'DEBUG' / (name + '.map'),
               module / 'DEBUG/AutoGen.c', module / 'DEBUG/AutoGen.h', depex,
               module / 'GNUmakefile', module / (name + '.makefile'),
               module / 'OUTPUT/static_library_files.lst', module / 'OUTPUT/object_files.lst']
    artifacts = [copy_record(p, evidence / p.relative_to(BUILD)) for p in outputs]
    libraries = [windows_path(line.strip()) for line in
                 (module / 'OUTPUT/static_library_files.lst').read_text().splitlines() if line.strip()]
    build_dirs = {module}
    for library in libraries:
        library.relative_to(BUILD)
        artifacts.append(copy_record(library, evidence / library.relative_to(BUILD)))
        build_dirs.add(library.parent.parent)
    source_paths = set()
    dependency_records = []
    for directory in sorted(build_dirs):
        files = set(directory.rglob('*.deps'))
        files.update(p for p in (directory / 'GNUmakefile', directory / 'DEBUG/AutoGen.c',
                                directory / 'DEBUG/AutoGen.h') if p.is_file())
        for path in sorted(files):
            destination = evidence / path.relative_to(BUILD)
            if not destination.exists():
                dependency_records.append(copy_record(path, destination))
            content = path.read_text()
            # Compiler dependency lines contain absolute WSL paths without spaces.
            for item in re.findall(r'/mnt/[a-z]/[^\s\\]+', content):
                candidate = windows_path(item)
                if candidate.is_file() and (candidate.is_relative_to(EDK) or candidate.is_relative_to(OVERLAY)):
                    source_paths.add(candidate)
            if path.name == 'GNUmakefile':
                directory_match = re.search(r'^MODULE_DIR = (.+)$', content, re.M)
                file_match = re.search(r'^MODULE_FILE = (.+)$', content, re.M)
                assert directory_match and file_match, str(path)
                source_paths.add(windows_path(directory_match[1]) / file_match[1])
    for package in ('MdePkg', 'MdeModulePkg', 'ArmPkg', 'EmbeddedPkg', 'UefiCpuPkg', 'ShellPkg'):
        source_paths.add(EDK / package / (package + '.dec'))
    shared_records = []
    previous_path = ROOT / 'REPORTS/SHARED_DEPENDENCIES.json'
    previous = by_path(json.loads(previous_path.read_text())['files']) if previous_path.exists() else {}
    shared_overlap = 0
    for source in sorted(source_paths):
        if source.is_relative_to(EDK):
            rel = source.relative_to(EDK)
            row = copy_record(source, source_snapshot / 'EDK2' / rel)
            if rel.as_posix() in previous:
                old = previous[rel.as_posix()]
                assert row['sha256'] == old['sha256'] and row['size'] == old['size'], 'Shared source drift: ' + str(source)
                shared_overlap += 1
            shared_records.append(row)
    overlay_records = [copy_record(OVERLAY / row['path'], source_snapshot / 'MotoG20Pkg' / row['path']) for row in actual]
    configuration_records = [copy_record(p, source_snapshot / 'CONF' / p.relative_to(ROOT / 'CONF'))
                             for p in sorted((ROOT / 'CONF').rglob('*')) if p.is_file()]
    launcher = pathlib.Path(__file__).with_name('build_usb_host.sh')
    for source in (pathlib.Path(__file__), launcher):
        configuration_records.append(copy_record(source, source_snapshot / 'RECORDING_TOOLS' / source.name))
    assert not (ROOT / 'BUILD_TRANCHE.lock').exists(), 'Build started during capture'
    assert by_path(inventory(OVERLAY)) == by_path(expected), 'Overlay changed during capture'
    for row in artifacts + dependency_records + shared_records + overlay_records + configuration_records:
        assert sha(pathlib.Path(row['path'])) == row['sha256'], 'Input changed during recording'
    closure = {
        'scope': 'Full MotoG20 overlay; linked module/library compiler-listed shared sources and module INFs; package DECs; current CONF; generated dependency lists and linked archives.',
        'snapshot_time': 'Post-build recording; overlay matches build-start manifest. Shared-source overlap is verified against earlier generic-build dependency snapshot. Other shared sources have no pre-build attestation.',
        'shared_sources': shared_records, 'shared_sources_matching_previous_snapshot': shared_overlap,
        'overlay': overlay_records, 'configuration': configuration_records,
        'generated_dependency_evidence': dependency_records,
    }
    save_json(run / 'SOURCE_CLOSURE.json', closure)
    report = {
        'status': 'PASS_HOST_COMPILATION_AND_PE_STATIC_CHECKS', 'module': name,
        'recorded_utc': datetime.now(timezone.utc).isoformat(), 'build_exit_code': 0,
        'module_type': 'DXE_DRIVER', 'build_started_utc': (run / 'started_utc.txt').read_text().strip(),
        'build_finished_utc': (run / 'finished_utc.txt').read_text().strip(),
        'logical_cpus': int((run / 'logical_cpus.txt').read_text().strip()),
        'source_overlay_manifest_sha256': sha(run / 'overlay_sha256.json'),
        'source_closure_sha256': sha(run / 'SOURCE_CLOSURE.json'),
        'build_log_sha256': sha(run / 'build.log'), 'toolchain_manifest_sha256': sha(run / 'toolchain.txt'),
        'artifacts': artifacts, 'pe': pe, 'elf_machine': 'AARCH64',
        'depex': {'size': depex.stat().st_size, 'sha256': sha(depex), 'hex': depex.read_bytes().hex()},
        'hardware_usb_verified': False, 'packaged_image_created': False,
        'limitations': ['Host compilation and static evidence do not prove VBUS, enumeration, hub support, or keyboard input on the phone.',
                        'Source closure is recorded after compilation; consult SOURCE_CLOSURE.json for temporal limits.',
                        'This recorder has no device transport and grants no physical execution or retry authorization.'],
    }
    save_json(run / 'BUILD_AUDIT.json', report)
    save_json(run / 'EVIDENCE_MANIFEST.json', inventory(run))
    print(json.dumps({'status': report['status'], 'module': name, 'pe_sha256': sha(pe_path),
                      'overlay_files': len(overlay_records), 'shared_source_files': len(shared_records),
                      'previous_shared_overlap': shared_overlap, 'report': str(run / 'BUILD_AUDIT.json')}, indent=2))


if __name__ == '__main__':
    main()

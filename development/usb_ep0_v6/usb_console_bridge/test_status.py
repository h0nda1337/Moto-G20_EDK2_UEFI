"""Test the V6 observer, actual producer and passive descriptor getter contract."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

HERE = Path(__file__).resolve().parent
V6 = HERE.parent
EDK = Path(r'C:\MOTO_G20_WOA_LAB\05_EDK2\edk2')
SOURCE = V6 / 'usb_musb_driver/MotoG20MusbDxe.c'
PROTOCOL = V6 / 'usb_musb_driver/MotoG20UsbDiagnostic.h'
ROLE_HEADER = V6 / 'usb_platform_driver/Library/MotoG20UsbPlatformLib.h'
USB_BUS = EDK / 'MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBus.c'
# A failed rerun must not leave the old PASS looking like current evidence.
(HERE / 'STATUS_TEST.json').write_text(json.dumps({
    'status': 'INCOMPLETE_HOST_TEST', 'hardware_validated': False}, indent=2), encoding='utf-8')
(HERE / 'STATUS_TEST.txt').write_text('Host test started; no current PASS yet.\n', encoding='utf-8')
text = SOURCE.read_text(encoding='utf-8')
signature = 'STATIC EFI_STATUS EFIAPI ReadDiagnostic ('
start = text.index(signature, text.index('STATIC VOID Record ('))
brace = text.index('{', start)
depth = 1
end = brace + 1
while depth:
    depth += (text[end] == '{') - (text[end] == '}')
    end += 1
producer = text[start:end]
(HERE / 'producer_under_test.h').write_text(producer + '\n', encoding='utf-8')
bus_text = USB_BUS.read_text(encoding='utf-8')
cached_getters = {}
for name, cached_field in [('UsbIoGetDeviceDescriptor', 'Dev->DevDesc->Desc'),
                           ('UsbIoGetInterfaceDescriptor', 'UsbIf->IfSetting->Desc')]:
    start = bus_text.index(name + ' (')
    brace = bus_text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (bus_text[end] == '{') - (bus_text[end] == '}')
        end += 1
    body = bus_text[brace:end]
    assert cached_field in body and 'CopyMem (' in body
    calls = set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', re.sub(r'/\*.*?\*/|//[^\n]*', '', body, flags=re.S)))
    assert calls <= {'if', 'RaiseTPL', 'USB_INTERFACE_FROM_USBIO', 'CopyMem', 'sizeof', 'RestoreTPL'}, calls
    cached_getters[name] = hashlib.sha256(body.encode()).hexdigest().upper()
include = HERE / 'test_include/Protocol'
include.mkdir(parents=True, exist_ok=True)
(include / PROTOCOL.name).write_bytes(PROTOCOL.read_bytes())
exe = HERE / 'status_test.exe'
cmd = ['gcc', '-std=c11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
       '-I' + str(HERE / 'test_include'), '-I' + str(V6 / 'usb_platform_driver'),
       '-I' + str(EDK / 'MdePkg/Include'), '-I' + str(EDK / 'MdePkg/Include/X64'),
       '-I' + str(EDK / 'MdeModulePkg/Include'), str(HERE / 'status_test.c'), '-o', str(exe)]
build = subprocess.run(cmd, capture_output=True, text=True)
print(build.stdout + build.stderr)
assert build.returncode == 0
run = subprocess.run([str(exe)], capture_output=True, text=True)
print(run.stdout + run.stderr)
(HERE / 'STATUS_TEST.txt').write_text(run.stdout + run.stderr, encoding='utf-8')
assert run.returncode == 0
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest().upper()
(HERE / 'STATUS_TEST.json').write_text(json.dumps({
    'status': 'PASS_HOST_PRODUCER_OBSERVER_ONLY', 'hardware_validated': False,
    'compile': cmd, 'stdout': run.stdout, 'producer_source': str(SOURCE),
    'producer_sha256': sha(SOURCE), 'tested_function_sha256': sha(HERE / 'producer_under_test.h'),
    'protocol_sha256': sha(PROTOCOL), 'role_header_sha256': sha(ROLE_HEADER),
    'observer_sha256': sha(HERE / 'MotoG20UsbConsoleStatus.c'),
    'regression_groups': 35, 'protocol_revision': 6, 'role_revision': 2,
    'cached_descriptor_source': str(USB_BUS), 'cached_descriptor_source_sha256': sha(USB_BUS),
    'cached_descriptor_getter_sha256': cached_getters,
    'harness_sha256': sha(HERE / 'status_test.c')}, indent=2), encoding='utf-8')

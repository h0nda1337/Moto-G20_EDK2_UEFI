"""Compile policy + exact upstream USB-class matcher using real EDK2 headers."""
from pathlib import Path
import hashlib
import json
import subprocess

HERE = Path(__file__).resolve().parent
EDK = Path(r'C:\MOTO_G20_WOA_LAB\05_EDK2\edk2')
SOURCE = EDK / 'MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatform.c'
text = SOURCE.read_text(encoding='utf-8')
start = text.index('BOOLEAN\nMatchUsbClass (')
end = text.index('\n/**', start)
matcher = '\n'.join(line for line in text.splitlines() if line.startswith('#define USB_')) + '\n' + text[start:end]
test = r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#undef NULL
#include <Uefi.h>
#include <Protocol/UsbIo.h>
#include <Library/DevicePathLib.h>
#include "MotoG20UsbConsolePolicy.c"

static USB_CLASS_DEVICE_PATH Selected;
static EFI_USB_DEVICE_DESCRIPTOR Dev;
static EFI_USB_INTERFACE_DESCRIPTOR If;
static EFI_STATUS UpdateStatus, DevStatus, IfStatus;
static unsigned Updated, IfCalls;
UINT8 EFIAPI DevicePathType(CONST VOID *P) { return ((const EFI_DEVICE_PATH_PROTOCOL *)P)->Type; }
UINT8 EFIAPI DevicePathSubType(CONST VOID *P) { return ((const EFI_DEVICE_PATH_PROTOCOL *)P)->SubType; }
EFI_STATUS EFIAPI EfiBootManagerUpdateConsoleVariable(CONSOLE_TYPE Type,
  EFI_DEVICE_PATH_PROTOCOL *Added, EFI_DEVICE_PATH_PROTOCOL *Deleted) {
  const unsigned char Expected[] = {3,15,11,0,255,255,255,255,3,1,1,127,255,4,0};
  assert(Type == ConIn && Deleted == NULL);
  assert(sizeof(mKeyboardPath) == sizeof Expected);
  assert(memcmp(Added, Expected, sizeof Expected) == 0);
  memcpy(&Selected, Added, sizeof Selected);
  Updated++;
  return UpdateStatus;
}
static EFI_STATUS EFIAPI GetDevice(EFI_USB_IO_PROTOCOL *P, EFI_USB_DEVICE_DESCRIPTOR *D) {
  (void)P; *D=Dev; return DevStatus;
}
static EFI_STATUS EFIAPI GetInterface(EFI_USB_IO_PROTOCOL *P, EFI_USB_INTERFACE_DESCRIPTOR *D) {
  (void)P; *D=If; IfCalls++; return IfStatus;
}
__MATCHER__
int main(void) {
  EFI_USB_IO_PROTOCOL Io={0};
  Io.UsbGetDeviceDescriptor=GetDevice;
  Io.UsbGetInterfaceDescriptor=GetInterface;
  assert(MotoG20RegisterUsbKeyboardConsole() == EFI_SUCCESS && Updated == 1);
  UpdateStatus=EFI_OUT_OF_RESOURCES;
  assert(MotoG20RegisterUsbKeyboardConsole() == EFI_OUT_OF_RESOURCES);
  Dev.IdVendor=0x1234; Dev.IdProduct=0x5678;
  If.InterfaceClass=3; If.InterfaceSubClass=1; If.InterfaceProtocol=1;
  assert(MatchUsbClass(&Io,&Selected)); /* Composite keyboard interface. */
  If.InterfaceProtocol=2; assert(!MatchUsbClass(&Io,&Selected)); /* Mouse rejected. */
  If.InterfaceProtocol=1; If.InterfaceSubClass=0;
  assert(!MatchUsbClass(&Io,&Selected)); /* Non-boot HID rejected. */
  If.InterfaceSubClass=1; IfStatus=EFI_DEVICE_ERROR;
  assert(!MatchUsbClass(&Io,&Selected)); /* No false match on descriptor failure. */
  IfStatus=EFI_SUCCESS; Dev.DeviceClass=3; Dev.DeviceSubClass=1; Dev.DeviceProtocol=1;
  IfCalls=0; assert(MatchUsbClass(&Io,&Selected) && IfCalls==0);
  DevStatus=EFI_DEVICE_ERROR; assert(!MatchUsbClass(&Io,&Selected));
  DevStatus=EFI_SUCCESS; Dev.DeviceClass=0xef; Dev.DeviceSubClass=2; Dev.DeviceProtocol=1;
  assert(MatchUsbClass(&Io,&Selected)); /* IAD composite keyboard. */
  Selected.VendorId=0x4321; assert(!MatchUsbClass(&Io,&Selected));
  puts("PASS: 10 policy/descriptor scenarios; no hardware validation");
  return 0;
}
'''.replace('__MATCHER__', matcher)
(HERE/'policy_test.c').write_text(test, encoding='utf-8')
exe = HERE/'policy_test.exe'
cmd = ['gcc','-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror',
       '-I'+str(EDK/'MdePkg/Include'), '-I'+str(EDK/'MdePkg/Include/X64'),
       '-I'+str(EDK/'MdeModulePkg/Include'), str(HERE/'policy_test.c'), '-o',str(exe)]
build = subprocess.run(cmd,capture_output=True,text=True)
print(build.stdout+build.stderr)
assert build.returncode == 0
run = subprocess.run([str(exe)],capture_output=True,text=True)
print(run.stdout+run.stderr)
assert run.returncode == 0
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest().upper()
(HERE/'POLICY_TEST.json').write_text(json.dumps({
    'status':'PASS_HOST_POLICY_ONLY', 'compile':cmd, 'stdout':run.stdout,
    'upstream_source':str(SOURCE), 'upstream_sha256':sha(SOURCE),
    'policy_sha256':sha(HERE/'MotoG20UsbConsolePolicy.c'),
    'harness_sha256':sha(HERE/'policy_test.c'), 'hardware_validated':False}, indent=2),encoding='utf-8')

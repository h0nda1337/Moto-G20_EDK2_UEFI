"""Materialize isolated USB proposal; does not package or access the phone."""
from pathlib import Path
import hashlib
import json
import shutil
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
ROOT = Path(r'C:\MOTO_G20_WOA_LAB\AUTOMATION\BUILD_ARTIFACTS\USB_HOST_20260929_V6')
PKG = ROOT/'WORKSPACE/MotoG20Pkg'
PARENT = ROOT/'SOURCE/MotoG20Pkg'

def put(relative, data):
    path = PKG/relative
    path.parent.mkdir(exist_ok=True, parents=True)
    if isinstance(data, str): data = data.encode('utf-8')
    path.write_bytes(data)

def main():
    assert not (ROOT/'BUILD_TRANCHE.lock').exists(), 'Build active; do not change sources'
    for name in ['MotoG20MusbDxe.c','MotoG20MusbDxe.inf','MotoG20MusbRegs.h','MotoG20UsbDiagnostic.h']:
        put('Drivers/MotoG20MusbDxe/'+name, (HERE/'usb_musb_driver'/name).read_bytes())
    for path in (HERE/'usb_platform_driver/Library').iterdir():
        if path.suffix in ['.c','.h','.inf']:
            put('Library/MotoG20UsbPlatformLib/'+path.name, path.read_bytes())
    put('Include/Library/MotoG20UsbPlatformLib.h', (HERE/'usb_platform_driver/Library/MotoG20UsbPlatformLib.h').read_bytes())
    put('Include/Protocol/MotoG20UsbDiagnostic.h', (HERE/'usb_musb_driver/MotoG20UsbDiagnostic.h').read_bytes())
    for name in ['MotoG20UsbConsolePolicy.c','MotoG20UsbConsoleStatus.c']:
        put('Library/MotoG20PlatformBootManagerLib/'+name, (HERE/'usb_console_bridge'/name).read_bytes())
    bds = 'Library/MotoG20PlatformBootManagerLib/'
    source = (PARENT/bds/'MotoG20PlatformBootManagerLib.c').read_text()
    source = source.replace('STATIC BOOLEAN  mShellLaunchAttempted;', '''EFI_STATUS EFIAPI MotoG20RegisterUsbKeyboardConsole (VOID);
EFI_STATUS EFIAPI MotoG20StartUsbConsoleStatus (VOID);
STATIC EFI_STATUS mInputPolicyStatus = EFI_NOT_READY;
STATIC BOOLEAN  mShellLaunchAttempted;''', 1)
    marker = '  Handles     = NULL;'
    before = source.index('PlatformBootManagerBeforeConsole (')
    pos = source.index(marker, before)
    source = source[:pos] + '  mInputPolicyStatus = MotoG20RegisterUsbKeyboardConsole ();\n\n' + source[pos:]
    after = source.index('PlatformBootManagerAfterConsole (')
    pos = source.index('  Status = LaunchBuiltInShell ();', after)
    source = source[:pos] + '''  if (EFI_ERROR (mInputPolicyStatus)) {
    Print (L"USB: keyboard ConIn policy failed: %r\\r\\n", mInputPolicyStatus);
  }
  Status = MotoG20StartUsbConsoleStatus ();
  if (EFI_ERROR (Status)) {
    Print (L"USB: status monitor failed: %r\\r\\n", Status);
  }

''' + source[pos:]
    put(bds+'MotoG20PlatformBootManagerLib.c', source)
    inf = (PARENT/bds/'MotoG20PlatformBootManagerLib.inf').read_text()
    inf = inf.replace('[Sources]', '[Sources]\n  MotoG20UsbConsolePolicy.c\n  MotoG20UsbConsoleStatus.c')
    inf = inf.replace('[Guids]', '[Guids]\n  gEfiConsoleInDeviceGuid\n  gEfiEventExitBootServicesGuid')
    inf = inf.replace('[Protocols]', '[Protocols]\n  gEfiSimpleTextInProtocolGuid\n  gEfiUsbIoProtocolGuid')
    put(bds+'MotoG20PlatformBootManagerLib.inf', inf)
    dec = (PARENT/'MotoG20Pkg.dec').read_text()
    dec = dec.replace('[LibraryClasses]', '[LibraryClasses]\n  MotoG20UsbPlatformLib|Include/Library/MotoG20UsbPlatformLib.h')
    put('MotoG20Pkg.dec',dec)
    dsc = (PKG/'Foundation/MotoG20UsbGeneric.dsc').read_text()
    dsc = dsc.replace('[LibraryClasses]', '[LibraryClasses]\n  MotoG20UsbPlatformLib|MotoG20Pkg/Library/MotoG20UsbPlatformLib/MotoG20UsbPlatformLib.inf',1)
    dsc += '\n  MotoG20Pkg/Drivers/MotoG20MusbDxe/MotoG20MusbDxe.inf\n'
    put('Foundation/MotoG20UsbHost.dsc', dsc)
    rows = [{'path':p.relative_to(PKG).as_posix(),'size':p.stat().st_size,
             'sha256':hashlib.sha256(p.read_bytes()).hexdigest().upper()}
            for p in sorted(PKG.rglob('*')) if p.is_file()]
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    (ROOT/'REPORTS'/('INTEGRATION_'+stamp+'.json')).write_text(json.dumps(rows,indent=2))
    print(json.dumps({'status':'SOURCES_INTEGRATED_NOT_HARDWARE_TESTED','files':len(rows),'utc':stamp}))

if __name__ == '__main__': main()


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
#define USB_BASE_CLASS_MISCELLANEOUS       0xEF
#define USB_MISCELLANEOUS_SUBCLASS_COMMON  0x02
#define USB_MISCELLANEOUS_PROTOCOL_IAD     0x01
BOOLEAN
MatchUsbClass (
  IN EFI_USB_IO_PROTOCOL    *UsbIo,
  IN USB_CLASS_DEVICE_PATH  *UsbClass
  )
{
  EFI_STATUS                    Status;
  EFI_USB_DEVICE_DESCRIPTOR     DevDesc;
  EFI_USB_INTERFACE_DESCRIPTOR  IfDesc;
  UINT8                         DeviceClass;
  UINT8                         DeviceSubClass;
  UINT8                         DeviceProtocol;

  if ((DevicePathType (UsbClass) != MESSAGING_DEVICE_PATH) ||
      (DevicePathSubType (UsbClass) != MSG_USB_CLASS_DP))
  {
    return FALSE;
  }

  //
  // Check Vendor Id and Product Id.
  //
  Status = UsbIo->UsbGetDeviceDescriptor (UsbIo, &DevDesc);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  if ((UsbClass->VendorId != 0xffff) &&
      (UsbClass->VendorId != DevDesc.IdVendor))
  {
    return FALSE;
  }

  if ((UsbClass->ProductId != 0xffff) &&
      (UsbClass->ProductId != DevDesc.IdProduct))
  {
    return FALSE;
  }

  DeviceClass    = DevDesc.DeviceClass;
  DeviceSubClass = DevDesc.DeviceSubClass;
  DeviceProtocol = DevDesc.DeviceProtocol;

  if ((DeviceClass == 0) ||
      ((DeviceClass == USB_BASE_CLASS_MISCELLANEOUS) &&
       (DeviceSubClass == USB_MISCELLANEOUS_SUBCLASS_COMMON) &&
       (DeviceProtocol == USB_MISCELLANEOUS_PROTOCOL_IAD)))
  {
    //
    // If Class in Device Descriptor is set to 0 (Device), or
    // Class/SubClass/Protocol is 0xEF/0x02/0x01 (IAD), use the Class, SubClass
    // and Protocol in Interface Descriptor instead.
    //
    Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &IfDesc);
    if (EFI_ERROR (Status)) {
      return FALSE;
    }

    DeviceClass    = IfDesc.InterfaceClass;
    DeviceSubClass = IfDesc.InterfaceSubClass;
    DeviceProtocol = IfDesc.InterfaceProtocol;
  }

  //
  // Check Class, SubClass and Protocol.
  //
  if ((UsbClass->DeviceClass != 0xff) &&
      (UsbClass->DeviceClass != DeviceClass))
  {
    return FALSE;
  }

  if ((UsbClass->DeviceSubClass != 0xff) &&
      (UsbClass->DeviceSubClass != DeviceSubClass))
  {
    return FALSE;
  }

  if ((UsbClass->DeviceProtocol != 0xff) &&
      (UsbClass->DeviceProtocol != DeviceProtocol))
  {
    return FALSE;
  }

  return TRUE;
}

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

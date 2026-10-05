/** @file
  Select HID boot keyboards as console input, including keyboards attached
  after Shell starts. This policy does not assert that a keyboard is present.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootManagerLib.h>

#pragma pack (1)
typedef struct {
  USB_CLASS_DEVICE_PATH       Keyboard;
  EFI_DEVICE_PATH_PROTOCOL   End;
} MOTO_USB_KEYBOARD_PATH;
#pragma pack ()

STATIC MOTO_USB_KEYBOARD_PATH mKeyboardPath = {
  {
    { MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP,
      { sizeof (USB_CLASS_DEVICE_PATH), 0 } },
    0xFFFF, 0xFFFF, 3, 1, 1
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

EFI_STATUS
EFIAPI
MotoG20RegisterUsbKeyboardConsole (
  VOID
  )
{
  // ConPlatform matches this USB class node against the physical UsbIo child.
  // A non-PCI host must separately ConnectController after publishing Usb2HC:
  // BmConnectUsbShortFormDevicePath only discovers PCI host controllers.
  return EfiBootManagerUpdateConsoleVariable (
           ConIn,
           (EFI_DEVICE_PATH_PROTOCOL *)&mKeyboardPath,
           NULL
           );
}

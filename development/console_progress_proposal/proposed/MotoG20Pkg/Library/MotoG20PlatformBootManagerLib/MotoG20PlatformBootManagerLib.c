/** @file
  Minimal BDS policy for the Moto G20 Foundation console milestone.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MotoG20DxeTelemetryLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/MotoG20DxeProgressControl.h>

STATIC BOOLEAN  mShellLaunchAttempted;

STATIC
EFI_DEVICE_PATH_PROTOCOL *
FindShellDevicePath (
  VOID
  )
{
  EFI_HANDLE                     *Handles;
  UINTN                          HandleCount;
  UINTN                          Index;
  EFI_STATUS                     Status;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FirmwareVolume;
  EFI_DEVICE_PATH_PROTOCOL       *FirmwareVolumePath;
  EFI_DEVICE_PATH_PROTOCOL       *ShellPath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  UINTN                          FileSize;
  EFI_FV_FILETYPE                FileType;
  EFI_FV_FILE_ATTRIBUTES         FileAttributes;
  UINT32                         AuthenticationStatus;

  Handles     = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  ShellPath = NULL;
  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FirmwareVolume
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    FileSize = 0;
    Status = FirmwareVolume->ReadFile (
                               FirmwareVolume,
                               &gUefiShellFileGuid,
                               NULL,
                               &FileSize,
                               &FileType,
                               &FileAttributes,
                               &AuthenticationStatus
                               );
    if (EFI_ERROR (Status)) {
      continue;
    }

    FirmwareVolumePath = DevicePathFromHandle (Handles[Index]);
    if (FirmwareVolumePath == NULL) {
      continue;
    }

    EfiInitializeFwVolDevicepathNode (&FileNode, &gUefiShellFileGuid);
    ShellPath = AppendDevicePathNode (
                  FirmwareVolumePath,
                  (EFI_DEVICE_PATH_PROTOCOL *)&FileNode
                  );
    break;
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  return ShellPath;
}

STATIC
EFI_STATUS
LaunchBuiltInShell (
  VOID
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *ShellPath;
  EFI_HANDLE                ShellImage;
  EFI_STATUS                Status;
  UINTN                     ExitDataSize;
  CHAR16                    *ExitData;

  if (mShellLaunchAttempted) {
    return EFI_ALREADY_STARTED;
  }

  mShellLaunchAttempted = TRUE;
  ShellPath = FindShellDevicePath ();
  if (ShellPath == NULL) {
    return EFI_NOT_FOUND;
  }

  MotoG20DxeTelemetryCheckpoint (
    MotoG20DxeStageShellLoad,
    EFI_SUCCESS,
    (UINT64)(UINTN)ShellPath,
    0U
    );

  ShellImage = NULL;
  Status = gBS->LoadImage (
                  FALSE,
                  gImageHandle,
                  ShellPath,
                  NULL,
                  0,
                  &ShellImage
                  );
  FreePool (ShellPath);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MotoG20DxeTelemetryCheckpoint (
    MotoG20DxeStageShellStart,
    EFI_SUCCESS,
    (UINT64)(UINTN)ShellImage,
    0U
    );

  ExitDataSize = 0;
  ExitData     = NULL;
  Status = gBS->StartImage (ShellImage, &ExitDataSize, &ExitData);
  if (ExitData != NULL) {
    FreePool (ExitData);
  }

  return Status;
}

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  EFI_HANDLE                *Handles;
  UINTN                     HandleCount;
  UINTN                     Index;
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL                *DevicePath;
  MOTO_G20_DXE_PROGRESS_CONTROL_PROTOCOL   *ProgressControl;
  UINT64                                  ProtocolBitmap;

  // Take ownership before console connection can install more text protocols.
  // This checkpoint proves arrival at the BDS callback, not usable text output.
  ProgressControl = NULL;
  ProtocolBitmap = 0U;
  Status = gBS->LocateProtocol (
                  &gMotoG20DxeProgressControlProtocolGuid,
                  NULL,
                  (VOID **)&ProgressControl
                  );
  if (!EFI_ERROR (Status)) {
    if ((ProgressControl == NULL) ||
        (ProgressControl->Revision != MOTO_G20_DXE_PROGRESS_CONTROL_REVISION) ||
        (ProgressControl->Quiesce == NULL))
    {
      Status = EFI_UNSUPPORTED;
    } else {
      Status = ProgressControl->Quiesce (ProgressControl, &ProtocolBitmap);
    }
  }

  if (EFI_ERROR (Status)) {
    MotoG20DxeTelemetryFailAndReset (
      MotoG20DxeStageBeforeConsole,
      Status,
      ProtocolBitmap,
      (UINT64)(UINTN)ProgressControl
      );
    return;
  }

  MotoG20DxeTelemetryCheckpoint (
    MotoG20DxeStageBeforeConsole,
    Status,
    ProtocolBitmap,
    (UINT64)(UINTN)ProgressControl
    );

  Handles     = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    DevicePath = DevicePathFromHandle (Handles[Index]);
    if (DevicePath == NULL) {
      continue;
    }

    Status = EfiBootManagerUpdateConsoleVariable (ConOut, DevicePath, NULL);
    if (!EFI_ERROR (Status)) {
      EfiBootManagerUpdateConsoleVariable (ErrOut, DevicePath, NULL);
    }
  }

  FreePool (Handles);
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_STATUS  Status;

  MotoG20DxeTelemetryCheckpoint (
    MotoG20DxeStageConsoleReady,
    (gST->ConOut == NULL) ? EFI_NOT_READY : EFI_SUCCESS,
    (UINT64)(UINTN)gST->ConOut,
    (UINT64)(UINTN)gST->ConsoleOutHandle
    );

  if (gST->ConOut == NULL) {
    MotoG20DxeTelemetryFailAndReset (
      MotoG20DxeStageFailure,
      EFI_NOT_READY,
      0U,
      0U
      );
  }

  MotoG20DxeTelemetryReleaseDisplay ();
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"Moto G20 UEFI\r\n\r\nStarting built-in UEFI Shell...\r\n");

  Status = LaunchBuiltInShell ();
  if (EFI_ERROR (Status)) {
    Print (L"\r\nFATAL: built-in Shell failed: %r\r\n", Status);
    MotoG20DxeTelemetryFailAndReset (
      MotoG20DxeStageFailure,
      Status,
      (UINT64)(UINTN)gST->ConOut,
      0U
      );
  }
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = LaunchBuiltInShell ();
  if (EFI_ERROR (Status) &&
      (Status != EFI_ALREADY_STARTED) &&
      (gST->ConOut != NULL))
  {
    Print (L"\r\nFATAL: no bootable UEFI application: %r\r\n", Status);
    MotoG20DxeTelemetryFailAndReset (
      MotoG20DxeStageFailure,
      Status,
      (UINT64)(UINTN)gST->ConOut,
      0U
      );
  }
}

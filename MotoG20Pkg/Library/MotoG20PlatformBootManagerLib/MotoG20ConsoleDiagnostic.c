/** @file
  Probe the known physical text route separately from the system console.

  A published protocol or successful call is not proof of visible pixels.
  The two distinct messages are intended for physical observation.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/ConsoleOutDevice.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MotoG20DxeTelemetryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>

STATIC CONST EFI_GUID  mFramebufferVendorGuid = {
  0x3fa80f1d, 0x6627, 0x41de, { 0xa6, 0xef, 0x71, 0xca, 0xa9, 0x57, 0x21, 0xd4 }
};

STATIC
EFI_STATUS
ConsoleFailure (
  IN MOTO_G20_DXE_STAGE  Stage,
  IN EFI_STATUS         Status,
  IN UINT64             Step,
  IN EFI_HANDLE         Handle
  )
{
  MotoG20DxeTelemetryFailAndReset (Stage, Status, Step, (UINT64)(UINTN)Handle);
  return Status;
}

EFI_STATUS
EFIAPI
MotoG20ConsoleProbe (
  VOID
  )
{
  EFI_HANDLE                           *Handles;
  EFI_HANDLE                           PhysicalHandle;
  EFI_DEVICE_PATH_PROTOCOL             *Path;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL      *PhysicalText;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY  *OpenInfo;
  EFI_STATUS                           Status;
  UINTN                                Count;
  UINTN                                Index;
  UINTN                                Matches;
  UINTN                                OpenCount;
  UINTN                                OpenIndex;
  BOOLEAN                              LinkedToConOut;

  Handles         = NULL;
  PhysicalHandle  = NULL;
  PhysicalText    = NULL;
  Count           = 0;
  Matches         = 0;
  LinkedToConOut  = FALSE;
  MotoG20DxeTelemetryCheckpoint (MotoG20DxeStagePhysicalConsole, EFI_SUCCESS, 0U, 0U);
  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles
                  );
  if (EFI_ERROR (Status)) {
    return ConsoleFailure (MotoG20DxeStagePhysicalConsole, Status, 1U, NULL);
  }

  // ConSplitter also publishes a virtual GOP. Select the platform GOP by its
  // exact vendor node instead of taking the first located protocol instance.
  for (Index = 0; Index < Count; Index++) {
    Path = DevicePathFromHandle (Handles[Index]);
    if ((Path != NULL) &&
        (DevicePathType (Path) == HARDWARE_DEVICE_PATH) &&
        (DevicePathSubType (Path) == HW_VENDOR_DP) &&
        (DevicePathNodeLength (Path) == sizeof (VENDOR_DEVICE_PATH)) &&
        CompareGuid (&((VENDOR_DEVICE_PATH *)Path)->Guid, &mFramebufferVendorGuid))
    {
      PhysicalHandle = Handles[Index];
      Matches++;
    }
  }

  FreePool (Handles);
  if (Matches != 1) {
    return ConsoleFailure (
             MotoG20DxeStagePhysicalConsole,
             (Matches == 0) ? EFI_NOT_FOUND : EFI_COMPROMISED_DATA,
             2U, PhysicalHandle
             );
  }

  Status = gBS->HandleProtocol (
                  PhysicalHandle, &gEfiSimpleTextOutProtocolGuid, (VOID **)&PhysicalText
                  );
  if (EFI_ERROR (Status) || (PhysicalText == NULL)) {
    return ConsoleFailure (
             MotoG20DxeStagePhysicalConsole,
             EFI_ERROR (Status) ? Status : EFI_NOT_READY, 3U, PhysicalHandle
             );
  }

  if ((gST->ConOut == NULL) || (gST->ConsoleOutHandle == NULL)) {
    return ConsoleFailure (MotoG20DxeStagePhysicalConsole, EFI_NOT_READY, 4U, PhysicalHandle);
  }

  // This relation proves that the splitter opened the physical console.
  // It does NOT prove that its internal AddDevice completed successfully.
  if ((gST->ConOut == PhysicalText) && (gST->ConsoleOutHandle == PhysicalHandle)) {
    LinkedToConOut = TRUE;
  } else {
    OpenInfo  = NULL;
    OpenCount = 0;
    Status = gBS->OpenProtocolInformation (
                    PhysicalHandle, &gEfiConsoleOutDeviceGuid, &OpenInfo, &OpenCount
                    );
    if (EFI_ERROR (Status)) {
      return ConsoleFailure (MotoG20DxeStagePhysicalConsole, Status, 5U, PhysicalHandle);
    }

    for (OpenIndex = 0; OpenIndex < OpenCount; OpenIndex++) {
      if (((OpenInfo[OpenIndex].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) != 0) &&
          (OpenInfo[OpenIndex].ControllerHandle == gST->ConsoleOutHandle))
      {
        LinkedToConOut = TRUE;
      }
    }

    if (OpenInfo != NULL) {
      FreePool (OpenInfo);
    }
  }

  if (!LinkedToConOut) {
    return ConsoleFailure (MotoG20DxeStagePhysicalConsole, EFI_NOT_READY, 6U, PhysicalHandle);
  }

  MotoG20DxeTelemetryCheckpoint (
    MotoG20DxeStagePhysicalConsole, EFI_SUCCESS, 7U, (UINT64)(UINTN)PhysicalHandle
    );
  // ProgressDxe has already been quiesced. Release this BDS telemetry instance
  // immediately before the actual output calls; fatal diagnostics can reclaim it.
  MotoG20DxeTelemetryReleaseDisplay ();
  Status = PhysicalText->ClearScreen (PhysicalText);
  if (EFI_ERROR (Status)) {
    return ConsoleFailure (MotoG20DxeStageConsoleWrite, Status, 1U, PhysicalHandle);
  }

  Status = gST->ConOut->ClearScreen (gST->ConOut);
  if (EFI_ERROR (Status)) {
    return ConsoleFailure (MotoG20DxeStageConsoleWrite, Status, 2U, gST->ConsoleOutHandle);
  }

  Status = PhysicalText->OutputString (PhysicalText, L"MOTO G20: PHYSICAL TEXT ROUTE\r\n");
  if (EFI_ERROR (Status)) {
    return ConsoleFailure (MotoG20DxeStageConsoleWrite, Status, 3U, PhysicalHandle);
  }

  // Explicit row separation avoids overwriting the physical-route marker if
  // the splitter's cursor has not yet tracked the direct physical write.
  Status = gST->ConOut->SetCursorPosition (gST->ConOut, 0, 2);
  if (EFI_ERROR (Status)) {
    return ConsoleFailure (MotoG20DxeStageConsoleWrite, Status, 4U, gST->ConsoleOutHandle);
  }

  Status = gST->ConOut->OutputString (gST->ConOut, L"MOTO G20: SYSTEM CONOUT ROUTE\r\n");
  if (EFI_ERROR (Status)) {
    return ConsoleFailure (MotoG20DxeStageConsoleWrite, Status, 5U, gST->ConsoleOutHandle);
  }

  MotoG20DxeTelemetryCheckpoint (
    MotoG20DxeStageConsoleWrite, Status, 6U, (UINT64)(UINTN)gST->ConOut
    );
  return Status;
}

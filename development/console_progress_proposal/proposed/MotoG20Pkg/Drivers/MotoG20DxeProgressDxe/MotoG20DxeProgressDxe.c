/** @file
  Early DXE protocol progress monitor for the Moto G20 Foundation FV.
**/

#include <Uefi.h>
#include <Library/MotoG20DxeTelemetryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/Bds.h>
#include <Protocol/Cpu.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HardwareInterrupt.h>
#include <Protocol/MotoG20DxeProgressControl.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/Timer.h>

typedef struct {
  EFI_GUID              *ProtocolGuid;
  MOTO_G20_DXE_STAGE    Stage;
  UINT64                Bit;
} MOTO_G20_PROTOCOL_WATCH;

STATIC CONST MOTO_G20_PROTOCOL_WATCH  mWatches[] = {
  { &gEfiCpuArchProtocolGuid,          MotoG20DxeStageCpuArch,        1ULL << 0 },
  { &gHardwareInterruptProtocolGuid,   MotoG20DxeStageInterruptArch,  1ULL << 1 },
  { &gEfiTimerArchProtocolGuid,        MotoG20DxeStageTimerArch,      1ULL << 2 },
  { &gEfiGraphicsOutputProtocolGuid,   MotoG20DxeStageGraphicsOutput, 1ULL << 3 },
  { &gEfiSimpleTextOutProtocolGuid,    MotoG20DxeStageTextOutput,     1ULL << 4 },
  { &gEfiBdsArchProtocolGuid,          MotoG20DxeStageBdsArch,        1ULL << 5 }
};

STATIC EFI_EVENT  mEvents[ARRAY_SIZE (mWatches)];
STATIC VOID       *mRegistrations[ARRAY_SIZE (mWatches)];
STATIC UINT64     mProtocolBitmap;
STATIC BOOLEAN    mQuiesced;

STATIC
EFI_STATUS
EFIAPI
QuiesceProgress (
  IN MOTO_G20_DXE_PROGRESS_CONTROL_PROTOCOL  *This,
  OUT UINT64                               *ProtocolBitmap
  );

STATIC MOTO_G20_DXE_PROGRESS_CONTROL_PROTOCOL  mProgressControl = {
  MOTO_G20_DXE_PROGRESS_CONTROL_REVISION,
  QuiesceProgress
};

STATIC
EFI_STATUS
EFIAPI
QuiesceProgress (
  IN MOTO_G20_DXE_PROGRESS_CONTROL_PROTOCOL  *This,
  OUT UINT64                               *ProtocolBitmap
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;
  EFI_STATUS  FirstError;
  UINTN       Index;

  if ((This != &mProgressControl) || (ProtocolBitmap == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // All monitor callbacks run at TPL_CALLBACK on the BSP.  Suppress them
  // before closing events; queued or unsuccessfully closed events cannot draw.
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  mQuiesced = TRUE;
  MotoG20DxeTelemetryReleaseDisplay ();
  *ProtocolBitmap = mProtocolBitmap;
  FirstError = EFI_SUCCESS;
  for (Index = 0U; Index < ARRAY_SIZE (mEvents); Index++) {
    if (mEvents[Index] != NULL) {
      Status = gBS->CloseEvent (mEvents[Index]);
      if (!EFI_ERROR (Status)) {
        mEvents[Index] = NULL;
        mRegistrations[Index] = NULL;
      } else if (!EFI_ERROR (FirstError)) {
        FirstError = Status;
      }
    }
  }

  gBS->RestoreTPL (OldTpl);
  return FirstError;
}

STATIC
VOID
EFIAPI
ProtocolInstalled (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  CONST MOTO_G20_PROTOCOL_WATCH  *Watch;
  VOID                           *Interface;
  EFI_STATUS                     Status;

  if (mQuiesced) {
    return;
  }

  Watch = (CONST MOTO_G20_PROTOCOL_WATCH *)Context;
  if ((mProtocolBitmap & Watch->Bit) != 0U) {
    return;
  }

  Interface = NULL;
  Status    = gBS->LocateProtocol (Watch->ProtocolGuid, NULL, &Interface);
  if (!EFI_ERROR (Status) && (Interface != NULL)) {
    mProtocolBitmap |= Watch->Bit;
    MotoG20DxeTelemetryCheckpoint (
      Watch->Stage,
      EFI_SUCCESS,
      mProtocolBitmap,
      (UINT64)(UINTN)Interface
      );
  }
}

EFI_STATUS
EFIAPI
MotoG20DxeProgressEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN       Index;
  EFI_STATUS  Status;

  MotoG20DxeTelemetryCheckpoint (
    MotoG20DxeStageProgressMonitor,
    EFI_SUCCESS,
    ARRAY_SIZE (mWatches),
    0U
    );
  for (Index = 0U; Index < ARRAY_SIZE (mWatches); Index++) {
    mEvents[Index] = EfiCreateProtocolNotifyEvent (
                       mWatches[Index].ProtocolGuid,
                       TPL_CALLBACK,
                       ProtocolInstalled,
                       (VOID *)&mWatches[Index],
                       &mRegistrations[Index]
                       );
    if (mEvents[Index] == NULL) {
      MotoG20DxeTelemetryFailAndReset (
        MotoG20DxeStageFailure,
        EFI_OUT_OF_RESOURCES,
        Index,
        mProtocolBitmap
        );
      return EFI_OUT_OF_RESOURCES;
    }
  }

  // Publish only after the complete watch set exists.  BDS invokes Quiesce
  // in this module, rather than releasing a different static library copy.
  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gMotoG20DxeProgressControlProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mProgressControl
                  );
  if (EFI_ERROR (Status)) {
    MotoG20DxeTelemetryFailAndReset (
      MotoG20DxeStageFailure,
      Status,
      ARRAY_SIZE (mWatches),
      mProtocolBitmap
      );
  }

  return Status;
}

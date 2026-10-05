/** @file
  DXE Core entry point with a first-instruction observable checkpoint.
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeCoreEntryPoint.h>
#include <Library/MotoG20DxeTelemetryLib.h>

extern UINT8  gMotoG20DxePlacementPad[];

VOID  *gHobList = NULL;

VOID
EFIAPI
_ModuleEntryPoint (
  IN VOID  *HobStart
  )
{
  gHobList = HobStart;
  MotoG20DxeTelemetryCheckpoint (
    MotoG20DxeStageCoreEntry,
    EFI_SUCCESS,
    (UINT64)(UINTN)HobStart,
    (UINT64)(UINTN)gMotoG20DxePlacementPad
    );
  ProcessModuleEntryPointList (HobStart);
  MotoG20DxeTelemetryFailAndReset (
    MotoG20DxeStageCoreReturned,
    EFI_DEVICE_ERROR,
    (UINT64)(UINTN)HobStart,
    0U
    );
  CpuDeadLoop ();
}

VOID
EFIAPI
EfiMain (
  IN VOID  *HobStart
  )
{
  _ModuleEntryPoint (HobStart);
}

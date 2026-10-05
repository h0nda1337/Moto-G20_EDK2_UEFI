/* SPDX-License-Identifier: GPL-2.0-only */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "MotoG20SdPlatform.h"
#include "MotoG20SdPlatformDiagnostic.h"

STATIC struct sdp_state mState;
STATIC MOTO_G20_SD_PLATFORM_DIAGNOSTIC mDiagnostic;
STATIC EFI_GUID mPlatformGuid = MOTO_G20_SD_PLATFORM_GUID;
STATIC EFI_GUID mDiagnosticGuid = MOTO_G20_SD_PLATFORM_DIAGNOSTIC_GUID;
STATIC EFI_EVENT mExitEvent;
STATIC EFI_EVENT mDetectEvent;
STATIC BOOLEAN mExited;

STATIC EFI_STATUS
ToEfi (enum sdro_status Status)
{
  switch (Status) {
    case SDRO_OK: return EFI_SUCCESS;
    case SDRO_NO_CARD: return EFI_NO_MEDIA;
    case SDRO_TIMEOUT: return EFI_TIMEOUT;
    case SDRO_ARGUMENT: return EFI_INVALID_PARAMETER;
    case SDRO_NOT_READY: return EFI_NOT_READY;
    case SDRO_UNSUPPORTED: return EFI_UNSUPPORTED;
    default: return EFI_DEVICE_ERROR;
  }
}

STATIC VOID UpdateDiagnostic (VOID) { CopyMem (&mDiagnostic.Hardware, &mState.diag, sizeof (mState.diag)); }

STATIC int
MapRegion (void *Context, sd_u32 Base, sd_u32 Size)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
  EFI_STATUS Status;
  UINT64 Address, End, Length, DescriptorEnd;
  (VOID)Context;
  Address = Base;
  End = Address + Size;
  while (Address < End) {
    Status = gDS->GetMemorySpaceDescriptor (Address, &Desc);
    if (EFI_ERROR (Status)) { goto Error; }
    if (Desc.Length == 0 || Desc.BaseAddress > Address ||
        Desc.Length > MAX_UINT64 - Desc.BaseAddress) {
      Status = EFI_COMPROMISED_DATA; goto Error;
    }
    DescriptorEnd = Desc.BaseAddress + Desc.Length;
    if (DescriptorEnd <= Address) { Status = EFI_COMPROMISED_DATA; goto Error; }
    Length = MIN (End, DescriptorEnd) - Address;
    if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Address, Length, EFI_MEMORY_UC);
      if (EFI_ERROR (Status)) { goto Error; }
    } else if (Desc.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) {
      Status = EFI_ACCESS_DENIED; goto Error;
    }
    Status = gDS->SetMemorySpaceAttributes (Address, Length, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) { goto Error; }
    Status = gDS->GetMemorySpaceDescriptor (Address, &Desc);
    if (EFI_ERROR (Status)) { goto Error; }
    if (Desc.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo ||
        (Desc.Attributes & (EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB)) != EFI_MEMORY_UC) {
      Status = EFI_DEVICE_ERROR; goto Error;
    }
    Address += Length;
  }
  mDiagnostic.MappingStatus = EFI_SUCCESS;
  return 1;
Error:
  mDiagnostic.MappingStatus = Status;
  return 0;
}
STATIC sd_u32 ReadMmio (void *Context, sd_u32 Address) { (VOID)Context; return MmioRead32 (Address); }
STATIC void WriteMmio (void *Context, sd_u32 Address, sd_u32 Value) { (VOID)Context; MmioWrite32 (Address, Value); }
STATIC void Delay (void *Context, sd_u32 Microseconds) { (VOID)Context; MicroSecondDelay (Microseconds); }
STATIC int AccessReady (void *Context) {
  int Ready;
  if (mExited || Context != &mState) { return 0; }
  Ready = sdp_access_ready (&mState); UpdateDiagnostic (); return Ready;
}
STATIC int CardPresent (void *Context) {
  int Present;
  if (mExited || Context != &mState) { return -1; }
  Present = sdp_card_present (&mState); UpdateDiagnostic (); return Present;
}

STATIC EFI_STATUS EFIAPI Prepare (MOTO_G20_SD_PLATFORM *This, MOTO_G20_SD_CONFIGURATION *Configuration);
STATIC MOTO_G20_SD_PLATFORM mPlatform = { MOTO_G20_SD_PLATFORM_REVISION, MOTO_G20_SD_BASE, Prepare };

STATIC EFI_STATUS EFIAPI
Prepare (MOTO_G20_SD_PLATFORM *This, MOTO_G20_SD_CONFIGURATION *Configuration)
{
  struct sdp_ops Ops;
  enum sdro_status Status;
  if (This != &mPlatform || Configuration == NULL) { return EFI_INVALID_PARAMETER; }
  ZeroMem (Configuration, sizeof (*Configuration));
  if (mExited) { return EFI_NOT_READY; }
  Ops.context = NULL; Ops.map = MapRegion; Ops.read = ReadMmio; Ops.write = WriteMmio; Ops.delay = Delay;
  Status = sdp_prepare (&mState, &Ops);
  mDiagnostic.PrepareStatus = ToEfi (Status); UpdateDiagnostic ();
  if (Status != SDRO_OK) { return mDiagnostic.PrepareStatus; }
  mDiagnostic.PrepareStatus = gBS->SetTimer (mDetectEvent, TimerPeriodic, 500000);
  if (EFI_ERROR (mDiagnostic.PrepareStatus)) {
    (VOID)sdp_stop (&mState); UpdateDiagnostic (); return mDiagnostic.PrepareStatus;
  }
  Configuration->SupplyMicrovolts = 3000000;
  Configuration->SignalMicrovolts = 3000000;
  Configuration->ClockHz = 26000000 / (4 * 17);
  Configuration->BusWidth = 1;
  Configuration->Io.context = &mState;
  Configuration->Io.read32 = sdp_read32;
  Configuration->Io.write32 = sdp_write32;
  Configuration->Io.delay_us = sdp_delay_us;
  Configuration->Io.access_ready = AccessReady;
  Configuration->Io.card_present = CardPresent;
  return EFI_SUCCESS;
}
STATIC VOID EFIAPI
PollDetect (EFI_EVENT Event, VOID *Context)
{
  (VOID)Event; (VOID)Context;
  /* Only the independent EIC is sampled. No host, ADI, file access, or console
   * output occurs in this event. A sampled removal remains latched until reboot. */
  if (!mExited && mState.ready) { (VOID)sdp_card_present (&mState); UpdateDiagnostic (); }
}
STATIC VOID EFIAPI
ExitBootServices (EFI_EVENT Event, VOID *Context)
{
  (VOID)Event; (VOID)Context;
  mExited = TRUE;
  if (mState.attempted) { mDiagnostic.ExitStatus = ToEfi (sdp_stop (&mState)); }
  UpdateDiagnostic ();
}
EFI_STATUS EFIAPI
MotoG20SdPlatformEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  (VOID)SystemTable;
  mDiagnostic.Revision = 1;
  mDiagnostic.Size = sizeof (mDiagnostic);
  mDiagnostic.MappingStatus = EFI_NOT_READY;
  mDiagnostic.PrepareStatus = EFI_NOT_READY;
  mDiagnostic.ExitStatus = EFI_NOT_READY;
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices, NULL,
                             &gEfiEventExitBootServicesGuid, &mExitEvent);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, PollDetect, NULL, &mDetectEvent);
  if (EFI_ERROR (Status)) { gBS->CloseEvent (mExitEvent); return Status; }
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle,
      &mDiagnosticGuid, &mDiagnostic, &mPlatformGuid, &mPlatform, NULL);
  if (EFI_ERROR (Status)) { gBS->CloseEvent (mDetectEvent); gBS->CloseEvent (mExitEvent); }
  return Status;
}

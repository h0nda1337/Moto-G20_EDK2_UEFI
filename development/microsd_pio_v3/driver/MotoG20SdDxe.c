/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Removable SD read-only BlockIo adapter. No platform provider is fabricated:
 * without an independently implemented Prepare protocol this remains NOT_READY.
 */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include "MotoG20SdPlatform.h"
#include "MotoG20SdDiagnostic.h"

STATIC EFI_GUID mPlatformGuid = MOTO_G20_SD_PLATFORM_GUID;
STATIC EFI_GUID mDiagnosticGuid = MOTO_G20_SD_DIAGNOSTIC_GUID;
STATIC EFI_GUID mControllerGuid = {0x45766337, 0xa136, 0x4b88, {0x93,0xe6,0x16,0x21,0x67,0xe9,0x4d,0x0d}};
STATIC struct sdro_host mHost;
STATIC MOTO_G20_SD_DIAGNOSTIC mDiagnostic = {2, 0, EFI_NOT_READY};
STATIC EFI_BLOCK_IO_MEDIA mMedia;
STATIC EFI_HANDLE mController;
STATIC EFI_EVENT mNotifyEvent;
STATIC VOID *mRegistration;
STATIC BOOLEAN mAttempted;
STATIC BOOLEAN mBusy;
STATIC BOOLEAN mExited;

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH Controller;
  SD_DEVICE_PATH Slot;
  EFI_DEVICE_PATH_PROTOCOL End;
} SD_DEVICE_PATH_COMPLETE;
#pragma pack()
STATIC SD_DEVICE_PATH_COMPLETE mPath;

STATIC EFI_STATUS ToEfi(enum sdro_status S) {
  switch (S) {
    case SDRO_OK: return EFI_SUCCESS;
    case SDRO_ARGUMENT: case SDRO_RANGE: return EFI_INVALID_PARAMETER;
    case SDRO_NOT_READY: return EFI_NOT_READY;
    case SDRO_NO_CARD: return EFI_NO_MEDIA;
    case SDRO_TIMEOUT: return EFI_TIMEOUT;
    case SDRO_UNSUPPORTED: return EFI_UNSUPPORTED;
    default: return EFI_DEVICE_ERROR;
  }
}

STATIC VOID Snapshot(enum sdro_status S) {
  mDiagnostic.Status = ToEfi(S);
  mDiagnostic.CoreStatus = (UINT32)S;
  mDiagnostic.LastCommand = mHost.last_command;
  mDiagnostic.LastIrq = mHost.last_irq;
  mDiagnostic.LastResponse = mHost.last_response;
  mDiagnostic.Commands = mHost.commands;
  mDiagnostic.BlocksRead = mHost.blocks_read;
  mDiagnostic.CapacityBlocks = mHost.card.blocks;
  mDiagnostic.TransferVersion = mHost.transfer_version;
  mDiagnostic.TransferBlock = mHost.transfer_block;
  mDiagnostic.TransferCount = mHost.transfer_count;
  mDiagnostic.TransferMode = mHost.transfer_mode;
  mDiagnostic.TransferCountOffset = mHost.transfer_count_offset;
  if (mHost.faulted) {
    mDiagnostic.Stage = 4;
    if (mMedia.MediaPresent) {
      mMedia.MediaPresent = FALSE;
      mMedia.MediaId++;
    }
  }
}

STATIC EFI_STATUS EFIAPI Reset(EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Extended) {
  if (This == NULL) return EFI_INVALID_PARAMETER;
  if (mExited || !mMedia.MediaPresent) return EFI_NO_MEDIA;
  /* Resetting/re-enumerating live removable media requires platform lifecycle
   * handling; never claim that ExtendedVerification actually happened. */
  return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI Read(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  UINTN Index, Count;
  EFI_TPL OldTpl;
  enum sdro_status Result = SDRO_OK;
  if (This == NULL) return EFI_INVALID_PARAMETER;
  if (mExited || !mMedia.MediaPresent) return EFI_NO_MEDIA;
  if (MediaId != mMedia.MediaId) return EFI_MEDIA_CHANGED;
  if (Size == 0) return EFI_SUCCESS;
  if (Buffer == NULL) return EFI_INVALID_PARAMETER;
  if (Size % 512 != 0) return EFI_BAD_BUFFER_SIZE;
  Count = Size / 512;
  if (Lba > mMedia.LastBlock || (UINT64)(Count - 1) > mMedia.LastBlock - Lba) return EFI_INVALID_PARAMETER;
  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  if (mBusy) { gBS->RestoreTPL(OldTpl); return EFI_NOT_READY; }
  mBusy = TRUE;
  gBS->RestoreTPL(OldTpl);
  for (Index = 0; Index < Count; ++Index) {
    Result = sdro_read_block(&mHost, Lba + Index, (sd_u8 *)Buffer + Index * 512, 512);
    if (Result != SDRO_OK) break;
  }
  Snapshot(Result);
  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  mBusy = FALSE;
  gBS->RestoreTPL(OldTpl);
  return ToEfi(Result);
}
STATIC EFI_STATUS EFIAPI Write(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  return EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI Flush(EFI_BLOCK_IO_PROTOCOL *This) {
  return mExited ? EFI_NOT_READY : EFI_SUCCESS; /* no write cache exists */
}
STATIC EFI_BLOCK_IO_PROTOCOL mBlockIo = { EFI_BLOCK_IO_PROTOCOL_REVISION, &mMedia, Reset, Read, Write, Flush };

STATIC VOID EFIAPI OnExit(EFI_EVENT Event, VOID *Context) {
  mExited = TRUE; /* provider owns hardware quiescence, driver never issues MMIO here */
  mMedia.MediaPresent = FALSE;
}

STATIC VOID EFIAPI TryStart(EFI_EVENT Event, VOID *Context) {
  MOTO_G20_SD_PLATFORM *Platform = NULL;
  MOTO_G20_SD_CONFIGURATION Config;
  EFI_STATUS Status;
  enum sdro_status Result;
  if (mAttempted || mExited) return;
  Status = gBS->LocateProtocol(&mPlatformGuid, NULL, (VOID **)&Platform);
  if (EFI_ERROR(Status)) return;
  mAttempted = TRUE;
  if (Platform == NULL || Platform->Revision != MOTO_G20_SD_PLATFORM_REVISION ||
      Platform->ControllerBase != MOTO_G20_SD_BASE || Platform->Prepare == NULL) {
    mDiagnostic.Status = EFI_UNSUPPORTED;
    return;
  }
  mDiagnostic.Stage = 1;
  ZeroMem(&Config, sizeof(Config));
  Status = Platform->Prepare(Platform, &Config);
  mDiagnostic.Status = Status;
  if (EFI_ERROR(Status)) return;
  if (Config.SupplyMicrovolts != 3000000 || Config.SignalMicrovolts != 3000000 ||
      Config.BusWidth != 1 || Config.ClockHz == 0 || Config.ClockHz > 400000) {
    mDiagnostic.Status = EFI_UNSUPPORTED;
    return;
  }
  Result = sdro_init(&mHost, &Config.Io, Config.ClockHz, 0x40000);
  if (Result != SDRO_OK) { Snapshot(Result); return; }
  mDiagnostic.Stage = 2;
  Result = sdro_identify(&mHost);
  Snapshot(Result);
  if (Result != SDRO_OK) return;
  ZeroMem(&mMedia, sizeof(mMedia));
  mMedia.MediaId = 1;
  mMedia.RemovableMedia = TRUE;
  mMedia.MediaPresent = TRUE;
  mMedia.ReadOnly = TRUE;
  mMedia.BlockSize = 512;
  mMedia.IoAlign = 1;
  mMedia.LastBlock = mHost.card.blocks - 1;
  ZeroMem(&mPath, sizeof(mPath));
  mPath.Controller.Header.Type = HARDWARE_DEVICE_PATH;
  mPath.Controller.Header.SubType = HW_VENDOR_DP;
  mPath.Controller.Header.Length[0] = sizeof(VENDOR_DEVICE_PATH);
  CopyGuid(&mPath.Controller.Guid, &mControllerGuid);
  mPath.Slot.Header.Type = MESSAGING_DEVICE_PATH;
  mPath.Slot.Header.SubType = MSG_SD_DP;
  mPath.Slot.Header.Length[0] = sizeof(SD_DEVICE_PATH);
  mPath.Slot.SlotNumber = 0;
  mPath.End.Type = END_DEVICE_PATH_TYPE;
  mPath.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  mPath.End.Length[0] = sizeof(EFI_DEVICE_PATH_PROTOCOL);
  Status = gBS->InstallMultipleProtocolInterfaces(&mController,
    &gEfiBlockIoProtocolGuid, &mBlockIo, &gEfiDevicePathProtocolGuid, &mPath, NULL);
  mDiagnostic.Status = Status;
  if (!EFI_ERROR(Status)) {
    mDiagnostic.Stage = 3;
    mDiagnostic.Published = TRUE;
  }
}

EFI_STATUS EFIAPI MotoG20SdEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
  EFI_HANDLE DiagnosticHandle = NULL;
  EFI_EVENT ExitEvent;
  Status = gBS->InstallMultipleProtocolInterfaces(&DiagnosticHandle, &mDiagnosticGuid, &mDiagnostic, NULL);
  if (EFI_ERROR(Status)) return Status;
  /* Keep the image resident after publishing diagnostics, including failures. */
  Status = gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, OnExit, NULL, &ExitEvent);
  if (EFI_ERROR(Status)) { mDiagnostic.Status = Status; return EFI_SUCCESS; }
  Status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TryStart, NULL, &mNotifyEvent);
  if (EFI_ERROR(Status)) { mDiagnostic.Status = Status; return EFI_SUCCESS; }
  Status = gBS->RegisterProtocolNotify(&mPlatformGuid, mNotifyEvent, &mRegistration);
  if (EFI_ERROR(Status)) {
    gBS->CloseEvent(mNotifyEvent);
    mDiagnostic.Status = Status;
    return EFI_SUCCESS;
  }
  TryStart(NULL, NULL);
  return EFI_SUCCESS;
}

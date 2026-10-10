/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * BDS microSD read proof. This never starts an image or changes a file.
 */
#include "MotoG20SdProbe.h"
#include "MotoG20SdDiagnostic.h"
#include "MotoG20SdPlatformDiagnostic.h"
#include "MotoG20RawSdProbe.h"
#include "MotoG20StorageConnect.h"
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC EFI_GUID mDiagnosticGuid = MOTO_G20_SD_DIAGNOSTIC_GUID;
STATIC EFI_GUID mPlatformDiagnosticGuid = MOTO_G20_SD_PLATFORM_DIAGNOSTIC_GUID;
STATIC EFI_GUID mControllerGuid = {
  0x45766337, 0xa136, 0x4b88, {0x93, 0xe6, 0x16, 0x21, 0x67, 0xe9, 0x4d, 0x0d}
};
STATIC CONST CHAR8 mExpected[] = "MOTO G20 MICROSD READ TEST V1\r\n";
STATIC BOOLEAN mProbed;

STATIC VOID
PrintPlatformDiagnostic (VOID)
{
  MOTO_G20_SD_PLATFORM_DIAGNOSTIC *Diagnostic;
  struct sdp_diag *Hw;
  EFI_STATUS Status;

  Diagnostic = NULL;
  Status = gBS->LocateProtocol (&mPlatformDiagnosticGuid, NULL, (VOID **)&Diagnostic);
  if (EFI_ERROR (Status) || Diagnostic == NULL) {
    Print (L"SDHW: diagnostic unavailable (%r).\r\n", Status);
    return;
  }
  if (Diagnostic->Revision != 1 || Diagnostic->Size != sizeof (*Diagnostic)) {
    Print (L"SDHW: diagnostic layout unsupported.\r\n");
    return;
  }
  Hw = &Diagnostic->Hardware;
  Print (L"SDHW: stage=%u map=%r prepare=%r ready=%u\r\n",
         Hw->stage, Diagnostic->MappingStatus, Diagnostic->PrepareStatus, Hw->ready);
  Print (L"SDHW: reg=%08x expected=%08x actual=%08x cleanup=%u\r\n",
         Hw->last_address, Hw->expected, Hw->actual, Hw->cleanup_status);
  Print (L"SDHW: CD=%08x mux=%08x clock=%08x timeout=%u\r\n",
         Hw->detect, Hw->mux, Hw->clock, Hw->timeout);
  Print (L"SDHW: rails=%04x/%04x/%04x/%04x elapsed-us=%u\r\n",
         Hw->rail_new[0], Hw->rail_new[1], Hw->rail_new[2], Hw->rail_new[3], Hw->elapsed_us);
  Print (L"SDHW: version=%08x caps=%08x present=%08x DLL=%08x\r\n",
         Hw->host_version, Hw->host_capabilities, Hw->host_present, Hw->dll);
  Print (L"SDHW: inherited pins CMD/D0/D1/CLK/D2/D3=%x/%x/%x/%x/%x/%x\r\n",
         Hw->pin_common[0], Hw->pin_common[1], Hw->pin_common[2],
         Hw->pin_common[3], Hw->pin_common[4], Hw->pin_common[5]);
}

/* A partition node may follow the SD node. No other controller, slot or path
 * instance is accepted, even if it exposes a file with the expected name.
 */
STATIC BOOLEAN
IsMicroSdPath (
  IN EFI_DEVICE_PATH_PROTOCOL *Path
  )
{
  VENDOR_DEVICE_PATH *Vendor;
  SD_DEVICE_PATH *Slot;

  if (Path == NULL || !IsDevicePathValid (Path, 1024)) {
    return FALSE;
  }
  if (DevicePathType (Path) != HARDWARE_DEVICE_PATH ||
      DevicePathSubType (Path) != HW_VENDOR_DP ||
      DevicePathNodeLength (Path) != sizeof (VENDOR_DEVICE_PATH)) {
    return FALSE;
  }
  Vendor = (VENDOR_DEVICE_PATH *)Path;
  if (CompareMem (&Vendor->Guid, &mControllerGuid, sizeof (EFI_GUID)) != 0) {
    return FALSE;
  }
  Path = NextDevicePathNode (Path);
  if (DevicePathType (Path) != MESSAGING_DEVICE_PATH ||
      DevicePathSubType (Path) != MSG_SD_DP ||
      DevicePathNodeLength (Path) != sizeof (SD_DEVICE_PATH)) {
    return FALSE;
  }
  Slot = (SD_DEVICE_PATH *)Path;
  if (Slot->SlotNumber != 0) {
    return FALSE;
  }
  while (!IsDevicePathEnd (Path)) {
    if (IsDevicePathEndInstance (Path)) {
      return FALSE;
    }
    Path = NextDevicePathNode (Path);
  }
  return TRUE;
}

STATIC EFI_STATUS
CheckReadTest (
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem,
  OUT BOOLEAN *Matches,
  OUT UINTN *BytesRead
  )
{
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;
  EFI_STATUS CloseStatus;
  UINT8 Data[128];
  UINTN Count;
  BOOLEAN EndOfFile;

  *Matches = FALSE;
  *BytesRead = 0;
  Root = NULL;
  File = NULL;
  EndOfFile = FALSE;
  Status = FileSystem->OpenVolume (FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Root == NULL) {
    return EFI_DEVICE_ERROR;
  }
  Status = Root->Open (Root, &File, L"\\G20SD.TXT", EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    if (File == NULL) {
      Status = EFI_DEVICE_ERROR;
    } else {
      /* A second/partial read is intentional: PASS requires observed EOF,
       * not merely a matching prefix or a file-size claim. At most 128 bytes
       * are read in total; a full buffer without EOF fails the check.
       */
      while (*BytesRead < sizeof (Data)) {
        Count = sizeof (Data) - *BytesRead;
        Status = File->Read (File, &Count, Data + *BytesRead);
        if (EFI_ERROR (Status)) {
          break;
        }
        if (Count > sizeof (Data) - *BytesRead) {
          Status = EFI_DEVICE_ERROR;
          break;
        }
        if (Count == 0) {
          EndOfFile = TRUE;
          break;
        }
        *BytesRead += Count;
      }
      CloseStatus = File->Close (File);
      if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) {
        Status = CloseStatus;
      }
    }
  }
  CloseStatus = Root->Close (Root);
  if (!EFI_ERROR (Status) && EFI_ERROR (CloseStatus)) {
    Status = CloseStatus;
  }
  if (!EFI_ERROR (Status)) {
    *Matches = EndOfFile && *BytesRead == sizeof (mExpected) - 1 &&
               CompareMem (Data, mExpected, sizeof (mExpected) - 1) == 0;
  }
  return Status;
}

VOID
MotoG20SdProbe (
  VOID
  )
{
  MOTO_G20_SD_DIAGNOSTIC *Diagnostic;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_HANDLE *Handles;
  EFI_STATUS Status;
  UINTN HandleCount;
  UINTN Index;
  UINTN MatchingVolumes;
  UINTN BytesRead;
  BOOLEAN Matches;
  BOOLEAN Passed;
  BOOLEAN RawPassed;

  if (mProbed) {
    return;
  }
  mProbed = TRUE;
  PrintPlatformDiagnostic ();
  Diagnostic = NULL;
  Status = gBS->LocateProtocol (&mDiagnosticGuid, NULL, (VOID **)&Diagnostic);
  if (EFI_ERROR (Status) || Diagnostic == NULL) {
    Print (L"MICROSD: diagnostic unavailable (%r); read test not run.\r\n", Status);
    return;
  }
  if (Diagnostic->Revision != 3) {
    Print (L"MICROSD: diagnostic revision unsupported; read test not run.\r\n");
    return;
  }
  Print (L"MICROSD: stage=%u status=%r published=%u\r\n",
         Diagnostic->Stage, Diagnostic->Status, (UINT32)Diagnostic->Published);
  Print (L"MICROSD: cmd=%u irq=%08x response=%08x commands=%u blocks=%u capacity=%Lu\r\n",
         Diagnostic->LastCommand, Diagnostic->LastIrq, Diagnostic->LastResponse,
         Diagnostic->Commands, Diagnostic->BlocksRead, Diagnostic->CapacityBlocks);
  Print (L"SDMA: allocation=%r address=%08x\r\n", Diagnostic->DmaAllocationStatus,
         Diagnostic->DmaAddress);
  if (!Diagnostic->Published) {
    Print (L"MICROSD: BlockIo not published; read test not run.\r\n");
    return;
  }

  RawPassed = MotoG20RawSdProbe ();
  Print (L"SDCMD17: setup version=%08x block=%08x count-reg=%02x count=%u mode=%04x\r\n",
         Diagnostic->TransferVersion, Diagnostic->TransferBlock,
         Diagnostic->TransferCountOffset, Diagnostic->TransferCount, Diagnostic->TransferMode);
  Print (L"SDMA: tries=%u complete=%u guards=%u fault=%u\r\n",
         Diagnostic->DmaAttempts, Diagnostic->DmaCompletions,
         Diagnostic->DmaGuardsOk, Diagnostic->DmaFaulted);
  Print (L"SDREG: valid=%04x phase=%u IRQ=%08x IE=%08x PRESENT=%08x\r\n",
         Diagnostic->SnapshotValid, Diagnostic->SnapshotPhase, Diagnostic->SnapshotIrq,
         Diagnostic->SnapshotIrqEnable, Diagnostic->SnapshotPresent);
  Print (L"SDREG: HC1=%08x HC2=%04x CMD=%08x R1=%08x\r\n",
         Diagnostic->SnapshotHostControl1, Diagnostic->SnapshotHostControl2,
         Diagnostic->SnapshotCommand, Diagnostic->SnapshotResponse);
  Print (L"SDREG: count=%u DMA=%08x:%08x\r\n", Diagnostic->SnapshotCount,
         Diagnostic->SnapshotDmaHigh, Diagnostic->SnapshotDmaLow);
  if (!RawPassed) {
    Print (L"MICROSD: exact sector proof failed; filesystem test not run.\r\n");
    return;
  }
  Status = MotoG20StorageConnect ();
  Print (L"MICROSD: targeted storage connect=%r; file read still unverified.\r\n", Status);
  Print (L"MICROSD: after connect stage=%u status=%r cmd=%u irq=%08x blocks=%u\r\n",
         Diagnostic->Stage, Diagnostic->Status, Diagnostic->LastCommand,
         Diagnostic->LastIrq, Diagnostic->BlocksRead);
  Handles = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                  NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    if (Handles != NULL) FreePool (Handles);
    Print (L"MICROSD: no filesystem (%r); read test not run.\r\n", Status);
    return;
  }
  if (Handles == NULL || HandleCount == 0 || HandleCount > 64) {
    if (Handles != NULL) FreePool (Handles);
    Print (L"MICROSD: invalid/bounded filesystem inventory; read test not run.\r\n");
    return;
  }
  MatchingVolumes = 0;
  Passed = FALSE;
  for (Index = 0; Index < HandleCount; ++Index) {
    Path = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path);
    if (EFI_ERROR (Status) || !IsMicroSdPath (Path)) {
      continue;
    }
    MatchingVolumes++;
    FileSystem = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid,
                                 (VOID **)&FileSystem);
    if (EFI_ERROR (Status) || FileSystem == NULL) {
      Print (L"MICROSD: volume %Lu unavailable (%r).\r\n", (UINT64)MatchingVolumes, Status);
      continue;
    }
    Status = CheckReadTest (FileSystem, &Matches, &BytesRead);
    if (EFI_ERROR (Status)) {
      Print (L"MICROSD: G20SD.TXT read failed (%r).\r\n", Status);
    } else if (!Matches) {
      Print (L"MICROSD: G20SD.TXT mismatch/incomplete (%Lu bytes).\r\n", (UINT64)BytesRead);
    } else {
      Print (L"MICROSD: PASS G20SD.TXT (%Lu bytes, exact content and EOF).\r\n", (UINT64)BytesRead);
      Passed = TRUE;
      break;
    }
  }
  FreePool (Handles);
  if (!Passed) {
    Print (L"MICROSD: read test not verified; matching volumes=%Lu.\r\n", (UINT64)MatchingVolumes);
  }
}

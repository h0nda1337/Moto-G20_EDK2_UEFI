/* Read-only physical SD smoke test, independent of Partition/FAT drivers. */
#include "MotoG20RawSdProbe.h"
#include "MotoG20SdSectorReferences.h"
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC EFI_GUID mControllerGuid = {0x45766337,0xa136,0x4b88,{0x93,0xe6,0x16,0x21,0x67,0xe9,0x4d,0x0d}};

STATIC BOOLEAN IsPhysicalSdPath (EFI_DEVICE_PATH_PROTOCOL *Path) {
  VENDOR_DEVICE_PATH *Vendor;
  SD_DEVICE_PATH *Slot;
  if (Path == NULL || !IsDevicePathValid (Path, 1024)) return FALSE;
  if (DevicePathType (Path) != HARDWARE_DEVICE_PATH || DevicePathSubType (Path) != HW_VENDOR_DP ||
      DevicePathNodeLength (Path) != sizeof (*Vendor)) return FALSE;
  Vendor = (VENDOR_DEVICE_PATH *)Path;
  if (CompareMem (&Vendor->Guid, &mControllerGuid, sizeof (EFI_GUID)) != 0) return FALSE;
  Path = NextDevicePathNode (Path);
  if (DevicePathType (Path) != MESSAGING_DEVICE_PATH || DevicePathSubType (Path) != MSG_SD_DP ||
      DevicePathNodeLength (Path) != sizeof (*Slot)) return FALSE;
  Slot = (SD_DEVICE_PATH *)Path;
  Path = NextDevicePathNode (Path);
  return Slot->SlotNumber == 0 && IsDevicePathEnd (Path) && !IsDevicePathEndInstance (Path);
}

VOID MotoG20RawSdProbe (VOID) {
  EFI_HANDLE *Handles;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_STATUS Status;
  UINTN Count, Index, Test;
  UINT64 Buffer[64];
  CONST UINT64 Lbas[] = {0, 2048, 99072};
  UINT32 MediaId;

  Handles = NULL;
  Count = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"SDRAW: BlockIo handles unavailable (%r).\r\n", Status);
    return;
  }
  for (Index = 0; Index < Count; Index++) {
    Path = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path);
    if (EFI_ERROR (Status) || !IsPhysicalSdPath (Path)) continue;
    Block = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Block);
    if (EFI_ERROR (Status) || Block == NULL || Block->Media == NULL) continue;
    if (!Block->Media->MediaPresent || Block->Media->LogicalPartition ||
        !Block->Media->ReadOnly || Block->Media->BlockSize != 512 ||
        Block->Media->LastBlock != 60456959 || Block->Media->IoAlign > 8) {
      Print (L"SDRAW: unexpected media parameters; no raw read.\r\n");
      break;
    }
    MediaId = Block->Media->MediaId;
    for (Test = 0; Test < 3; Test++) {
      ZeroMem (Buffer, sizeof (Buffer));
      Status = Block->ReadBlocks (Block, MediaId, Lbas[Test], sizeof (Buffer), Buffer);
      if (EFI_ERROR (Status)) {
        Print (L"SDRAW: LBA=%Lu read=%r.\r\n", Lbas[Test], Status);
        break;
      }
      if (CompareMem (Buffer, mSdSectorReferences[Test], sizeof (Buffer)) != 0) {
        Print (L"SDRAW: LBA=%Lu data differs from Android reference.\r\n", Lbas[Test]);
        break;
      }
      Print (L"SDRAW: PASS LBA=%Lu 512 bytes exact.\r\n", Lbas[Test]);
    }
    break;
  }
  if (Index == Count) Print (L"SDRAW: physical SD0 handle not found.\r\n");
  FreePool (Handles);
}

/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Storage-chain inventory and explicit connection; separate from frozen V1.
 */
#include "MotoG20StorageConnect.h"
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/UnicodeCollation.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define STORAGE_MAX_IMAGES 128U
#define STORAGE_MAX_BINDINGS 128U
#define STORAGE_MAX_BLOCK_HANDLES 64U
#define STORAGE_MAX_CHILDREN 8U

typedef struct {
  EFI_GUID File;
  CONST CHAR16 *Name;
  EFI_HANDLE Image;
  UINTN Images;
  UINTN Bindings;
} STORAGE_DRIVER;

STATIC CONST EFI_GUID mSdController = {
  0x45766337, 0xa136, 0x4b88, {0x93,0xe6,0x16,0x21,0x67,0xe9,0x4d,0x0d}
};
STATIC BOOLEAN mStorageAttempted;

/* 1 = exact physical vendor/SD0/end, 2 = vendor/SD0/HD/end.
 * No instance, extra hardware, file node or nested partition is admitted. */
STATIC UINTN
StoragePathKind (CONST EFI_DEVICE_PATH_PROTOCOL *Path)
{
  CONST VENDOR_DEVICE_PATH *Vendor;
  CONST SD_DEVICE_PATH *Sd;
  CONST HARDDRIVE_DEVICE_PATH *Hd;
  UINTN Kind;
  if (Path == NULL || !IsDevicePathValid (Path, 1024)) return 0;
  if (DevicePathType (Path) != HARDWARE_DEVICE_PATH || DevicePathSubType (Path) != HW_VENDOR_DP ||
      DevicePathNodeLength (Path) != sizeof (VENDOR_DEVICE_PATH)) return 0;
  Vendor = (CONST VENDOR_DEVICE_PATH *)Path;
  if (CompareMem (&Vendor->Guid, &mSdController, sizeof (EFI_GUID)) != 0) return 0;
  Path = NextDevicePathNode (Path);
  if (DevicePathType (Path) != MESSAGING_DEVICE_PATH || DevicePathSubType (Path) != MSG_SD_DP ||
      DevicePathNodeLength (Path) != sizeof (SD_DEVICE_PATH)) return 0;
  Sd = (CONST SD_DEVICE_PATH *)Path;
  if (Sd->SlotNumber != 0) return 0;
  Path = NextDevicePathNode (Path);
  Kind = 1;
  if (DevicePathType (Path) == MEDIA_DEVICE_PATH && DevicePathSubType (Path) == MEDIA_HARDDRIVE_DP) {
    if (DevicePathNodeLength (Path) != sizeof (HARDDRIVE_DEVICE_PATH)) return 0;
    Hd = (CONST HARDDRIVE_DEVICE_PATH *)Path;
    if (Hd->PartitionNumber == 0 || Hd->PartitionSize == 0) return 0;
    Kind = 2;
    Path = NextDevicePathNode (Path);
  }
  return IsDevicePathEnd (Path) && DevicePathNodeLength (Path) == sizeof (*Path) ? Kind : 0;
}

STATIC BOOLEAN
StorageImageMatches (CONST EFI_DEVICE_PATH_PROTOCOL *Path, CONST EFI_GUID *File)
{
  CONST EFI_DEVICE_PATH_PROTOCOL *Last;
  CONST MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *FvFile;
  if (Path == NULL || !IsDevicePathValid (Path, 1024)) return FALSE;
  Last = NULL;
  while (!IsDevicePathEnd (Path)) {
    if (DevicePathType (Path) == END_DEVICE_PATH_TYPE) return FALSE;
    Last = Path;
    Path = NextDevicePathNode (Path);
  }
  if (Last == NULL || DevicePathType (Last) != MEDIA_DEVICE_PATH ||
      DevicePathSubType (Last) != MEDIA_PIWG_FW_FILE_DP ||
      DevicePathNodeLength (Last) != sizeof (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH)) return FALSE;
  FvFile = (CONST MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)Last;
  return CompareMem (&FvFile->FvFileName, File, sizeof (EFI_GUID)) == 0;
}

STATIC BOOLEAN
StorageHasProtocol (EFI_HANDLE Handle, EFI_GUID *Guid)
{
  VOID *Interface = NULL;
  EFI_STATUS Status = gBS->HandleProtocol (Handle, Guid, &Interface);
  return !EFI_ERROR (Status) && Interface != NULL;
}

STATIC EFI_STATUS
StorageDrivers (STORAGE_DRIVER *Drivers)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_DRIVER_BINDING_PROTOCOL *Binding;
  UINTN Count, Index, Driver;
  BOOLEAN Complete = TRUE;
  Handles = NULL;
  Count = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Count, &Handles);
  Print (L"SDCHAIN: LoadedImage locate=%r count=%Lu\r\n", Status, (UINT64)Count);
  if (EFI_ERROR (Status) || Handles == NULL || Count > STORAGE_MAX_IMAGES) {
    if (Handles != NULL) FreePool (Handles);
    return EFI_ERROR (Status) ? Status : EFI_BAD_BUFFER_SIZE;
  }
  for (Index = 0; Index < Count; ++Index) {
    Loaded = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
    if (EFI_ERROR (Status) || Loaded == NULL) continue;
    for (Driver = 0; Driver < 3; ++Driver) {
      if (StorageImageMatches (Loaded->FilePath, &Drivers[Driver].File)) {
        Drivers[Driver].Images++;
        Drivers[Driver].Image = Handles[Index];
      }
    }
  }
  FreePool (Handles);
  Handles = NULL;
  Count = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiDriverBindingProtocolGuid, NULL, &Count, &Handles);
  Print (L"SDCHAIN: DriverBinding locate=%r count=%Lu\r\n", Status, (UINT64)Count);
  if (EFI_ERROR (Status) || Handles == NULL || Count > STORAGE_MAX_BINDINGS) {
    if (Handles != NULL) FreePool (Handles);
    return EFI_ERROR (Status) ? Status : EFI_BAD_BUFFER_SIZE;
  }
  for (Index = 0; Index < Count; ++Index) {
    Binding = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiDriverBindingProtocolGuid, (VOID **)&Binding);
    if (EFI_ERROR (Status) || Binding == NULL) continue;
    for (Driver = 0; Driver < 3; ++Driver) {
      if (Drivers[Driver].Images == 1 && Binding->ImageHandle == Drivers[Driver].Image &&
          Binding->DriverBindingHandle == Handles[Index]) Drivers[Driver].Bindings++;
    }
  }
  FreePool (Handles);
  for (Driver = 0; Driver < 3; ++Driver) {
    Print (L"SDCHAIN: %s images=%Lu bindings=%Lu\r\n", Drivers[Driver].Name,
           (UINT64)Drivers[Driver].Images, (UINT64)Drivers[Driver].Bindings);
    if (Drivers[Driver].Images != 1 || Drivers[Driver].Bindings != 1) Complete = FALSE;
  }
  if (!Complete) {
    Print (L"SDCHAIN: missing/ambiguous binding; no image load or dispatch bypass.\r\n");
    return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}

/* Refresh after each connection phase because PartitionDxe can create handles.
 * The returned child list contains only validated paths with partition media. */
STATIC EFI_STATUS
StorageInventory (CONST CHAR16 *Phase, EFI_HANDLE *Root, EFI_HANDLE *Children,
                  UINTN *ChildCount, UINTN *FileSystems)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  UINTN Count, Index, Kind, Roots, Seen;
  BOOLEAN Disk, Fs;
  *Root = NULL;
  *ChildCount = 0;
  *FileSystems = 0;
  Roots = 0;
  Seen = 0;
  Handles = NULL;
  Count = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  Print (L"SDCHAIN: %s BlockIo locate=%r count=%Lu\r\n", Phase, Status, (UINT64)Count);
  if (EFI_ERROR (Status) || Handles == NULL || Count > STORAGE_MAX_BLOCK_HANDLES) {
    if (Handles != NULL) FreePool (Handles);
    return EFI_ERROR (Status) ? Status : EFI_BAD_BUFFER_SIZE;
  }
  for (Index = 0; Index < Count; ++Index) {
    Path = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Path);
    if (EFI_ERROR (Status)) continue;
    Kind = StoragePathKind (Path);
    if (Kind == 0) continue;
    Block = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Block);
    if (EFI_ERROR (Status) || Block == NULL || Block->Media == NULL) continue;
    if ((Kind == 1 && Block->Media->LogicalPartition) || (Kind == 2 && !Block->Media->LogicalPartition)) continue;
    if (++Seen > STORAGE_MAX_CHILDREN + 1) {
      FreePool (Handles);
      return EFI_BAD_BUFFER_SIZE;
    }
    Disk = StorageHasProtocol (Handles[Index], &gEfiDiskIoProtocolGuid);
    Fs = StorageHasProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid);
    Print (L"SDCHAIN: %s kind=%u media=%u id=%u block=%u last=%Lu DiskIo=%u SFS=%u\r\n",
           Phase, (UINT32)Kind, (UINT32)Block->Media->MediaPresent, Block->Media->MediaId,
           Block->Media->BlockSize, (UINT64)Block->Media->LastBlock, (UINT32)Disk, (UINT32)Fs);
    if (Kind == 1) {
      Roots++;
      *Root = Handles[Index];
    } else {
      Children[*ChildCount] = Handles[Index];
      (*ChildCount)++;
      if (Fs && Block->Media->MediaPresent) (*FileSystems)++;
    }
  }
  FreePool (Handles);
  if (Roots != 1) {
    Print (L"SDCHAIN: physical SD roots=%Lu; connection skipped.\r\n", (UINT64)Roots);
    return Roots == 0 ? EFI_NOT_FOUND : EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
MotoG20StorageConnect (VOID)
{
  STORAGE_DRIVER Drivers[3] = {
    {{0x6b38f7b4,0xad98,0x40e9,{0x90,0x93,0xac,0xa2,0xb5,0xa2,0x53,0xc4}}, L"DiskIo", NULL,0,0},
    {{0x1fa1f39e,0xfeff,0x4aae,{0xbd,0x7b,0x38,0xa0,0x70,0xa3,0xb6,0x09}}, L"Partition", NULL,0,0},
    {{0x961578fe,0xb6b7,0x44c3,{0xaf,0x35,0x6b,0xc7,0x05,0xcd,0x2b,0x1f}}, L"Fat", NULL,0,0}
  };
  EFI_STATUS Status, DriversStatus;
  EFI_HANDLE Root, AfterRoot, Children[STORAGE_MAX_CHILDREN], Priorities[4];
  UINTN ChildCount, FileSystems, Index;
  VOID *Collation;
  if (mStorageAttempted) return EFI_ALREADY_STARTED;
  mStorageAttempted = TRUE;
  DriversStatus = StorageDrivers (Drivers);
  Collation = NULL;
  Status = gBS->LocateProtocol (&gEfiUnicodeCollation2ProtocolGuid, NULL, &Collation);
  Print (L"SDCHAIN: UnicodeCollation2=%r present=%u\r\n", Status, (UINT32)(!EFI_ERROR (Status) && Collation != NULL));
  Status = StorageInventory (L"before", &Root, Children, &ChildCount, &FileSystems);
  if (EFI_ERROR (Status)) return Status;
  if (EFI_ERROR (DriversStatus)) return DriversStatus;
  Priorities[0] = Drivers[0].Image;
  Priorities[1] = Drivers[1].Image;
  Priorities[2] = Drivers[2].Image;
  Priorities[3] = NULL;
  /* These are priorities, not an exclusive allowlist. DXE may try remaining
   * resident bindings. FALSE avoids recursively touching unrelated handles. */
  Status = gBS->ConnectController (Root, Priorities, NULL, FALSE);
  Print (L"SDCHAIN: physical ConnectController=%r (DiskIo/Partition/Fat priority)\r\n", Status);
  Status = StorageInventory (L"after-root", &AfterRoot, Children, &ChildCount, &FileSystems);
  if (EFI_ERROR (Status)) return Status;
  if (Root != AfterRoot) return EFI_MEDIA_CHANGED;
  Priorities[1] = Drivers[2].Image;
  Priorities[2] = NULL;
  for (Index = 0; Index < ChildCount; ++Index) {
    Status = gBS->ConnectController (Children[Index], Priorities, NULL, FALSE);
    Print (L"SDCHAIN: partition %Lu ConnectController=%r (DiskIo/Fat priority)\r\n", (UINT64)Index + 1, Status);
  }
  Status = StorageInventory (L"after-children", &AfterRoot, Children, &ChildCount, &FileSystems);
  if (EFI_ERROR (Status)) return Status;
  if (Root != AfterRoot) return EFI_MEDIA_CHANGED;
  Print (L"SDCHAIN: SD partitions=%Lu filesystems=%Lu; file read not established.\r\n", (UINT64)ChildCount, (UINT64)FileSystems);
  return FileSystems > 0 ? EFI_SUCCESS : EFI_NOT_FOUND;
}

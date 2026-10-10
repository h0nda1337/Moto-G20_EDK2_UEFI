/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Minimal host surface, not the firmware ABI. Production C is compiled below.
 */
#ifndef STORAGE_HOST_UEFI_MOCK_H
#define STORAGE_HOST_UEFI_MOCK_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define STATIC static
#define CONST const
#define VOID void
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef ptrdiff_t INTN;
typedef uint16_t CHAR16;
typedef bool BOOLEAN;
typedef UINT64 EFI_STATUS;
typedef VOID *EFI_HANDLE;
#define TRUE true
#define FALSE false
#define EFI_SUCCESS UINT64_C(0)
#define EFI_ERROR_BIT UINT64_C(0x8000000000000000)
#define EFI_UNSUPPORTED (EFI_ERROR_BIT | UINT64_C(3))
#define EFI_BAD_BUFFER_SIZE (EFI_ERROR_BIT | UINT64_C(4))
#define EFI_NOT_READY (EFI_ERROR_BIT | UINT64_C(6))
#define EFI_DEVICE_ERROR (EFI_ERROR_BIT | UINT64_C(7))
#define EFI_NO_MEDIA (EFI_ERROR_BIT | UINT64_C(12))
#define EFI_MEDIA_CHANGED (EFI_ERROR_BIT | UINT64_C(13))
#define EFI_NOT_FOUND (EFI_ERROR_BIT | UINT64_C(14))
#define EFI_ALREADY_STARTED (EFI_ERROR_BIT | UINT64_C(20))
#define EFI_ERROR(Status) (((Status) & EFI_ERROR_BIT) != 0)
typedef struct { UINT32 Data1; UINT16 Data2, Data3; UINT8 Data4[8]; } EFI_GUID;
#pragma pack(push, 1)
typedef struct { UINT8 Type, SubType, Length[2]; } EFI_DEVICE_PATH_PROTOCOL;
typedef struct { EFI_DEVICE_PATH_PROTOCOL Header; EFI_GUID Guid; } VENDOR_DEVICE_PATH;
typedef struct { EFI_DEVICE_PATH_PROTOCOL Header; UINT8 SlotNumber; } SD_DEVICE_PATH;
typedef struct {
  EFI_DEVICE_PATH_PROTOCOL Header;
  UINT32 PartitionNumber;
  UINT64 PartitionStart, PartitionSize;
  UINT8 Signature[16], MBRType, SignatureType;
} HARDDRIVE_DEVICE_PATH;
typedef struct { EFI_DEVICE_PATH_PROTOCOL Header; EFI_GUID FvFileName; } MEDIA_FW_VOL_FILEPATH_DEVICE_PATH;
#pragma pack(pop)
#define HARDWARE_DEVICE_PATH 1u
#define HW_VENDOR_DP 4u
#define MESSAGING_DEVICE_PATH 3u
#define MSG_SD_DP 0x1au
#define MEDIA_DEVICE_PATH 4u
#define MEDIA_HARDDRIVE_DP 1u
#define MEDIA_PIWG_FW_FILE_DP 6u
#define END_DEVICE_PATH_TYPE 0x7fu
#define END_ENTIRE_DEVICE_PATH_SUBTYPE 0xffu
#define END_INSTANCE_DEVICE_PATH_SUBTYPE 1u
#define DevicePathType(Node) ((Node)->Type)
#define DevicePathSubType(Node) ((Node)->SubType)
#define DevicePathNodeLength(Node) ((UINTN)(Node)->Length[0] | ((UINTN)(Node)->Length[1] << 8))
#define NextDevicePathNode(Node) ((EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)(Node) + DevicePathNodeLength(Node)))
#define IsDevicePathEnd(Node) ((Node)->Type == END_DEVICE_PATH_TYPE && (Node)->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)
typedef struct { EFI_DEVICE_PATH_PROTOCOL *FilePath; } EFI_LOADED_IMAGE_PROTOCOL;
typedef struct { EFI_HANDLE ImageHandle, DriverBindingHandle; } EFI_DRIVER_BINDING_PROTOCOL;
typedef struct {
  UINT32 MediaId;
  BOOLEAN LogicalPartition, MediaPresent;
  UINT32 BlockSize;
  UINT64 LastBlock;
} EFI_BLOCK_IO_MEDIA;
typedef struct { EFI_BLOCK_IO_MEDIA *Media; } EFI_BLOCK_IO_PROTOCOL;
typedef enum { ByProtocol = 2 } EFI_LOCATE_SEARCH_TYPE;
typedef struct {
  EFI_STATUS (*LocateProtocol)(EFI_GUID *, VOID *, VOID **);
  EFI_STATUS (*LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *, VOID *, UINTN *, EFI_HANDLE **);
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE, EFI_GUID *, VOID **);
  EFI_STATUS (*ConnectController)(EFI_HANDLE, EFI_HANDLE *, EFI_DEVICE_PATH_PROTOCOL *, BOOLEAN);
} EFI_BOOT_SERVICES;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_GUID gEfiLoadedImageProtocolGuid, gEfiDriverBindingProtocolGuid;
extern EFI_GUID gEfiBlockIoProtocolGuid, gEfiDevicePathProtocolGuid;
extern EFI_GUID gEfiDiskIoProtocolGuid, gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiUnicodeCollation2ProtocolGuid;
BOOLEAN IsDevicePathValid (CONST EFI_DEVICE_PATH_PROTOCOL *, UINTN);
INTN CompareMem (CONST VOID *, CONST VOID *, UINTN);
VOID FreePool (VOID *);
UINTN Print (CONST CHAR16 *, ...);
#endif

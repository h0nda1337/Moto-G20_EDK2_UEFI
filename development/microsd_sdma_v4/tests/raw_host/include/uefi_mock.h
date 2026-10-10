/* Host-only UEFI surface; not a firmware ABI. */
#ifndef RAW_SD_HOST_MOCK_H
#define RAW_SD_HOST_MOCK_H
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
#define EFI_DEVICE_ERROR (EFI_ERROR_BIT | UINT64_C(7))
#define EFI_NOT_FOUND (EFI_ERROR_BIT | UINT64_C(14))
#define EFI_ERROR(Status) (((Status) & EFI_ERROR_BIT) != 0)
typedef struct { UINT32 Data1; UINT16 Data2, Data3; UINT8 Data4[8]; } EFI_GUID;
#pragma pack(push, 1)
typedef struct { UINT8 Type, SubType, Length[2]; } EFI_DEVICE_PATH_PROTOCOL;
typedef struct { EFI_DEVICE_PATH_PROTOCOL Header; EFI_GUID Guid; } VENDOR_DEVICE_PATH;
typedef struct { EFI_DEVICE_PATH_PROTOCOL Header; UINT8 SlotNumber; } SD_DEVICE_PATH;
#pragma pack(pop)
#define HARDWARE_DEVICE_PATH 0x01u
#define HW_VENDOR_DP 0x04u
#define MESSAGING_DEVICE_PATH 0x03u
#define MSG_SD_DP 0x1au
#define END_DEVICE_PATH_TYPE 0x7fu
#define END_ENTIRE_DEVICE_PATH_SUBTYPE 0xffu
#define END_INSTANCE_DEVICE_PATH_SUBTYPE 0x01u
#define DevicePathType(Node) ((Node)->Type)
#define DevicePathSubType(Node) ((Node)->SubType)
#define DevicePathNodeLength(Node) ((UINTN)(Node)->Length[0] | ((UINTN)(Node)->Length[1] << 8))
#define NextDevicePathNode(Node) ((EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)(Node) + DevicePathNodeLength(Node)))
#define IsDevicePathEnd(Node) ((Node)->Type == END_DEVICE_PATH_TYPE && (Node)->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)
#define IsDevicePathEndInstance(Node) ((Node)->Type == END_DEVICE_PATH_TYPE && (Node)->SubType == END_INSTANCE_DEVICE_PATH_SUBTYPE)
BOOLEAN IsDevicePathValid (CONST EFI_DEVICE_PATH_PROTOCOL *, UINTN);
INTN CompareMem (CONST VOID *, CONST VOID *, UINTN);
VOID FreePool (VOID *);
UINTN Print (CONST CHAR16 *, ...);
typedef struct {
  UINT32 MediaId;
  BOOLEAN MediaPresent, LogicalPartition, ReadOnly;
  UINT32 BlockSize, IoAlign;
  UINT64 LastBlock;
} EFI_BLOCK_IO_MEDIA;
typedef struct _EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;
struct _EFI_BLOCK_IO_PROTOCOL {
  EFI_BLOCK_IO_MEDIA *Media;
  EFI_STATUS (*ReadBlocks)(EFI_BLOCK_IO_PROTOCOL *, UINT32, UINT64, UINTN, VOID *);
};
typedef enum { ByProtocol = 2 } EFI_LOCATE_SEARCH_TYPE;
typedef struct {
  EFI_STATUS (*LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *, VOID *, UINTN *, EFI_HANDLE **);
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE, EFI_GUID *, VOID **);
} EFI_BOOT_SERVICES;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_GUID gEfiBlockIoProtocolGuid;
extern EFI_GUID gEfiDevicePathProtocolGuid;
#endif

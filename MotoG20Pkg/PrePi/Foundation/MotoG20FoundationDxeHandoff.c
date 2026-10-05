/** @file
  Fresh high-HOB construction and bounded DXE Core handoff.

  The generic PrePi loader uses ASSERTs for malformed images and allocation
  failures.  This local path preserves its successful semantics while making
  every fallible operation explicit, bounded and visible on the framebuffer.
**/

#include <Base.h>
#include <PiPei.h>
#include <Pi/PiBootMode.h>
#include <Pi/PiFirmwareVolume.h>
#include <Guid/FdtHob.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PeCoffLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/PrePiLib.h>

#include "MotoG20FoundationBootstrap.h"
#include "MotoG20FoundationDisplay.h"

#define FOUNDATION_TEXT      __attribute__((section (".foundation.text")))
#define FOUNDATION_DATA      __attribute__((section (".foundation.data")))
#define FOUNDATION_NORETURN  __attribute__((noreturn))

#define MOTO_G20_FOUNDATION_DXE_CORE_MAX_IMAGE_SIZE  0x01000000ULL
#define MOTO_G20_FOUNDATION_DXE_CORE_IMAGE_SIZE      0x00049000ULL
#define MOTO_G20_FOUNDATION_DXE_CORE_ENTRY_RVA       0x00001000ULL
#define MOTO_G20_FOUNDATION_DXE_BRIDGE_GUARD_OFFSET  0x00001000ULL
#define MOTO_G20_FOUNDATION_DXE_BRIDGE_GUARD_VALUE   0x4252494447455354ULL

#define MOTO_G20_DXE_ENTRY_OPCODE_0  0xA9BE7BFDU
#define MOTO_G20_DXE_ENTRY_OPCODE_1  0x910003FDU
#define MOTO_G20_DXE_ENTRY_OPCODE_2  0xF9000FE0U
#define MOTO_G20_DXE_ENTRY_OPCODE_14 0x9400A7DDU

#define MOTO_G20_DXE_BRIDGE_FAIL_CONTEXT    0x4252444700000001ULL
#define MOTO_G20_DXE_BRIDGE_FAIL_SCTLR      0x4252444700000002ULL
#define MOTO_G20_DXE_BRIDGE_FAIL_STACK      0x4252444700000003ULL
#define MOTO_G20_DXE_BRIDGE_FAIL_HOB        0x4252444700000004ULL
#define MOTO_G20_DXE_BRIDGE_FAIL_IMAGE      0x4252444700000005ULL
#define MOTO_G20_DXE_BRIDGE_FAIL_OPCODE_PRE 0x4252444700000006ULL
#define MOTO_G20_DXE_BRIDGE_FAIL_GUARD      0x4252444700000007ULL
#define MOTO_G20_DXE_BRIDGE_FAIL_OPCODE_POST 0x4252444700000008ULL

typedef
VOID
(EFIAPI *MOTO_G20_DXE_CORE_ENTRY_POINT)(
  IN VOID  *HobList
  );

typedef struct {
  UINT64                Signature;
  EFI_PHYSICAL_ADDRESS  ImageAddress;
  UINT64                ImageSize;
  EFI_PHYSICAL_ADDRESS  EntryPoint;
  EFI_PHYSICAL_ADDRESS  StackBase;
  UINT64                StackSize;
  EFI_PHYSICAL_ADDRESS  StackTop;
  EFI_PHYSICAL_ADDRESS  StackGuard;
  VOID                  *HobList;
} MOTO_G20_FOUNDATION_DXE_BRIDGE_CONTEXT;

STATIC
BOOLEAN
FOUNDATION_TEXT
MotoG20FoundationRangeIsHighMemory (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  );

STATIC MOTO_G20_FOUNDATION_DXE_BRIDGE_CONTEXT
  mMotoG20FoundationDxeBridgeContext FOUNDATION_DATA;

STATIC
UINT64
FOUNDATION_TEXT
MotoG20FoundationReadSctlrEl2ForDxeBridge (
  VOID
  )
{
  UINT64  Value;

  __asm__ volatile ("mrs %0, SCTLR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINTN
FOUNDATION_TEXT
MotoG20FoundationReadSpForDxeBridge (
  VOID
  )
{
  UINTN  Value;

  __asm__ volatile ("mov %0, sp" : "=r" (Value));
  return Value;
}

STATIC
VOID
FOUNDATION_TEXT
FOUNDATION_NORETURN
MotoG20FoundationDxeBridgeFail (
  IN EFI_STATUS  Status,
  IN UINT64      Detail
  )
{
  MotoG20FoundationDisplayProgress (
    MotoG20FoundationStageDxeStackBridge,
    Status,
    Detail
    );
  ArmDataSynchronizationBarrier ();
  for (;;) {
    __asm__ volatile ("wfe");
  }
}

STATIC
VOID
EFIAPI
FOUNDATION_TEXT
FOUNDATION_NORETURN
MotoG20FoundationDxeStackBridge (
  IN VOID  *HobList,
  IN VOID  *ContextPointer
  )
{
  MOTO_G20_FOUNDATION_DXE_BRIDGE_CONTEXT  *Context;
  MOTO_G20_DXE_CORE_ENTRY_POINT            DxeCoreEntry;
  volatile UINT32                          *EntryOpcodes;
  volatile UINT64                          *StackGuard;
  UINT64                                   Sctlr;
  UINTN                                    StackPointer;

  Context = (MOTO_G20_FOUNDATION_DXE_BRIDGE_CONTEXT *)ContextPointer;
  if ((Context != &mMotoG20FoundationDxeBridgeContext) ||
      (Context->Signature != SIGNATURE_64 ('M', 'G', '2', '0', 'D', 'X', 'E', 'B'))) {
    MotoG20FoundationDxeBridgeFail (
      EFI_COMPROMISED_DATA,
      MOTO_G20_DXE_BRIDGE_FAIL_CONTEXT
      );
  }

  Sctlr = MotoG20FoundationReadSctlrEl2ForDxeBridge ();
  if ((Sctlr & (BIT0 | BIT2)) != (BIT0 | BIT2)) {
    MotoG20FoundationDxeBridgeFail (
      EFI_DEVICE_ERROR,
      MOTO_G20_DXE_BRIDGE_FAIL_SCTLR
      );
  }

  StackPointer = MotoG20FoundationReadSpForDxeBridge ();
  if (((StackPointer & (CPU_STACK_ALIGNMENT - 1U)) != 0) ||
      (StackPointer < (Context->StackBase + MOTO_G20_FOUNDATION_DXE_BRIDGE_GUARD_OFFSET + sizeof (UINT64))) ||
      (StackPointer > Context->StackTop) ||
      (Context->StackSize != MOTO_G20_FOUNDATION_DXE_STACK_SIZE)) {
    MotoG20FoundationDxeBridgeFail (
      EFI_COMPROMISED_DATA,
      MOTO_G20_DXE_BRIDGE_FAIL_STACK
      );
  }

  if ((HobList == NULL) || (HobList != Context->HobList) ||
      ((UINTN)HobList < MOTO_G20_FOUNDATION_DXE_MEMORY_BASE) ||
      ((UINTN)HobList >= MOTO_G20_FOUNDATION_DXE_MEMORY_END)) {
    MotoG20FoundationDxeBridgeFail (
      EFI_COMPROMISED_DATA,
      MOTO_G20_DXE_BRIDGE_FAIL_HOB
      );
  }

  if ((Context->ImageSize != MOTO_G20_FOUNDATION_DXE_CORE_IMAGE_SIZE) ||
      !MotoG20FoundationRangeIsHighMemory (Context->ImageAddress, Context->ImageSize) ||
      (Context->EntryPoint !=
       (Context->ImageAddress + MOTO_G20_FOUNDATION_DXE_CORE_ENTRY_RVA))) {
    MotoG20FoundationDxeBridgeFail (
      EFI_COMPROMISED_DATA,
      MOTO_G20_DXE_BRIDGE_FAIL_IMAGE
      );
  }

  EntryOpcodes = (volatile UINT32 *)(UINTN)Context->EntryPoint;
  if ((EntryOpcodes[0] != MOTO_G20_DXE_ENTRY_OPCODE_0) ||
      (EntryOpcodes[1] != MOTO_G20_DXE_ENTRY_OPCODE_1) ||
      (EntryOpcodes[2] != MOTO_G20_DXE_ENTRY_OPCODE_2) ||
      (EntryOpcodes[14] != MOTO_G20_DXE_ENTRY_OPCODE_14)) {
    MotoG20FoundationDxeBridgeFail (
      EFI_COMPROMISED_DATA,
      MOTO_G20_DXE_BRIDGE_FAIL_OPCODE_PRE
      );
  }

  StackGuard = (volatile UINT64 *)(UINTN)Context->StackGuard;
  if (*StackGuard != MOTO_G20_FOUNDATION_DXE_BRIDGE_GUARD_VALUE) {
    MotoG20FoundationDxeBridgeFail (
      EFI_COMPROMISED_DATA,
      MOTO_G20_DXE_BRIDGE_FAIL_GUARD
      );
  }

  MotoG20FoundationDisplayProgress (
    MotoG20FoundationStageDxeStackBridge,
    EFI_SUCCESS,
    StackPointer
    );

  WriteBackDataCacheRange (
    (VOID *)(UINTN)Context->ImageAddress,
    (UINTN)Context->ImageSize
    );
  InvalidateInstructionCacheRange (
    (VOID *)(UINTN)Context->ImageAddress,
    (UINTN)Context->ImageSize
    );
  ArmDataSynchronizationBarrier ();
  ArmInstructionSynchronizationBarrier ();

  if ((EntryOpcodes[0] != MOTO_G20_DXE_ENTRY_OPCODE_0) ||
      (EntryOpcodes[1] != MOTO_G20_DXE_ENTRY_OPCODE_1) ||
      (EntryOpcodes[2] != MOTO_G20_DXE_ENTRY_OPCODE_2) ||
      (EntryOpcodes[14] != MOTO_G20_DXE_ENTRY_OPCODE_14)) {
    MotoG20FoundationDxeBridgeFail (
      EFI_COMPROMISED_DATA,
      MOTO_G20_DXE_BRIDGE_FAIL_OPCODE_POST
      );
  }

  MotoG20FoundationDisplayProgress (
    MotoG20FoundationStageDxeEntryDispatch,
    EFI_SUCCESS,
    Context->EntryPoint
    );
  ArmDataSynchronizationBarrier ();
  ArmInstructionSynchronizationBarrier ();

  DxeCoreEntry = (MOTO_G20_DXE_CORE_ENTRY_POINT)(UINTN)Context->EntryPoint;
  DxeCoreEntry (HobList);

  MotoG20FoundationDisplayProgress (
    MotoG20FoundationStageDxeCoreReturned,
    EFI_DEVICE_ERROR,
    Context->EntryPoint
    );
  ArmDataSynchronizationBarrier ();
  for (;;) {
    __asm__ volatile ("wfe");
  }
}

STATIC
BOOLEAN
FOUNDATION_TEXT
MotoG20FoundationRangeIsHighMemory (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  return (BOOLEAN)(
                    (Size != 0) &&
                    (Base >= MOTO_G20_FOUNDATION_DXE_MEMORY_BASE) &&
                    (Base < MOTO_G20_FOUNDATION_DXE_MEMORY_END) &&
                    (Size <= (MOTO_G20_FOUNDATION_DXE_MEMORY_END - Base))
                    );
}

STATIC
VOID *
FOUNDATION_TEXT
MotoG20FoundationAllocateCodePages (
  IN UINTN  Pages
  )
{
  VOID                  *Allocation;
  EFI_PEI_HOB_POINTERS  Hob;
  UINT64                AllocationSize;

  if ((Pages == 0) || (Pages > (MAX_UINTN / EFI_PAGE_SIZE))) {
    return NULL;
  }

  AllocationSize = (UINT64)Pages * EFI_PAGE_SIZE;
  Allocation     = AllocatePages (Pages);
  if ((Allocation == NULL) ||
      !MotoG20FoundationRangeIsHighMemory (
         (EFI_PHYSICAL_ADDRESS)(UINTN)Allocation,
         AllocationSize
         )) {
    return NULL;
  }

  Hob.Raw = GetFirstHob (EFI_HOB_TYPE_MEMORY_ALLOCATION);
  while (Hob.Raw != NULL) {
    if ((Hob.MemoryAllocation->AllocDescriptor.MemoryBaseAddress ==
         (EFI_PHYSICAL_ADDRESS)(UINTN)Allocation) &&
        (Hob.MemoryAllocation->AllocDescriptor.MemoryLength == AllocationSize) &&
        (Hob.MemoryAllocation->AllocDescriptor.MemoryType == EfiBootServicesData)) {
      Hob.MemoryAllocation->AllocDescriptor.MemoryType = EfiBootServicesCode;
      return Allocation;
    }

    Hob.Raw = GetNextHob (
                EFI_HOB_TYPE_MEMORY_ALLOCATION,
                GET_NEXT_HOB (Hob)
                );
  }

  return NULL;
}

STATIC
BOOLEAN
FOUNDATION_TEXT
MotoG20FoundationModuleHobMatches (
  IN CONST EFI_GUID        *ModuleName,
  IN EFI_PHYSICAL_ADDRESS  ImageAddress,
  IN UINT64                ImageSize,
  IN EFI_PHYSICAL_ADDRESS  EntryPoint
  )
{
  EFI_PEI_HOB_POINTERS  Hob;

  Hob.Raw = GetFirstHob (EFI_HOB_TYPE_MEMORY_ALLOCATION);
  while (Hob.Raw != NULL) {
    if ((Hob.Header->HobLength >= sizeof (EFI_HOB_MEMORY_ALLOCATION_MODULE)) &&
        CompareGuid (
          &Hob.MemoryAllocationModule->MemoryAllocationHeader.Name,
          &gEfiHobMemoryAllocModuleGuid
          ) &&
        CompareGuid (&Hob.MemoryAllocationModule->ModuleName, ModuleName) &&
        (Hob.MemoryAllocationModule->MemoryAllocationHeader.MemoryBaseAddress == ImageAddress) &&
        (Hob.MemoryAllocationModule->MemoryAllocationHeader.MemoryLength == ImageSize) &&
        (Hob.MemoryAllocationModule->EntryPoint == EntryPoint)) {
      return TRUE;
    }

    Hob.Raw = GetNextHob (
                EFI_HOB_TYPE_MEMORY_ALLOCATION,
                GET_NEXT_HOB (Hob)
                );
  }

  return FALSE;
}

STATIC
BOOLEAN
FOUNDATION_TEXT
MotoG20FoundationStackHobMatches (
  IN EFI_PHYSICAL_ADDRESS  StackBase,
  IN UINT64                StackSize
  )
{
  EFI_PEI_HOB_POINTERS  Hob;

  Hob.Raw = GetFirstHob (EFI_HOB_TYPE_MEMORY_ALLOCATION);
  while (Hob.Raw != NULL) {
    if ((Hob.Header->HobLength >= sizeof (EFI_HOB_MEMORY_ALLOCATION_STACK)) &&
        CompareGuid (
          &Hob.MemoryAllocationStack->AllocDescriptor.Name,
          &gEfiHobMemoryAllocStackGuid
          ) &&
        (Hob.MemoryAllocationStack->AllocDescriptor.MemoryBaseAddress == StackBase) &&
        (Hob.MemoryAllocationStack->AllocDescriptor.MemoryLength == StackSize)) {
      return TRUE;
    }

    Hob.Raw = GetNextHob (
                EFI_HOB_TYPE_MEMORY_ALLOCATION,
                GET_NEXT_HOB (Hob)
                );
  }

  return FALSE;
}

STATIC
BOOLEAN
FOUNDATION_TEXT
MotoG20FoundationFvHobsMatch (
  IN EFI_PHYSICAL_ADDRESS  FvBase,
  IN UINT64                FvSize
  )
{
  EFI_PEI_HOB_POINTERS  Hob;
  BOOLEAN               FoundFv;
  BOOLEAN               FoundResource;

  FoundFv       = FALSE;
  FoundResource = FALSE;
  Hob.Raw       = GetFirstHob (EFI_HOB_TYPE_FV);
  while (Hob.Raw != NULL) {
    if ((Hob.FirmwareVolume->BaseAddress == FvBase) &&
        (Hob.FirmwareVolume->Length == FvSize)) {
      FoundFv = TRUE;
      break;
    }

    Hob.Raw = GetNextHob (EFI_HOB_TYPE_FV, GET_NEXT_HOB (Hob));
  }

  Hob.Raw = GetFirstHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR);
  while (Hob.Raw != NULL) {
    if ((Hob.ResourceDescriptor->ResourceType == EFI_RESOURCE_FIRMWARE_DEVICE) &&
        (Hob.ResourceDescriptor->PhysicalStart == FvBase) &&
        (Hob.ResourceDescriptor->ResourceLength == FvSize)) {
      FoundResource = TRUE;
      break;
    }

    Hob.Raw = GetNextHob (
                EFI_HOB_TYPE_RESOURCE_DESCRIPTOR,
                GET_NEXT_HOB (Hob)
                );
  }

  return (BOOLEAN)(FoundFv && FoundResource);
}

STATIC
EFI_STATUS
FOUNDATION_TEXT
MotoG20FoundationLoadPeCoff (
  IN  VOID                  *PeCoffImage,
  OUT EFI_PHYSICAL_ADDRESS  *ImageAddress,
  OUT UINT64                *ImageSize,
  OUT EFI_PHYSICAL_ADDRESS  *EntryPoint,
  OUT UINT32                *ImageError
  )
{
  PE_COFF_LOADER_IMAGE_CONTEXT  ImageContext;
  RETURN_STATUS                 ReturnStatus;
  VOID                          *Buffer;
  UINTN                         Pages;
  UINT64                        RoundedSize;

  if ((PeCoffImage == NULL) || (ImageAddress == NULL) ||
      (ImageSize == NULL) || (EntryPoint == NULL) || (ImageError == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *ImageAddress = 0;
  *ImageSize    = 0;
  *EntryPoint   = 0;
  *ImageError   = 0;
  ZeroMem (&ImageContext, sizeof (ImageContext));
  ImageContext.Handle    = PeCoffImage;
  ImageContext.ImageRead = PeCoffLoaderImageReadFromMemory;

  ReturnStatus = PeCoffLoaderGetImageInfo (&ImageContext);
  *ImageError  = ImageContext.ImageError;
  if (RETURN_ERROR (ReturnStatus)) {
    return (EFI_STATUS)ReturnStatus;
  }

  if ((ImageContext.Machine != IMAGE_FILE_MACHINE_ARM64) ||
      (ImageContext.ImageSize == 0) ||
      (ImageContext.ImageSize > MOTO_G20_FOUNDATION_DXE_CORE_MAX_IMAGE_SIZE) ||
      (ImageContext.ImageSize > MAX_UINTN) ||
      ImageContext.RelocationsStripped) {
    return EFI_UNSUPPORTED;
  }

  Pages       = EFI_SIZE_TO_PAGES ((UINTN)ImageContext.ImageSize);
  RoundedSize = (UINT64)Pages * EFI_PAGE_SIZE;
  Buffer      = MotoG20FoundationAllocateCodePages (Pages);
  if ((Buffer == NULL) ||
      !MotoG20FoundationRangeIsHighMemory (
         (EFI_PHYSICAL_ADDRESS)(UINTN)Buffer,
         RoundedSize
         )) {
    return EFI_OUT_OF_RESOURCES;
  }

  ImageContext.ImageAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)Buffer;
  ReturnStatus = PeCoffLoaderLoadImage (&ImageContext);
  *ImageError  = ImageContext.ImageError;
  if (RETURN_ERROR (ReturnStatus)) {
    return (EFI_STATUS)ReturnStatus;
  }

  ReturnStatus = PeCoffLoaderRelocateImage (&ImageContext);
  *ImageError  = ImageContext.ImageError;
  if (RETURN_ERROR (ReturnStatus)) {
    return (EFI_STATUS)ReturnStatus;
  }

  if ((ImageContext.EntryPoint < ImageContext.ImageAddress) ||
      (ImageContext.EntryPoint >=
       (ImageContext.ImageAddress + ImageContext.ImageSize))) {
    return EFI_COMPROMISED_DATA;
  }

  InvalidateInstructionCacheRange (
    (VOID *)(UINTN)ImageContext.ImageAddress,
    (UINTN)ImageContext.ImageSize
    );
  *ImageAddress = ImageContext.ImageAddress;
  *ImageSize    = ImageContext.ImageSize;
  *EntryPoint   = ImageContext.EntryPoint;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
FOUNDATION_TEXT
MotoG20FoundationLoadDxeCore (
  OUT MOTO_G20_FOUNDATION_STAGE  *FailureStage
  )
{
  EFI_PEI_FV_HANDLE    VolumeHandle;
  EFI_PEI_FILE_HANDLE  FileHandle;
  EFI_FV_FILE_INFO     FileInfo;
  EFI_STATUS           Status;
  VOID                 *PeCoffImage;
  EFI_PHYSICAL_ADDRESS ImageAddress;
  UINT64               ImageSize;
  EFI_PHYSICAL_ADDRESS EntryPoint;
  UINT32               ImageError;
  UINTN                ImagePages;
  UINTN                StackPages;
  VOID                 *BaseOfStack;
  VOID                 *TopOfStack;
  VOID                 *HobList;

  if (FailureStage == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  VolumeHandle = NULL;
  FileHandle   = NULL;
  PeCoffImage  = NULL;
  *FailureStage = MotoG20FoundationStageDxeCoreLocate;
  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_NOT_READY,
    MOTO_G20_FOUNDATION_FV_STAGING_BASE
    );
  Status = FfsAnyFvFindFirstFile (
             EFI_FV_FILETYPE_DXE_CORE,
             &VolumeHandle,
             &FileHandle
             );
  if (EFI_ERROR (Status) || (VolumeHandle == NULL) || (FileHandle == NULL)) {
    Status = EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
    MotoG20FoundationDisplayProgress (*FailureStage, Status, 0);
    return Status;
  }

  Status = FfsFindSectionDataWithHook (
             EFI_SECTION_PE32,
             NULL,
             FileHandle,
             &PeCoffImage
             );
  if (EFI_ERROR (Status) || (PeCoffImage == NULL)) {
    Status = EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
    MotoG20FoundationDisplayProgress (
      *FailureStage,
      Status,
      (UINT64)(UINTN)FileHandle
      );
    return Status;
  }

  ZeroMem (&FileInfo, sizeof (FileInfo));
  Status = FfsGetFileInfo (FileHandle, &FileInfo);
  if (EFI_ERROR (Status)) {
    MotoG20FoundationDisplayProgress (
      *FailureStage,
      Status,
      (UINT64)(UINTN)FileHandle
      );
    return Status;
  }

  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_SUCCESS,
    (UINT64)(UINTN)PeCoffImage
    );

  *FailureStage = MotoG20FoundationStageDxeCoreLoad;
  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_NOT_READY,
    (UINT64)(UINTN)PeCoffImage
    );
  Status = MotoG20FoundationLoadPeCoff (
             PeCoffImage,
             &ImageAddress,
             &ImageSize,
             &EntryPoint,
             &ImageError
             );
  if (EFI_ERROR (Status)) {
    MotoG20FoundationDisplayProgress (*FailureStage, Status, ImageError);
    return Status;
  }

  ImagePages = EFI_SIZE_TO_PAGES ((UINTN)ImageSize);
  BuildModuleHob (
    &FileInfo.FileName,
    ImageAddress,
    (UINT64)ImagePages * EFI_PAGE_SIZE,
    EntryPoint
    );
  if (!MotoG20FoundationModuleHobMatches (
         &FileInfo.FileName,
         ImageAddress,
         (UINT64)ImagePages * EFI_PAGE_SIZE,
         EntryPoint
         )) {
    Status = EFI_COMPROMISED_DATA;
    MotoG20FoundationDisplayProgress (*FailureStage, Status, ImageAddress);
    return Status;
  }

  MotoG20FoundationDisplayProgress (*FailureStage, EFI_SUCCESS, EntryPoint);

  *FailureStage = MotoG20FoundationStageDxeStack;
  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_NOT_READY,
    MOTO_G20_FOUNDATION_DXE_STACK_SIZE
    );
  StackPages  = EFI_SIZE_TO_PAGES (MOTO_G20_FOUNDATION_DXE_STACK_SIZE);
  BaseOfStack = AllocatePages (StackPages);
  if ((BaseOfStack == NULL) ||
      !MotoG20FoundationRangeIsHighMemory (
         (EFI_PHYSICAL_ADDRESS)(UINTN)BaseOfStack,
         (UINT64)StackPages * EFI_PAGE_SIZE
         )) {
    Status = EFI_OUT_OF_RESOURCES;
    MotoG20FoundationDisplayProgress (*FailureStage, Status, 0);
    return Status;
  }

  TopOfStack = (VOID *)((UINTN)BaseOfStack +
                        ((UINTN)StackPages * EFI_PAGE_SIZE) -
                        CPU_STACK_ALIGNMENT);
  TopOfStack = ALIGN_POINTER (TopOfStack, CPU_STACK_ALIGNMENT);
  UpdateStackHob (
    (EFI_PHYSICAL_ADDRESS)(UINTN)BaseOfStack,
    MOTO_G20_FOUNDATION_DXE_STACK_SIZE
    );
  if (!MotoG20FoundationStackHobMatches (
         (EFI_PHYSICAL_ADDRESS)(UINTN)BaseOfStack,
         MOTO_G20_FOUNDATION_DXE_STACK_SIZE
         )) {
    Status = EFI_COMPROMISED_DATA;
    MotoG20FoundationDisplayProgress (
      *FailureStage,
      Status,
      (UINT64)(UINTN)BaseOfStack
      );
    return Status;
  }

  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_SUCCESS,
    (UINT64)(UINTN)TopOfStack
    );

  HobList = GetHobList ();
  if (HobList == NULL) {
    Status = EFI_NOT_FOUND;
    MotoG20FoundationDisplayProgress (*FailureStage, Status, 0);
    return Status;
  }

  ZeroMem (&mMotoG20FoundationDxeBridgeContext, sizeof (mMotoG20FoundationDxeBridgeContext));
  mMotoG20FoundationDxeBridgeContext.ImageAddress = ImageAddress;
  mMotoG20FoundationDxeBridgeContext.ImageSize    = ImageSize;
  mMotoG20FoundationDxeBridgeContext.EntryPoint   = EntryPoint;
  mMotoG20FoundationDxeBridgeContext.StackBase    = (EFI_PHYSICAL_ADDRESS)(UINTN)BaseOfStack;
  mMotoG20FoundationDxeBridgeContext.StackSize    = MOTO_G20_FOUNDATION_DXE_STACK_SIZE;
  mMotoG20FoundationDxeBridgeContext.StackTop     = (EFI_PHYSICAL_ADDRESS)(UINTN)TopOfStack;
  mMotoG20FoundationDxeBridgeContext.StackGuard   =
    (EFI_PHYSICAL_ADDRESS)(UINTN)BaseOfStack +
    MOTO_G20_FOUNDATION_DXE_BRIDGE_GUARD_OFFSET;
  mMotoG20FoundationDxeBridgeContext.HobList = HobList;
  *(volatile UINT64 *)(UINTN)mMotoG20FoundationDxeBridgeContext.StackGuard =
    MOTO_G20_FOUNDATION_DXE_BRIDGE_GUARD_VALUE;
  ArmDataSynchronizationBarrier ();
  if (*(volatile UINT64 *)(UINTN)mMotoG20FoundationDxeBridgeContext.StackGuard !=
      MOTO_G20_FOUNDATION_DXE_BRIDGE_GUARD_VALUE) {
    Status = EFI_DEVICE_ERROR;
    MotoG20FoundationDisplayProgress (
      MotoG20FoundationStageDxeStackBridge,
      Status,
      MOTO_G20_DXE_BRIDGE_FAIL_GUARD
      );
    return Status;
  }

  mMotoG20FoundationDxeBridgeContext.Signature =
    SIGNATURE_64 ('M', 'G', '2', '0', 'D', 'X', 'E', 'B');
  ArmDataSynchronizationBarrier ();

  *FailureStage = MotoG20FoundationStageDxeCoreTransfer;
  MotoG20FoundationDisplayProgress (*FailureStage, EFI_SUCCESS, EntryPoint);
  SwitchStack (
    MotoG20FoundationDxeStackBridge,
    HobList,
    &mMotoG20FoundationDxeBridgeContext,
    TopOfStack
    );

  *FailureStage = MotoG20FoundationStageDxeCoreReturned;
  Status = EFI_DEVICE_ERROR;
  MotoG20FoundationDisplayProgress (*FailureStage, Status, EntryPoint);
  return Status;
}

EFI_STATUS
EFIAPI
FOUNDATION_TEXT
MotoG20FoundationBuildHobAndLoadDxe (
  IN  CONST MOTO_G20_FOUNDATION_FV_PLAN  *Plan,
  OUT MOTO_G20_FOUNDATION_STAGE          *FailureStage
  )
{
  EFI_HOB_HANDOFF_INFO_TABLE  *HobList;
  EFI_RESOURCE_ATTRIBUTE_TYPE SystemAttributes;
  EFI_RESOURCE_ATTRIBUTE_TYPE FvAttributes;
  EFI_STATUS                  Status;
  UINT64                      *FdtHobData;
  volatile UINT64             *DxeMemory;

  if (FailureStage == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *FailureStage = MotoG20FoundationStageDxeMemoryClear;
  if ((Plan == NULL) ||
      (Plan->DtbAddress != MOTO_G20_FOUNDATION_DTB_ADDRESS) ||
      (Plan->StagingBase != MOTO_G20_FOUNDATION_FV_STAGING_BASE) ||
      (Plan->StagingSize == 0) ||
      (Plan->StagingSize > MOTO_G20_FOUNDATION_FV_TRANSPORT_MAX) ||
      (Plan->FirmwareVolumeSize < sizeof (EFI_FIRMWARE_VOLUME_HEADER)) ||
      (Plan->FirmwareVolumeSize > Plan->StagingSize)) {
    Status = EFI_INVALID_PARAMETER;
    MotoG20FoundationDisplayProgress (*FailureStage, Status, 0);
    return Status;
  }

  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_NOT_READY,
    MOTO_G20_FOUNDATION_DXE_MEMORY_SIZE
    );
  SetMem (
    (VOID *)(UINTN)MOTO_G20_FOUNDATION_DXE_MEMORY_BASE,
    (UINTN)MOTO_G20_FOUNDATION_DXE_MEMORY_SIZE,
    0
    );
  DxeMemory = (volatile UINT64 *)(UINTN)MOTO_G20_FOUNDATION_DXE_MEMORY_BASE;
  if ((DxeMemory[0] != 0) ||
      (DxeMemory[MOTO_G20_FOUNDATION_DXE_MEMORY_SIZE / (2U * sizeof (UINT64))] != 0) ||
      (DxeMemory[(MOTO_G20_FOUNDATION_DXE_MEMORY_SIZE / sizeof (UINT64)) - 1U] != 0)) {
    Status = EFI_DEVICE_ERROR;
    MotoG20FoundationDisplayProgress (*FailureStage, Status, 0);
    return Status;
  }

  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_SUCCESS,
    MOTO_G20_FOUNDATION_DXE_MEMORY_BASE
    );

  *FailureStage = MotoG20FoundationStageHobConstruct;
  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_NOT_READY,
    MOTO_G20_FOUNDATION_DXE_MEMORY_BASE
    );
  HobList = HobConstructor (
              (VOID *)(UINTN)MOTO_G20_FOUNDATION_DXE_MEMORY_BASE,
              (UINTN)MOTO_G20_FOUNDATION_DXE_MEMORY_SIZE,
              (VOID *)(UINTN)MOTO_G20_FOUNDATION_DXE_MEMORY_BASE,
              (VOID *)(UINTN)MOTO_G20_FOUNDATION_DXE_MEMORY_END
              );
  if ((HobList == NULL) ||
      (HobList->EfiMemoryBottom != MOTO_G20_FOUNDATION_DXE_MEMORY_BASE) ||
      (HobList->EfiMemoryTop != MOTO_G20_FOUNDATION_DXE_MEMORY_END) ||
      (HobList->EfiFreeMemoryBottom < MOTO_G20_FOUNDATION_DXE_MEMORY_BASE) ||
      (HobList->EfiFreeMemoryTop > MOTO_G20_FOUNDATION_DXE_MEMORY_END) ||
      (HobList->EfiFreeMemoryBottom >= HobList->EfiFreeMemoryTop)) {
    Status = EFI_OUT_OF_RESOURCES;
    MotoG20FoundationDisplayProgress (*FailureStage, Status, 0);
    return Status;
  }

  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_SUCCESS,
    (UINT64)(UINTN)HobList
    );

  *FailureStage = MotoG20FoundationStageHobPublish;
  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_NOT_READY,
    (UINT64)(UINTN)HobList
    );
  Status = PrePeiSetHobList (HobList);
  if (EFI_ERROR (Status) || (PrePeiGetHobList () != HobList)) {
    Status = EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    MotoG20FoundationDisplayProgress (*FailureStage, Status, 0);
    return Status;
  }

  SetBootMode (BOOT_WITH_FULL_CONFIGURATION);
  SystemAttributes = EFI_RESOURCE_ATTRIBUTE_PRESENT |
                     EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                     EFI_RESOURCE_ATTRIBUTE_TESTED |
                     EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE;
  BuildResourceDescriptorHob (
    EFI_RESOURCE_SYSTEM_MEMORY,
    SystemAttributes,
    MOTO_G20_FOUNDATION_DXE_MEMORY_BASE,
    MOTO_G20_FOUNDATION_DXE_MEMORY_SIZE
    );
  BuildStackHob (0x811F0000ULL, 0x00010000ULL);
  BuildCpuHob (40, 16);

  FdtHobData = BuildGuidHob (&gFdtHobGuid, sizeof (*FdtHobData));
  if (FdtHobData == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    MotoG20FoundationDisplayProgress (*FailureStage, Status, 0);
    return Status;
  }

  *FdtHobData = Plan->DtbAddress;
  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_SUCCESS,
    (UINT64)(UINTN)PrePeiGetHobList ()
    );

  *FailureStage = MotoG20FoundationStageFvHob;
  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_NOT_READY,
    Plan->StagingBase
    );
  FvAttributes = EFI_RESOURCE_ATTRIBUTE_PRESENT |
                 EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                 EFI_RESOURCE_ATTRIBUTE_TESTED |
                 EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE;
  BuildFvHobs (Plan->StagingBase, Plan->FirmwareVolumeSize, &FvAttributes);
  if (!MotoG20FoundationFvHobsMatch (
         Plan->StagingBase,
         Plan->FirmwareVolumeSize
         )) {
    Status = EFI_COMPROMISED_DATA;
    MotoG20FoundationDisplayProgress (*FailureStage, Status, Plan->StagingBase);
    return Status;
  }

  MotoG20FoundationDisplayProgress (
    *FailureStage,
    EFI_SUCCESS,
    Plan->FirmwareVolumeSize
    );
  return MotoG20FoundationLoadDxeCore (FailureStage);
}

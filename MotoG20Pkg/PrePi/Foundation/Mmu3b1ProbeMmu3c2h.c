/** @file
  Minimal bridge from the physically frozen MMU3C2I path into the EDK2
  Foundation continuation.

  The frozen probe remains a separately linked object at its historical RVA.
  This file performs only the work that must happen around it:

    * before M=1, locate and stage the firmware-volume transport and validate
      the complete extended bootstrap image against the live translation map;
    * enter the frozen MMU/exception path;
    * after the frozen success tail is surgically redirected here, extend the
      live map and hand a fresh high HOB list to DXE Core.

  No diagnostic renderer is duplicated here.
**/

#include <Base.h>
#include <PiPei.h>
#include <AArch64/AArch64Mmu.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/HobLib.h>
#include <Library/MotoG20TranslationTableLib.h>

#include "MotoG20FoundationBootstrap.h"
#include "MotoG20FoundationDisplay.h"
#include "MotoG20FoundationFvIdentity.generated.h"

#if MOTO_G20_FOUNDATION_FV_IDENTITY_PROVISIONED != 1
#error "Foundation FV identity is not provisioned; bootstrap build is fail-closed"
#endif

#define FOUNDATION_TEXT      __attribute__((section (".foundation.text")))
#define FOUNDATION_DATA      __attribute__((section (".foundation.data")))
#define FOUNDATION_RODATA    __attribute__((section (".foundation.rodata")))
#define FOUNDATION_NORETURN  __attribute__((noreturn))

#define MOTO_G20_FOUNDATION_FAILURE_SIGNATURE  0x4641494C33463247ULL
#define MOTO_G20_FOUNDATION_PE_BASE            0x81000000ULL
#define MOTO_G20_FOUNDATION_PE_MAX_SIZE        0x00100000ULL
#define MOTO_G20_FOUNDATION_PAGE_SIZE          0x1000ULL
#define MOTO_G20_FOUNDATION_MMU_BITS           (BIT0 | BIT2 | BIT12)
#define MOTO_G20_FOUNDATION_LOADER_STACK_BASE  0x811D0000ULL
#define MOTO_G20_FOUNDATION_LOADER_STACK_SIZE  0x00030000ULL
#define MOTO_G20_FOUNDATION_LOW_HOB_BASE        0x81882000ULL
#define MOTO_G20_FOUNDATION_LOW_HOB_SIZE        0x00100000ULL
#define MOTO_G20_FOUNDATION_FB_BASE             0x9E000000ULL
#define MOTO_G20_FOUNDATION_FB_L2_BLOCKS        4U
#define MOTO_G20_FOUNDATION_FB_L3_PAGES         0x1E4U
#define MOTO_G20_FOUNDATION_FB_DESCRIPTOR_WRITES \
  (MOTO_G20_FOUNDATION_FB_L2_BLOCKS + MOTO_G20_FOUNDATION_FB_L3_PAGES)

typedef struct {
  UINT64  MagicA;
  UINT64  MagicB;
  UINT64  DtbAddress;
  UINT64  PhysicalAddressBits;
} MOTO_G20_FOUNDATION_LOW_HOB_PAYLOAD;

extern VOID EFIAPI MotoG20Mmu3b1ProbeFrozenTerminal (VOID);
extern UINT8 __MotoG20FoundationImageEnd[];

VOID
EFIAPI
ProcessLibraryConstructorList (
  VOID
  );

STATIC CONST EFI_GUID mMotoG20FoundationRealHobGuid FOUNDATION_RODATA = {
  0x8c84d850,
  0xa9a6,
  0x48bb,
  { 0xa4, 0x55, 0x4d, 0x4f, 0x54, 0x4f, 0x47, 0x32 }
};

STATIC CONST MOTO_G20_FOUNDATION_FV_IDENTITY mMotoG20FoundationExpectedFv FOUNDATION_RODATA = {
  MOTO_G20_FOUNDATION_FV_TRANSPORT_SIZE,
  MOTO_G20_FOUNDATION_FV_SIZE,
  MOTO_G20_FOUNDATION_FV_SHA256_BYTES
};

STATIC MOTO_G20_FOUNDATION_FV_PLAN mMotoG20FoundationPlan FOUNDATION_DATA;

volatile MOTO_G20_FOUNDATION_FAILURE_RECORD gMotoG20FoundationFailure FOUNDATION_DATA = {
  MOTO_G20_FOUNDATION_FAILURE_SIGNATURE,
  MotoG20FoundationStageNone,
  EFI_NOT_READY,
  0,
  0
};

STATIC
UINT64
FOUNDATION_TEXT
MotoG20FoundationReadSctlrEl2 (
  VOID
  )
{
  UINT64  Value;

  __asm__ volatile ("mrs %0, SCTLR_EL2" : "=r" (Value));
  return Value;
}

/**
  Move from the physically proven M-only regime to M+C without changing any
  other SCTLR_EL2 bit, then prove that the UMS512 accepts the exact exclusive
  primitive used by BaseSynchronizationLib.

  The cache invalidations cover only Normal-WB ranges that are both mapped and
  consumed by the Foundation/DXE path.  They run while C=0, so no dirty line
  can have been allocated by this execution regime.  The high map must already
  be installed before this function is called, making 0x82000000-0x874FFFFF
  one continuous Normal-WB interval.
**/
STATIC
EFI_STATUS
FOUNDATION_TEXT
MotoG20FoundationEnableDataCacheAndTestExclusive (
  OUT UINT64  *EnabledSctlr
  )
{
  volatile UINT32  *Probe;
  UINT64           BeforeSctlr;
  UINT64           AfterSctlr;
  UINT32           Loaded;
  UINT32           Updated;
  UINT32           StoreStatus;
  UINTN            Attempt;

  if (EnabledSctlr == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *EnabledSctlr = 0;
  BeforeSctlr  = MotoG20FoundationReadSctlrEl2 ();
  if ((BeforeSctlr & MOTO_G20_FOUNDATION_MMU_BITS) != BIT0) {
    return EFI_SECURITY_VIOLATION;
  }

  Probe  = (volatile UINT32 *)(UINTN)MOTO_G20_FOUNDATION_DXE_MEMORY_BASE;
  *Probe = 0;
  ArmDataSynchronizationBarrier ();

  InvalidateDataCacheRange (
    (VOID *)(UINTN)MOTO_G20_FOUNDATION_PE_BASE,
    (UINTN)MOTO_G20_FOUNDATION_PE_MAX_SIZE
    );
  InvalidateDataCacheRange (
    (VOID *)(UINTN)MOTO_G20_FOUNDATION_LOADER_STACK_BASE,
    (UINTN)MOTO_G20_FOUNDATION_LOADER_STACK_SIZE
    );
  InvalidateDataCacheRange (
    (VOID *)(UINTN)MOTO_G20_FOUNDATION_LOW_HOB_BASE,
    (UINTN)MOTO_G20_FOUNDATION_LOW_HOB_SIZE
    );
  InvalidateDataCacheRange (
    (VOID *)(UINTN)MOTO_G20_FOUNDATION_DTB_ADDRESS,
    (UINTN)(MOTO_G20_FOUNDATION_DXE_MEMORY_END - MOTO_G20_FOUNDATION_DTB_ADDRESS)
    );

  ArmDataSynchronizationBarrier ();
  ArmEnableDataCache ();
  AfterSctlr   = MotoG20FoundationReadSctlrEl2 ();
  *EnabledSctlr = AfterSctlr;
  if (((BeforeSctlr ^ AfterSctlr) != BIT2) ||
      ((AfterSctlr & MOTO_G20_FOUNDATION_MMU_BITS) != (BIT0 | BIT2))) {
    return EFI_SECURITY_VIOLATION;
  }

  StoreStatus = 1U;
  Loaded      = MAX_UINT32;
  Updated     = MAX_UINT32;
  for (Attempt = 0; Attempt < 64U; Attempt++) {
    __asm__ volatile (
      "ldxr %w0, [%3]\n"
      "add %w1, %w0, #1\n"
      "stxr %w2, %w1, [%3]\n"
      : "=&r" (Loaded), "=&r" (Updated), "=&r" (StoreStatus)
      : "r" (Probe)
      : "memory"
      );
    if (StoreStatus == 0U) {
      break;
    }
  }

  ArmDataSynchronizationBarrier ();
  if ((StoreStatus != 0U) || (Loaded != 0U) ||
      (Updated != 1U) || (*Probe != 1U)) {
    return EFI_DEVICE_ERROR;
  }

  *Probe = 0;
  ArmDataSynchronizationBarrier ();
  return EFI_SUCCESS;
}

STATIC
VOID
FOUNDATION_TEXT
FOUNDATION_NORETURN
MotoG20FoundationFailClosed (
  IN MOTO_G20_FOUNDATION_STAGE  Stage,
  IN EFI_STATUS                 Status,
  IN UINT64                     DtbAddress
  )
{
  gMotoG20FoundationFailure.Signature  = MOTO_G20_FOUNDATION_FAILURE_SIGNATURE;
  gMotoG20FoundationFailure.Stage      = Stage;
  gMotoG20FoundationFailure.Status     = Status;
  gMotoG20FoundationFailure.DtbAddress = DtbAddress;
  gMotoG20FoundationFailure.SctlrEl2   = MotoG20FoundationReadSctlrEl2 ();
  ArmDataSynchronizationBarrier ();
  MotoG20FoundationDisplayFailure (
    (CONST MOTO_G20_FOUNDATION_FAILURE_RECORD *)&gMotoG20FoundationFailure
    );
  for (;;) {
    __asm__ volatile ("wfe");
  }
}

STATIC
EFI_STATUS
FOUNDATION_TEXT
MotoG20FoundationValidateBootstrapMap (
  VOID
  )
{
  MOTO_G20_TRANSLATION_LOOKUP  Result;
  EFI_STATUS                   Status;
  UINT64                       Descriptor;
  UINT64                       ImageEnd;
  UINT64                       RoundedEnd;
  UINT64                       Address;

  ImageEnd = (UINT64)(UINTN)__MotoG20FoundationImageEnd;
  if ((ImageEnd <= MOTO_G20_FOUNDATION_PE_BASE) ||
      (ImageEnd > (MOTO_G20_FOUNDATION_PE_BASE + MOTO_G20_FOUNDATION_PE_MAX_SIZE)) ||
      (ImageEnd > (MAX_UINT64 - (MOTO_G20_FOUNDATION_PAGE_SIZE - 1ULL)))) {
    return EFI_BAD_BUFFER_SIZE;
  }

  RoundedEnd = (ImageEnd + MOTO_G20_FOUNDATION_PAGE_SIZE - 1ULL) &
               ~(MOTO_G20_FOUNDATION_PAGE_SIZE - 1ULL);
  for (Address = MOTO_G20_FOUNDATION_PE_BASE;
       Address < RoundedEnd;
       Address += MOTO_G20_FOUNDATION_PAGE_SIZE) {
    Status = MotoG20LookupTranslation (
               (VOID *)(UINTN)MOTO_G20_FOUNDATION_TTBR0,
               Address,
               &Result
               );
    Descriptor = Result.Descriptor;
    if (EFI_ERROR (Status) ||
        !Result.Mapped ||
        (Result.PhysicalAddress != Address) ||
        (Result.Level < 2U) ||
        (Result.Level > 3U) ||
        ((Descriptor & TT_ATTR_INDX_MASK) != TT_ATTR_INDX_MEMORY_WRITE_BACK) ||
        ((Descriptor & TT_SH_MASK) != TT_SH_INNER_SHAREABLE) ||
        ((Descriptor & TT_AF) == 0) ||
        ((Descriptor & TT_AP_MASK) != TT_AP_NO_RW) ||
        ((Descriptor & TT_XN_MASK) != 0)) {
      return EFI_SECURITY_VIOLATION;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_HOB_GUID_TYPE *
FOUNDATION_TEXT
MotoG20FoundationFindLowGuidHob (
  VOID
  )
{
  EFI_HOB_HANDOFF_INFO_TABLE  *Phit;
  EFI_HOB_GENERIC_HEADER      *Hob;
  EFI_HOB_GUID_TYPE           *GuidHob;
  UINTN                        ArenaBase;
  UINTN                        ArenaEnd;
  UINTN                        EndOfList;
  UINTN                        HobAddress;
  UINTN                        NextAddress;

  ArenaBase = (UINTN)MOTO_G20_FOUNDATION_LOW_HOB_BASE;
  ArenaEnd  = ArenaBase + (UINTN)MOTO_G20_FOUNDATION_LOW_HOB_SIZE;
  Phit      = (EFI_HOB_HANDOFF_INFO_TABLE *)ArenaBase;
  if ((Phit->Header.HobType != EFI_HOB_TYPE_HANDOFF) ||
      (Phit->Header.HobLength != sizeof (*Phit)) ||
      (Phit->Version != EFI_HOB_HANDOFF_TABLE_VERSION) ||
      ((UINTN)Phit->EfiMemoryBottom != ArenaBase) ||
      ((UINTN)Phit->EfiMemoryTop != ArenaEnd)) {
    return NULL;
  }

  EndOfList = (UINTN)Phit->EfiEndOfHobList;
  if ((EndOfList < (ArenaBase + sizeof (*Phit))) ||
      (EndOfList > (ArenaEnd - sizeof (EFI_HOB_GENERIC_HEADER)))) {
    return NULL;
  }

  Hob = (EFI_HOB_GENERIC_HEADER *)(Phit + 1);
  for (;;) {
    HobAddress = (UINTN)Hob;
    if ((HobAddress > EndOfList) ||
        (Hob->HobLength < sizeof (*Hob)) ||
        ((Hob->HobLength & 7U) != 0) ||
        (HobAddress > (ArenaEnd - Hob->HobLength))) {
      return NULL;
    }

    if (Hob->HobType == EFI_HOB_TYPE_END_OF_HOB_LIST) {
      return NULL;
    }

    if ((Hob->HobType == EFI_HOB_TYPE_GUID_EXTENSION) &&
        (Hob->HobLength >=
         (sizeof (EFI_HOB_GUID_TYPE) + sizeof (MOTO_G20_FOUNDATION_LOW_HOB_PAYLOAD)))) {
      GuidHob = (EFI_HOB_GUID_TYPE *)Hob;
      if ((GuidHob->Name.Data1 == mMotoG20FoundationRealHobGuid.Data1) &&
          (GuidHob->Name.Data2 == mMotoG20FoundationRealHobGuid.Data2) &&
          (GuidHob->Name.Data3 == mMotoG20FoundationRealHobGuid.Data3) &&
          (CompareMem (
             GuidHob->Name.Data4,
             mMotoG20FoundationRealHobGuid.Data4,
             sizeof (GuidHob->Name.Data4)
             ) == 0)) {
        return GuidHob;
      }
    }

    NextAddress = HobAddress + Hob->HobLength;
    if ((NextAddress <= HobAddress) || (NextAddress > EndOfList)) {
      return NULL;
    }

    Hob = (EFI_HOB_GENERIC_HEADER *)NextAddress;
  }
}

/**
  Add execute-never to the bootloader-created framebuffer mappings before
  enabling the EL2 MMU.  Validate the complete table topology and every
  descriptor first so a mismatch cannot leave a partially edited live map.
**/
STATIC
EFI_STATUS
FOUNDATION_TEXT
MotoG20FoundationHardenFramebufferMap (
  OUT UINTN  *DescriptorWrites
  )
{
  volatile UINT64  *Root;
  volatile UINT64  *L1;
  volatile UINT64  *L2;
  volatile UINT64  *L3;
  UINT64            Descriptor;
  UINT64            ExpectedAddress;
  UINTN             Index;
  UINTN             L2FirstIndex;
  UINTN             L2TableIndex;

  if (DescriptorWrites == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *DescriptorWrites = 0;
  Root              = (volatile UINT64 *)(UINTN)MOTO_G20_FOUNDATION_TTBR0;
  L1                = (volatile UINT64 *)(UINTN)MOTO_G20_FOUNDATION_L1;
  L2                = (volatile UINT64 *)(UINTN)MOTO_G20_FOUNDATION_L2_80000000;
  L3                = (volatile UINT64 *)(UINTN)MOTO_G20_FOUNDATION_EXISTING_L3_FRAMEBUFFER;
  L2FirstIndex      = (UINTN)((MOTO_G20_FOUNDATION_FB_BASE >> 21) & 0x1FFULL);
  L2TableIndex      = L2FirstIndex + MOTO_G20_FOUNDATION_FB_L2_BLOCKS;

  Descriptor = Root[(MOTO_G20_FOUNDATION_FB_BASE >> 39) & 0x1FFULL];
  if (((Descriptor & TT_TYPE_MASK) != TT_TYPE_TABLE_ENTRY) ||
      ((Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE) != MOTO_G20_FOUNDATION_L1)) {
    return EFI_COMPROMISED_DATA;
  }

  Descriptor = L1[(MOTO_G20_FOUNDATION_FB_BASE >> 30) & 0x1FFULL];
  if (((Descriptor & TT_TYPE_MASK) != TT_TYPE_TABLE_ENTRY) ||
      ((Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE) !=
       MOTO_G20_FOUNDATION_L2_80000000)) {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = 0; Index < MOTO_G20_FOUNDATION_FB_L2_BLOCKS; Index++) {
    Descriptor      = L2[L2FirstIndex + Index];
    ExpectedAddress = MOTO_G20_FOUNDATION_FB_BASE + (Index << 21);
    if (((Descriptor & TT_TYPE_MASK) != TT_TYPE_BLOCK_ENTRY) ||
        ((Descriptor & TT_ADDRESS_MASK_BLOCK_ENTRY) != ExpectedAddress) ||
        ((Descriptor & TT_ATTR_INDX_MASK) != TT_ATTR_INDX_MEMORY_NON_CACHEABLE) ||
        ((Descriptor & TT_SH_MASK) != TT_SH_NON_SHAREABLE) ||
        ((Descriptor & TT_AP_MASK) != TT_AP_NO_RW) ||
        ((Descriptor & TT_AF) == 0) ||
        ((Descriptor & TT_XN_MASK) != 0)) {
      return EFI_SECURITY_VIOLATION;
    }
  }

  Descriptor = L2[L2TableIndex];
  if (((Descriptor & TT_TYPE_MASK) != TT_TYPE_TABLE_ENTRY) ||
      ((Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE) !=
       MOTO_G20_FOUNDATION_EXISTING_L3_FRAMEBUFFER)) {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = 0; Index < MOTO_G20_FOUNDATION_FB_L3_PAGES; Index++) {
    Descriptor      = L3[Index];
    ExpectedAddress = MOTO_G20_FOUNDATION_FB_BASE +
                      (MOTO_G20_FOUNDATION_FB_L2_BLOCKS << 21) +
                      (Index << 12);
    if (((Descriptor & TT_TYPE_MASK) != TT_TYPE_BLOCK_ENTRY_LEVEL3) ||
        ((Descriptor & TT_ADDRESS_MASK_BLOCK_ENTRY) != ExpectedAddress) ||
        ((Descriptor & TT_ATTR_INDX_MASK) != TT_ATTR_INDX_MEMORY_NON_CACHEABLE) ||
        ((Descriptor & TT_SH_MASK) != TT_SH_NON_SHAREABLE) ||
        ((Descriptor & TT_AP_MASK) != TT_AP_NO_RW) ||
        ((Descriptor & TT_AF) == 0) ||
        ((Descriptor & TT_XN_MASK) != 0)) {
      return EFI_SECURITY_VIOLATION;
    }
  }

  for (Index = MOTO_G20_FOUNDATION_FB_L3_PAGES; Index < TT_ENTRY_COUNT; Index++) {
    if (L3[Index] != 0) {
      return EFI_COMPROMISED_DATA;
    }
  }

  for (Index = 0; Index < MOTO_G20_FOUNDATION_FB_L2_BLOCKS; Index++) {
    L2[L2FirstIndex + Index] |= TT_XN_MASK;
    (*DescriptorWrites)++;
  }

  for (Index = 0; Index < MOTO_G20_FOUNDATION_FB_L3_PAGES; Index++) {
    L3[Index] |= TT_XN_MASK;
    (*DescriptorWrites)++;
  }

  ArmDataSynchronizationBarrier ();
  ArmInstructionSynchronizationBarrier ();
  return EFI_SUCCESS;
}

VOID
EFIAPI
FOUNDATION_TEXT
FOUNDATION_NORETURN
MotoG20Mmu3b1Probe (
  VOID
  )
{
  EFI_HOB_GUID_TYPE                    *GuidHob;
  MOTO_G20_FOUNDATION_LOW_HOB_PAYLOAD  *Payload;
  EFI_STATUS                            Status;
  UINT64                                DtbAddress;
  UINTN                                 DescriptorWrites;

  GuidHob = MotoG20FoundationFindLowGuidHob ();
  if (GuidHob == NULL) {
    MotoG20FoundationFailClosed (MotoG20FoundationStageFdt, EFI_NOT_FOUND, 0);
  }

  Payload = (MOTO_G20_FOUNDATION_LOW_HOB_PAYLOAD *)(GuidHob + 1);
  if ((Payload->MagicA != 0x1122334455667788ULL) ||
      (Payload->MagicB != 0xDEADBEEFA55A5AA5ULL) ||
      (Payload->PhysicalAddressBits != 40ULL)) {
    MotoG20FoundationFailClosed (MotoG20FoundationStageFdt, EFI_COMPROMISED_DATA, 0);
  }

  DtbAddress = Payload->DtbAddress;
  ProcessLibraryConstructorList ();
  Status = MotoG20FoundationPlanAndStageFv (
             DtbAddress,
             &mMotoG20FoundationExpectedFv,
             &mMotoG20FoundationPlan
             );
  if (EFI_ERROR (Status)) {
    MotoG20FoundationFailClosed (MotoG20FoundationStageFvCopy, Status, DtbAddress);
  }

  Status = MotoG20FoundationValidateBootstrapMap ();
  if (EFI_ERROR (Status)) {
    MotoG20FoundationFailClosed (MotoG20FoundationStageMmuProof, Status, DtbAddress);
  }

  Status = MotoG20FoundationHardenFramebufferMap (&DescriptorWrites);
  if (EFI_ERROR (Status) ||
      (DescriptorWrites != MOTO_G20_FOUNDATION_FB_DESCRIPTOR_WRITES)) {
    MotoG20FoundationFailClosed (
      MotoG20FoundationStageFramebufferMap,
      EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA,
      DtbAddress
      );
  }

  ArmDataSynchronizationBarrier ();
  MotoG20Mmu3b1ProbeFrozenTerminal ();
  MotoG20FoundationFailClosed (MotoG20FoundationStageMmuProof, EFI_DEVICE_ERROR, DtbAddress);
}

VOID
EFIAPI
FOUNDATION_TEXT
FOUNDATION_NORETURN
MotoG20FoundationContinuePostMmu (
  VOID
  )
{
  MOTO_G20_FOUNDATION_MAP_RESULT  MapResult;
  MOTO_G20_FOUNDATION_STAGE       DxeStage;
  EFI_STATUS                      Status;
  UINT64                          DtbAddress;
  UINT64                          Sctlr;

  DtbAddress = mMotoG20FoundationPlan.DtbAddress;
  Sctlr     = MotoG20FoundationReadSctlrEl2 ();
  if ((DtbAddress != MOTO_G20_FOUNDATION_DTB_ADDRESS) ||
      ((Sctlr & MOTO_G20_FOUNDATION_MMU_BITS) != BIT0)) {
    MotoG20FoundationFailClosed (
      MotoG20FoundationStageMmuProof,
      EFI_SECURITY_VIOLATION,
      DtbAddress
      );
  }

  MotoG20FoundationDisplayBegin (DtbAddress, Sctlr);
  MotoG20FoundationDisplayProgress (
    MotoG20FoundationStageMmuProof,
    EFI_SUCCESS,
    Sctlr
    );

  MotoG20FoundationDisplayProgress (
    MotoG20FoundationStageHighMap,
    EFI_NOT_READY,
    0
    );
  Status = MotoG20FoundationAppendHighMap (&MapResult);
  if (EFI_ERROR (Status) || (MapResult.TotalDescriptorWrites != 794U)) {
    MotoG20FoundationFailClosed (
      MotoG20FoundationStageHighMap,
      EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA,
      DtbAddress
      );
  }

  MotoG20FoundationDisplayProgress (
    MotoG20FoundationStageHighMap,
    EFI_SUCCESS,
    MapResult.TotalDescriptorWrites
    );

  Status = MotoG20FoundationEnableDataCacheAndTestExclusive (&Sctlr);
  if (EFI_ERROR (Status)) {
    MotoG20FoundationFailClosed (
      MotoG20FoundationStageMmuProof,
      Status,
      DtbAddress
      );
  }

  MotoG20FoundationDisplayProgress (
    MotoG20FoundationStageMmuProof,
    EFI_SUCCESS,
    Sctlr
    );

  DxeStage = MotoG20FoundationStageDxeMemoryClear;
  Status = MotoG20FoundationBuildHobAndLoadDxe (
             &mMotoG20FoundationPlan,
             &DxeStage
             );
  MotoG20FoundationFailClosed (DxeStage, Status, DtbAddress);
}

#include <Base.h>
#include <AArch64Mmu.h>
#include <Library/MotoG20PostMmuTelemetryLib.h>

#define MOTO_G20_SCTLR_M                 0x0000000000000001ULL
#define MOTO_G20_PAGE_SIZE               0x0000000000001000ULL
#define MOTO_G20_PAGE_MASK               (MOTO_G20_PAGE_SIZE - 1U)
#define MOTO_G20_TABLE_INDEX_MASK        0x1FFULL
#define MOTO_G20_ADDRESS_MASK            0x000000FFFFFFFFFFULL
#define MOTO_G20_RAMOOPS_END             \
  (MOTO_G20_RAMOOPS_BASE + MOTO_G20_RAMOOPS_SIZE)

typedef enum {
  MotoG20StrictLookupInvalid,
  MotoG20StrictLookupUnmapped,
  MotoG20StrictLookupMapped
} MOTO_G20_STRICT_LOOKUP_STATE;

typedef struct {
  MOTO_G20_STRICT_LOOKUP_STATE  State;
  UINTN                         Level;
  UINT64                        Descriptor;
  UINT64                        PhysicalAddress;
} MOTO_G20_STRICT_LOOKUP;

STATIC CONST UINT8  mMotoG20RamoopsKmsgHeader[8] = {
  '=', '=', '=', '=', '0', '.', '0', '\n'
};

STATIC
BOOLEAN
MotoG20PostMmuTableAddressAllowed (
  IN UINT64  Address
  )
{
  return (BOOLEAN)(
           (Address >= MOTO_G20_POST_MMU_TABLE_BASE) &&
           (Address < MOTO_G20_POST_MMU_TABLE_LIMIT) &&
           ((Address & MOTO_G20_PAGE_MASK) == 0)
           );
}

STATIC
UINTN
MotoG20PostMmuLevelShift (
  IN UINTN  Level
  )
{
  return 12U + ((3U - Level) * 9U);
}

STATIC
VOID
MotoG20PostMmuStrictLookup (
  IN  CONST VOID                 *TranslationTableBase,
  IN  UINT64                     VirtualAddress,
  OUT MOTO_G20_STRICT_LOOKUP     *Result
  )
{
  volatile CONST UINT64  *Table;
  UINT64                 Descriptor;
  UINT64                 TableAddress;
  UINT64                 BlockSize;
  UINT64                 BlockMask;
  UINTN                  Index;
  UINTN                  Level;

  Result->State           = MotoG20StrictLookupInvalid;
  Result->Level           = 0;
  Result->Descriptor      = 0;
  Result->PhysicalAddress = 0;
  if (((UINT64)(UINTN)TranslationTableBase !=
       MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2) ||
      ((VirtualAddress & ~MOTO_G20_ADDRESS_MASK) != 0)) {
    return;
  }

  Table = (volatile CONST UINT64 *)TranslationTableBase;
  for (Level = 0; Level < 4; Level++) {
    Index = (UINTN)(
                     (VirtualAddress >> MotoG20PostMmuLevelShift (Level)) &
                     MOTO_G20_TABLE_INDEX_MASK
                     );
    Descriptor       = Table[Index];
    Result->Level      = Level;
    Result->Descriptor = Descriptor;
    if (Descriptor == 0) {
      Result->State = MotoG20StrictLookupUnmapped;
      return;
    }

    if (Level == 3) {
      if ((Descriptor & TT_TYPE_MASK) != TT_TYPE_BLOCK_ENTRY_LEVEL3) {
        return;
      }

      Result->State = MotoG20StrictLookupMapped;
      Result->PhysicalAddress =
        (Descriptor & TT_ADDRESS_MASK_BLOCK_ENTRY) |
        (VirtualAddress & MOTO_G20_PAGE_MASK);
      return;
    }

    if ((Descriptor & TT_TYPE_MASK) == TT_TYPE_TABLE_ENTRY) {
      TableAddress = Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE;
      if (!MotoG20PostMmuTableAddressAllowed (TableAddress)) {
        return;
      }

      Table = (volatile CONST UINT64 *)(UINTN)TableAddress;
      continue;
    }

    if ((Level == 0) ||
        ((Descriptor & TT_TYPE_MASK) != TT_TYPE_BLOCK_ENTRY)) {
      return;
    }

    BlockSize = 1ULL << MotoG20PostMmuLevelShift (Level);
    BlockMask = BlockSize - 1U;
    Result->State = MotoG20StrictLookupMapped;
    Result->PhysicalAddress =
      (Descriptor & TT_ADDRESS_MASK_BLOCK_ENTRY & ~BlockMask) |
      (VirtualAddress & BlockMask);
    return;
  }
}

BOOLEAN
EFIAPI
MotoG20TelemetryV2ValidateActivePostMmuMap (
  IN CONST VOID  *TranslationTableBase,
  IN UINT64      SctlrEl2,
  IN UINT64      TcrEl2,
  IN UINT64      Ttbr0El2,
  IN UINT64      MairEl2
  )
{
  MOTO_G20_STRICT_LOOKUP  Lookup;
  UINT64                  Address;
  UINT64                  RequiredAttributes;

  if (((SctlrEl2 != MOTO_G20_POST_MMU_EXPECTED_SCTLR_M_ONLY) &&
       (SctlrEl2 != MOTO_G20_POST_MMU_EXPECTED_SCTLR_M_AND_C)) ||
      (TcrEl2 != MOTO_G20_POST_MMU_EXPECTED_TCR_EL2) ||
      (Ttbr0El2 != MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2) ||
      (MairEl2 != MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2) ||
      ((UINT64)(UINTN)TranslationTableBase !=
       MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2)) {
    return FALSE;
  }

  RequiredAttributes =
    TT_ATTR_INDX_DEVICE_MEMORY |
    TT_AP_NO_RW |
    TT_SH_NON_SHAREABLE |
    TT_AF |
    TT_XN_MASK;
  for (
    Address = MOTO_G20_RAMOOPS_BASE;
    Address < MOTO_G20_RAMOOPS_END;
    Address += MOTO_G20_PAGE_SIZE
    ) {
    MotoG20PostMmuStrictLookup (
      TranslationTableBase,
      Address,
      &Lookup
      );
    if ((Lookup.State != MotoG20StrictLookupMapped) ||
        (Lookup.Level != 3) ||
        (Lookup.PhysicalAddress != Address) ||
        ((Lookup.Descriptor & TT_ATTRIBUTES_MASK)
         != RequiredAttributes)) {
      return FALSE;
    }
  }

  MotoG20PostMmuStrictLookup (
    TranslationTableBase,
    MOTO_G20_RAMOOPS_BASE - MOTO_G20_PAGE_SIZE,
    &Lookup
    );
  if (Lookup.State != MotoG20StrictLookupUnmapped) {
    return FALSE;
  }

  MotoG20PostMmuStrictLookup (
    TranslationTableBase,
    MOTO_G20_RAMOOPS_END,
    &Lookup
    );
  return (BOOLEAN)(Lookup.State == MotoG20StrictLookupUnmapped);
}

STATIC
VOID
MotoG20PostMmuBarrier (
  VOID
  )
{
#if defined (MOTO_G20_POST_MMU_TELEMETRY_HOST_TEST)
  __asm__ __volatile__ ("" ::: "memory");
#else
  __asm__ __volatile__ ("dsb sy" ::: "memory");
#endif
}

STATIC
VOID
MotoG20PostMmuWriteUint32 (
  OUT volatile UINT8  *Address,
  IN  UINT32          Value
  )
{
  *(volatile UINT32 *)(VOID *)Address = Value;
}

STATIC
UINT32
MotoG20PostMmuReadUint32 (
  IN volatile CONST UINT8  *Address
  )
{
  return *(volatile CONST UINT32 *)(CONST VOID *)Address;
}

STATIC
VOID
MotoG20PostMmuCopyWords (
  OUT volatile UINT8  *Destination,
  IN  CONST UINT8     *Source,
  IN  UINTN           Size
  )
{
  UINT32  Value;
  UINTN   Index;

  for (Index = 0; Index < Size; Index += 4U) {
    Value = (UINT32)Source[Index] |
            ((UINT32)Source[Index + 1U] << 8) |
            ((UINT32)Source[Index + 2U] << 16) |
            ((UINT32)Source[Index + 3U] << 24);
    MotoG20PostMmuWriteUint32 (Destination + Index, Value);
  }
}

STATIC
BOOLEAN
MotoG20PostMmuBytesMatch (
  IN volatile CONST UINT8  *Stored,
  IN CONST UINT8           *Expected,
  IN UINTN                 Size
  )
{
  UINTN  Index;

  for (Index = 0; Index < Size; Index++) {
    if (Stored[Index] != Expected[Index]) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
BOOLEAN
MotoG20TelemetryV2CommitMappedToZone (
  IN OUT volatile UINT8  *Zone,
  IN     UINTN           ZoneSize,
  IN     CONST UINT8     Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE],
  IN     CONST VOID      *TranslationTableBase,
  IN     UINT64          SctlrEl2,
  IN     UINT64          TcrEl2,
  IN     UINT64          Ttbr0El2,
  IN     UINT64          MairEl2,
  IN     UINT32          StopAfterStage
  )
{
  if (((UINT64)(UINTN)Zone != MOTO_G20_RAMOOPS_TELEMETRY_ZONE_BASE) ||
      (ZoneSize != MOTO_G20_RAMOOPS_RECORD_SIZE) ||
      (Frame == NULL) ||
      (StopAfterStage > MOTO_G20_TELEMETRY_COMMIT_AFTER_SIGNATURE) ||
      !MotoG20TelemetryV2ValidateFrame (Frame) ||
      !MotoG20TelemetryV2ValidateActivePostMmuMap (
         TranslationTableBase,
         SctlrEl2,
         TcrEl2,
         Ttbr0El2,
         MairEl2
         )) {
    return FALSE;
  }

  // Signature invalidation is the first store in every accepted transaction.
  MotoG20PostMmuWriteUint32 (Zone, 0U);
  MotoG20PostMmuBarrier ();
  if (StopAfterStage == MOTO_G20_TELEMETRY_COMMIT_AFTER_INVALID) {
    return TRUE;
  }

  // Device memory receives naturally aligned 32-bit stores only.
  MotoG20PostMmuCopyWords (
    Zone + 12U,
    mMotoG20RamoopsKmsgHeader,
    sizeof (mMotoG20RamoopsKmsgHeader)
    );
  MotoG20PostMmuCopyWords (
    Zone + 20U,
    Frame,
    MOTO_G20_TELEMETRY_V2_FRAME_SIZE
    );
  MotoG20PostMmuBarrier ();
  if (StopAfterStage == MOTO_G20_TELEMETRY_COMMIT_AFTER_PAYLOAD) {
    return TRUE;
  }

  MotoG20PostMmuWriteUint32 (
    Zone + 4U,
    MOTO_G20_RAMOOPS_RECORD_PAYLOAD_SIZE
    );
  MotoG20PostMmuWriteUint32 (
    Zone + 8U,
    MOTO_G20_RAMOOPS_RECORD_PAYLOAD_SIZE
    );
  MotoG20PostMmuBarrier ();
  if (StopAfterStage == MOTO_G20_TELEMETRY_COMMIT_AFTER_GEOMETRY) {
    return TRUE;
  }

  // Read back every committed field and revalidate the active map before the
  // persistent signature makes the record visible to a collector.
  if ((MotoG20PostMmuReadUint32 (Zone) != 0U) ||
      (MotoG20PostMmuReadUint32 (Zone + 4U) !=
       MOTO_G20_RAMOOPS_RECORD_PAYLOAD_SIZE) ||
      (MotoG20PostMmuReadUint32 (Zone + 8U) !=
       MOTO_G20_RAMOOPS_RECORD_PAYLOAD_SIZE) ||
      !MotoG20PostMmuBytesMatch (
         Zone + 12U,
         mMotoG20RamoopsKmsgHeader,
         sizeof (mMotoG20RamoopsKmsgHeader)
         ) ||
      !MotoG20PostMmuBytesMatch (
         Zone + 20U,
         Frame,
         MOTO_G20_TELEMETRY_V2_FRAME_SIZE
         ) ||
      !MotoG20TelemetryV2ValidateActivePostMmuMap (
         TranslationTableBase,
         SctlrEl2,
         TcrEl2,
         Ttbr0El2,
         MairEl2
         )) {
    return FALSE;
  }

  // The persistent signature is the sole acceptance marker and is last.
  MotoG20PostMmuWriteUint32 (
    Zone,
    MOTO_G20_RAMOOPS_PERSISTENT_SIGNATURE
    );
  MotoG20PostMmuBarrier ();
  return TRUE;
}

#if !defined (MOTO_G20_POST_MMU_TELEMETRY_HOST_TEST)
STATIC
UINT64
MotoG20PostMmuReadCurrentEl (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, CurrentEL" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20PostMmuReadSctlrEl2 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, SCTLR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20PostMmuReadTcrEl2 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, TCR_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20PostMmuReadTtbr0El2 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, TTBR0_EL2" : "=r" (Value));
  return Value;
}

STATIC
UINT64
MotoG20PostMmuReadMairEl2 (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, MAIR_EL2" : "=r" (Value));
  return Value;
}
#endif

BOOLEAN
EFIAPI
MotoG20TelemetryV2CommitPostMmuFrame (
  IN CONST UINT8  Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  )
{
#if defined (MOTO_G20_POST_MMU_TELEMETRY_HOST_TEST)
  // A host build must never dereference the fixed physical ramoops address.
  (VOID)Frame;
  return FALSE;
#else
  UINT64  CurrentEl;
  UINT64  SctlrEl2;
  UINT64  TcrEl2;
  UINT64  Ttbr0El2;
  UINT64  MairEl2;

  __asm__ __volatile__ ("isb" ::: "memory");
  CurrentEl = MotoG20PostMmuReadCurrentEl ();
  if ((CurrentEl & 0xCU) != 0x8U) {
    return FALSE;
  }

  SctlrEl2 = MotoG20PostMmuReadSctlrEl2 ();
  TcrEl2   = MotoG20PostMmuReadTcrEl2 ();
  Ttbr0El2 = MotoG20PostMmuReadTtbr0El2 ();
  MairEl2  = MotoG20PostMmuReadMairEl2 ();
  return MotoG20TelemetryV2CommitMappedToZone (
           (volatile UINT8 *)(UINTN)MOTO_G20_RAMOOPS_TELEMETRY_ZONE_BASE,
           MOTO_G20_RAMOOPS_RECORD_SIZE,
           Frame,
           (CONST VOID *)(UINTN)MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           SctlrEl2,
           TcrEl2,
           Ttbr0El2,
           MairEl2,
           MOTO_G20_TELEMETRY_COMMIT_COMPLETE
           );
#endif
}

BOOLEAN
EFIAPI
MotoG20TelemetryV2PublishPostMmu (
  IN  CONST MOTO_G20_TELEMETRY_V2_INPUT  *Input,
  OUT UINT8                              FrameScratch[MOTO_G20_TELEMETRY_V2_FRAME_SIZE]
  )
{
  if (!MotoG20TelemetryV2Encode (Input, FrameScratch)) {
    return FALSE;
  }

  return MotoG20TelemetryV2CommitPostMmuFrame (FrameScratch);
}

#if defined (MOTO_G20_POST_MMU_TELEMETRY_HOST_TEST)
BOOLEAN
EFIAPI
MotoG20TelemetryV2HostCommitPostMmuToZone (
  IN OUT volatile UINT8  *Zone,
  IN     UINTN           ZoneSize,
  IN     CONST UINT8     Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE],
  IN     CONST VOID      *TranslationTableBase,
  IN     UINT64          SimulatedSctlrEl2,
  IN     UINT64          SimulatedTcrEl2,
  IN     UINT64          SimulatedTtbr0El2,
  IN     UINT64          SimulatedMairEl2,
  IN     UINT32          StopAfterStage
  )
{
  return MotoG20TelemetryV2CommitMappedToZone (
           Zone,
           ZoneSize,
           Frame,
           TranslationTableBase,
           SimulatedSctlrEl2,
           SimulatedTcrEl2,
           SimulatedTtbr0El2,
           SimulatedMairEl2,
           StopAfterStage
           );
}
#endif

#define _GNU_SOURCE

#include <AArch64Mmu.h>
#include <Library/MotoG20PostMmuTelemetryLib.h>
#include <Library/MotoG20TranslationTableLib.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE_SIZE                   0x1000ULL
#define WORKSPACE_BASE              0x81882000ULL
#define WORKSPACE_END               0x81982000ULL
#define WORKSPACE_SIZE              0x00100000ULL
#define EXPECTED_TABLE_PAGES        8U
#define EXPECTED_FINAL_TOP          0x8197A000ULL
#define RAMOOPS_END                 \
  (MOTO_G20_RAMOOPS_BASE + MOTO_G20_RAMOOPS_SIZE)
#define VALID_SCTLR_M_ONLY          MOTO_G20_POST_MMU_EXPECTED_SCTLR_M_ONLY
#define VALID_SCTLR_M_AND_C         MOTO_G20_POST_MMU_EXPECTED_SCTLR_M_AND_C
#define MAX_TABLE_PAGES             32U

static unsigned char *gWorkspace;
static UINT64 gTop = WORKSPACE_END;
static UINTN gAllocCount;
static UINTN gPassed;
static UINTN gTotal;

VOID *EFIAPI
AllocatePages (IN UINTN Pages)
{
  UINT64 NewTop;

  if ((Pages != 1U) || (gAllocCount >= MAX_TABLE_PAGES)) {
    return NULL;
  }

  NewTop = gTop - PAGE_SIZE;
  if (NewTop < WORKSPACE_BASE) {
    return NULL;
  }

  gTop = NewTop;
  memset ((void *)(uintptr_t)gTop, 0xA5, PAGE_SIZE);
  gAllocCount++;
  return (VOID *)(UINTN)gTop;
}

VOID *EFIAPI
ZeroMem (OUT VOID *Buffer, IN UINTN Length)
{
  return memset (Buffer, 0, Length);
}

static void
Check (int Condition, const char *Name)
{
  gTotal++;
  if (Condition) {
    gPassed++;
    return;
  }

  fprintf (stderr, "FAIL: %s\n", Name);
}

static ARM_MEMORY_REGION_DESCRIPTOR gMemoryTable[] = {
  { 0x80000000ULL, 0x80000000ULL, 0x01FF0000ULL,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK },
  { 0x82000000ULL, 0x82000000ULL, 0x00A0B000ULL,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK },
  { 0x9E000000ULL, 0x9E000000ULL, 0x009E4000ULL,
    ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED },
  { MOTO_G20_RAMOOPS_BASE, MOTO_G20_RAMOOPS_BASE,
    MOTO_G20_RAMOOPS_SIZE, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE },
  { 0, 0, 0, 0 }
};

static UINTN
LevelShift (UINTN Level)
{
  return 12U + ((3U - Level) * 9U);
}

static volatile UINT64 *
FindDescriptor (VOID *Root, UINT64 VirtualAddress, UINTN WantedLevel)
{
  volatile UINT64 *Table;
  volatile UINT64 *Entry;
  UINT64 Descriptor;
  UINTN Index;
  UINTN Level;

  Table = (volatile UINT64 *)Root;
  for (Level = 0; Level <= WantedLevel; Level++) {
    Index = (UINTN)((VirtualAddress >> LevelShift (Level)) & 0x1FFU);
    Entry = &Table[Index];
    if (Level == WantedLevel) {
      return Entry;
    }

    Descriptor = *Entry;
    if ((Descriptor & TT_TYPE_MASK) != TT_TYPE_TABLE_ENTRY) {
      return NULL;
    }

    Table = (volatile UINT64 *)(UINTN)(
                                      Descriptor &
                                      TT_ADDRESS_MASK_DESCRIPTION_TABLE
                                      );
  }

  return NULL;
}

static UINT32
ReadUint32 (volatile const UINT8 *Address)
{
  return *(volatile const UINT32 *)(const void *)Address;
}

static int
BytesEqualVolatile (
  volatile const UINT8 *Left,
  const UINT8 *Right,
  UINTN Size
  )
{
  UINTN Index;

  for (Index = 0; Index < Size; Index++) {
    if (Left[Index] != Right[Index]) {
      return 0;
    }
  }

  return 1;
}

static void
ResetZone (volatile UINT8 *Zone, UINT8 Pattern)
{
  UINTN Index;

  for (Index = 0; Index < MOTO_G20_RAMOOPS_RECORD_SIZE; Index++) {
    Zone[Index] = Pattern;
  }
}

static int
MapValid (VOID *Root, UINT64 Sctlr)
{
  return MotoG20TelemetryV2ValidateActivePostMmuMap (
           Root,
           Sctlr,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2
           );
}

static int
Commit (
  volatile UINT8 *Zone,
  const UINT8 *Frame,
  VOID *Root,
  UINT64 Sctlr,
  UINT64 Tcr,
  UINT64 Ttbr0,
  UINT64 Mair,
  UINT32 StopAfterStage
  )
{
  return MotoG20TelemetryV2HostCommitPostMmuToZone (
           Zone,
           MOTO_G20_RAMOOPS_RECORD_SIZE,
           Frame,
           Root,
           Sctlr,
           Tcr,
           Ttbr0,
           Mair,
           StopAfterStage
           );
}

static void
BuildFrame (UINT8 Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE])
{
  MOTO_G20_TELEMETRY_V2_INPUT Input;
  const char BuildId[] = "MMU3C2E";
  UINTN Index;

  memset (&Input, 0, sizeof (Input));
  for (Index = 0; Index < MOTO_G20_TELEMETRY_V2_SHA256_SIZE; Index++) {
    Input.CanonicalCandidateSha256[Index] = (UINT8)(Index + 1U);
  }
  memcpy (Input.BuildId, BuildId, sizeof (BuildId) - 1U);
  Input.BuildIdLength   = sizeof (BuildId) - 1U;
  Input.Sequence        = 0x0102030405060708ULL;
  Input.AttemptNumber   = 2U;
  Input.Stage           = 0x032EU;
  Input.Phase           = 4U;
  Input.FailureCode     = 0U;
  Input.CheckpointBitmap = 0x000000000000000FULL;
  Input.CurrentEl       = 8U;
  Input.SctlrEl2        = VALID_SCTLR_M_ONLY;
  Input.TcrEl2          = MOTO_G20_POST_MMU_EXPECTED_TCR_EL2;
  Input.Ttbr0El2        = MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2;
  Input.MairEl2         = MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2;
  Input.VbarEl2         = 0x81004000ULL;
  Input.StackPointer    = 0x811FFFC0ULL;
  Input.MonotonicTicks  = 0x1122334455667788ULL;
  Check (MotoG20TelemetryV2Encode (&Input, Frame), "frame encode");
  Check (MotoG20TelemetryV2ValidateFrame (Frame), "frame validate");
}

int
main (void)
{
  VOID *Root;
  VOID *WorkspaceMap;
  VOID *ZoneMap;
  volatile UINT8 *Zone;
  volatile UINT64 *Entry;
  UINT8 Frame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE];
  UINT8 SavedZone[MOTO_G20_RAMOOPS_RECORD_SIZE];
  UINT8 TailPattern[MOTO_G20_RAMOOPS_RECORD_SIZE - 220U];
  UINT8 InvalidFrame[MOTO_G20_TELEMETRY_V2_FRAME_SIZE];
  UINT8 AlternateZone[MOTO_G20_RAMOOPS_RECORD_SIZE];
  UINT64 SavedDescriptor;
  UINT64 Address;
  UINTN TableSize;
  UINTN AllocatedPages;
  UINTN Index;
  EFI_STATUS Status;
  static const UINT8 KmsgHeader[8] = {
    '=', '=', '=', '=', '0', '.', '0', '\n'
  };

  gPassed = 0;
  gTotal = 0;
  WorkspaceMap = mmap (
                   (void *)(uintptr_t)WORKSPACE_BASE,
                   WORKSPACE_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                   -1,
                   0
                   );
  if (WorkspaceMap == MAP_FAILED) {
    perror ("mmap workspace");
    return 2;
  }
  gWorkspace = WorkspaceMap;
  memset (gWorkspace, 0xCC, WORKSPACE_SIZE);

  ZoneMap = mmap (
              (void *)(uintptr_t)MOTO_G20_RAMOOPS_TELEMETRY_ZONE_BASE,
              MOTO_G20_RAMOOPS_RECORD_SIZE,
              PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
              -1,
              0
              );
  if (ZoneMap == MAP_FAILED) {
    perror ("mmap zone");
    munmap (WorkspaceMap, WORKSPACE_SIZE);
    return 2;
  }
  Zone = (volatile UINT8 *)ZoneMap;

  Root = NULL;
  TableSize = 0;
  AllocatedPages = 0;
  Status = MotoG20BuildTranslationTables (
             gMemoryTable,
             &Root,
             &TableSize,
             &AllocatedPages
             );
  Check (!EFI_ERROR (Status), "translation build status");
  Check ((UINT64)(UINTN)Root == MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
         "exact root");
  Check (TableSize == sizeof (UINT64) * 2U, "root size");
  Check (AllocatedPages == EXPECTED_TABLE_PAGES, "eight table pages");
  Check (gTop == EXPECTED_FINAL_TOP, "exact final top");

  Check (MapValid (Root, VALID_SCTLR_M_ONLY), "M-only map valid");
  Check (MapValid (Root, VALID_SCTLR_M_AND_C), "M+C map valid");
  Check (!MapValid (Root, VALID_SCTLR_M_ONLY & ~1ULL), "M=0 rejected");
  Check (!MapValid (Root, VALID_SCTLR_M_ONLY | (1ULL << 25)),
         "unexpected SCTLR state rejected");
  Check (!MotoG20TelemetryV2ValidateActivePostMmuMap (
           Root,
           VALID_SCTLR_M_ONLY,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2 ^ 1ULL,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2
           ), "TCR mismatch rejected");
  Check (!MotoG20TelemetryV2ValidateActivePostMmuMap (
           Root,
           VALID_SCTLR_M_ONLY,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2 + PAGE_SIZE,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2
           ), "TTBR mismatch rejected");
  Check (!MotoG20TelemetryV2ValidateActivePostMmuMap (
           Root,
           VALID_SCTLR_M_ONLY,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2 ^ 0x100ULL
           ), "MAIR mismatch rejected");
  Check (!MotoG20TelemetryV2ValidateActivePostMmuMap (
           NULL,
           VALID_SCTLR_M_ONLY,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2
           ), "null root rejected");

  // Every ramoops leaf is mandatory.  Removing any one of all 64 pages must
  // fail the complete-map precondition.
  for (Address = MOTO_G20_RAMOOPS_BASE;
       Address < RAMOOPS_END;
       Address += PAGE_SIZE) {
    Entry = FindDescriptor (Root, Address, 3);
    Check (Entry != NULL, "ramoops leaf located");
    if (Entry == NULL) {
      continue;
    }
    SavedDescriptor = *Entry;
    *Entry = 0;
    Check (!MapValid (Root, VALID_SCTLR_M_ONLY),
           "missing ramoops leaf rejected");
    *Entry = SavedDescriptor;
  }

  Entry = FindDescriptor (Root, MOTO_G20_RAMOOPS_TELEMETRY_ZONE_BASE, 3);
  Check (Entry != NULL, "zone leaf located");
  if (Entry != NULL) {
    SavedDescriptor = *Entry;
    *Entry = SavedDescriptor & ~TT_XN_MASK;
    Check (!MapValid (Root, VALID_SCTLR_M_ONLY), "executable leaf rejected");
    *Entry = (SavedDescriptor & ~TT_ATTR_INDX_MASK) |
             TT_ATTR_INDX_MEMORY_WRITE_BACK;
    Check (!MapValid (Root, VALID_SCTLR_M_ONLY), "cacheable leaf rejected");
    *Entry = (SavedDescriptor & ~TT_AP_MASK) | TT_AP_NO_RO;
    Check (!MapValid (Root, VALID_SCTLR_M_ONLY), "read-only leaf rejected");
    *Entry = (SavedDescriptor & ~TT_ADDRESS_MASK_BLOCK_ENTRY) |
             ((MOTO_G20_RAMOOPS_TELEMETRY_ZONE_BASE + PAGE_SIZE) &
              TT_ADDRESS_MASK_BLOCK_ENTRY);
    Check (!MapValid (Root, VALID_SCTLR_M_ONLY), "non-identity leaf rejected");
    *Entry = SavedDescriptor;
  }

  Entry = FindDescriptor (Root, MOTO_G20_RAMOOPS_BASE - PAGE_SIZE, 3);
  Check ((Entry != NULL) && (*Entry == 0), "lower guard initially unmapped");
  if (Entry != NULL) {
    *Entry = (MOTO_G20_RAMOOPS_BASE - PAGE_SIZE) |
             TT_TYPE_BLOCK_ENTRY_LEVEL3 | TT_AF | TT_XN_MASK;
    Check (!MapValid (Root, VALID_SCTLR_M_ONLY), "mapped lower guard rejected");
    *Entry = 0;
  }
  Entry = FindDescriptor (Root, RAMOOPS_END, 3);
  Check ((Entry != NULL) && (*Entry == 0), "upper guard initially unmapped");
  if (Entry != NULL) {
    *Entry = RAMOOPS_END | TT_TYPE_BLOCK_ENTRY_LEVEL3 | TT_AF | TT_XN_MASK;
    Check (!MapValid (Root, VALID_SCTLR_M_ONLY), "mapped upper guard rejected");
    *Entry = 0;
  }

  Entry = FindDescriptor (Root, MOTO_G20_RAMOOPS_BASE, 2);
  Check (Entry != NULL, "ramoops L2 descriptor located");
  if (Entry != NULL) {
    SavedDescriptor = *Entry;
    *Entry = 0x81969000ULL | TT_TYPE_TABLE_ENTRY;
    Check (!MapValid (Root, VALID_SCTLR_M_ONLY),
           "out-of-envelope child table rejected without dereference");
    *Entry = SavedDescriptor;
  }
  Check (MapValid (Root, VALID_SCTLR_M_ONLY), "map restored after mutations");

  BuildFrame (Frame);
  memset (TailPattern, 0xA6, sizeof (TailPattern));
  ResetZone (Zone, 0xA6);
  Check (Commit (
           Zone, Frame, Root, VALID_SCTLR_M_ONLY,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2,
           MOTO_G20_TELEMETRY_COMMIT_COMPLETE
           ), "complete M-only commit");
  Check (ReadUint32 (Zone) == MOTO_G20_RAMOOPS_PERSISTENT_SIGNATURE,
         "signature committed");
  Check (ReadUint32 (Zone + 4U) == MOTO_G20_RAMOOPS_RECORD_PAYLOAD_SIZE,
         "start geometry");
  Check (ReadUint32 (Zone + 8U) == MOTO_G20_RAMOOPS_RECORD_PAYLOAD_SIZE,
         "size geometry");
  Check (BytesEqualVolatile (Zone + 12U, KmsgHeader, sizeof (KmsgHeader)),
         "kmsg header exact");
  Check (BytesEqualVolatile (
           Zone + 20U, Frame, MOTO_G20_TELEMETRY_V2_FRAME_SIZE
           ), "frame exact");
  Check (BytesEqualVolatile (
           Zone + 220U, TailPattern, sizeof (TailPattern)
           ), "tail preserved");

  ResetZone (Zone, 0xA6);
  Check (Commit (
           Zone, Frame, Root, VALID_SCTLR_M_AND_C,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2,
           MOTO_G20_TELEMETRY_COMMIT_COMPLETE
           ), "complete M+C commit over Device mapping");
  Check (ReadUint32 (Zone) == MOTO_G20_RAMOOPS_PERSISTENT_SIGNATURE,
         "M+C signature committed");

  for (Index = MOTO_G20_TELEMETRY_COMMIT_AFTER_INVALID;
       Index <= MOTO_G20_TELEMETRY_COMMIT_AFTER_GEOMETRY;
       Index++) {
    ResetZone (Zone, 0x5A);
    Check (Commit (
             Zone, Frame, Root, VALID_SCTLR_M_ONLY,
             MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
             MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
             MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2,
             (UINT32)Index
             ), "partial transaction reached requested boundary");
    Check (ReadUint32 (Zone) == 0U,
           "partial transaction remains uncommitted");
  }
  ResetZone (Zone, 0x5A);
  Check (Commit (
           Zone, Frame, Root, VALID_SCTLR_M_ONLY,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2,
           MOTO_G20_TELEMETRY_COMMIT_AFTER_SIGNATURE
           ), "signature boundary completes transaction");
  Check (ReadUint32 (Zone) == MOTO_G20_RAMOOPS_PERSISTENT_SIGNATURE,
         "signature boundary is accepted");

  // Every invalid precondition must reject before the first write.
  ResetZone (Zone, 0x3C);
  for (Index = 0; Index < sizeof (SavedZone); Index++) {
    SavedZone[Index] = Zone[Index];
  }
  Check (!Commit (
           Zone, Frame, Root, VALID_SCTLR_M_ONLY & ~1ULL,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2,
           MOTO_G20_TELEMETRY_COMMIT_COMPLETE
           ), "M=0 commit rejected");
  Check (BytesEqualVolatile (Zone, SavedZone, sizeof (SavedZone)),
         "M=0 rejection write-free");

  memcpy (InvalidFrame, Frame, sizeof (InvalidFrame));
  InvalidFrame[80] ^= 1U;
  Check (!Commit (
           Zone, InvalidFrame, Root, VALID_SCTLR_M_ONLY,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2,
           MOTO_G20_TELEMETRY_COMMIT_COMPLETE
           ), "invalid frame rejected");
  Check (BytesEqualVolatile (Zone, SavedZone, sizeof (SavedZone)),
         "invalid frame rejection write-free");

  Check (!MotoG20TelemetryV2HostCommitPostMmuToZone (
           Zone,
           MOTO_G20_RAMOOPS_RECORD_SIZE - 4U,
           Frame,
           Root,
           VALID_SCTLR_M_ONLY,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2,
           MOTO_G20_TELEMETRY_COMMIT_COMPLETE
           ), "wrong zone size rejected");
  Check (BytesEqualVolatile (Zone, SavedZone, sizeof (SavedZone)),
         "wrong zone size rejection write-free");

  memset (AlternateZone, 0x77, sizeof (AlternateZone));
  Check (!MotoG20TelemetryV2HostCommitPostMmuToZone (
           AlternateZone,
           sizeof (AlternateZone),
           Frame,
           Root,
           VALID_SCTLR_M_ONLY,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2,
           MOTO_G20_TELEMETRY_COMMIT_COMPLETE
           ), "alternate zone rejected");
  Check (AlternateZone[0] == 0x77 &&
         AlternateZone[sizeof (AlternateZone) - 1U] == 0x77,
         "alternate zone rejection write-free");

  Check (!MotoG20TelemetryV2HostCommitPostMmuToZone (
           Zone,
           MOTO_G20_RAMOOPS_RECORD_SIZE,
           Frame,
           Root,
           VALID_SCTLR_M_ONLY,
           MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
           MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
           MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2,
           MOTO_G20_TELEMETRY_COMMIT_AFTER_SIGNATURE + 1U
           ), "invalid fault boundary rejected");
  Check (BytesEqualVolatile (Zone, SavedZone, sizeof (SavedZone)),
         "invalid fault boundary rejection write-free");

  Entry = FindDescriptor (Root, MOTO_G20_RAMOOPS_TELEMETRY_ZONE_BASE, 3);
  Check (Entry != NULL, "zone leaf relocalized");
  if (Entry != NULL) {
    SavedDescriptor = *Entry;
    *Entry = 0;
    Check (!Commit (
             Zone, Frame, Root, VALID_SCTLR_M_ONLY,
             MOTO_G20_POST_MMU_EXPECTED_TCR_EL2,
             MOTO_G20_POST_MMU_EXPECTED_TTBR0_EL2,
             MOTO_G20_POST_MMU_EXPECTED_MAIR_EL2,
             MOTO_G20_TELEMETRY_COMMIT_COMPLETE
             ), "corrupt map commit rejected");
    Check (BytesEqualVolatile (Zone, SavedZone, sizeof (SavedZone)),
           "corrupt map rejection write-free");
    *Entry = SavedDescriptor;
  }

  Check (!MotoG20TelemetryV2CommitPostMmuFrame (Frame),
         "host production entry refuses fixed-address commit");

  printf (
    "MMU3C2E_POST_MMU_WRITER pages=%u final_top=0x%llx "
    "ramoops=0x%llx-0x%llx zone=0x%llx attr=DEVICE xn=1 "
    "m_only=1 m_plus_c=1 aligned_store_bits=32\n",
    (unsigned)AllocatedPages,
    (unsigned long long)gTop,
    (unsigned long long)MOTO_G20_RAMOOPS_BASE,
    (unsigned long long)RAMOOPS_END,
    (unsigned long long)MOTO_G20_RAMOOPS_TELEMETRY_ZONE_BASE
    );
  printf (
    "MMU3C2E_POST_MMU_WRITER_RESULT PASS=%u TOTAL=%u\n",
    (unsigned)gPassed,
    (unsigned)gTotal
    );

  munmap (ZoneMap, MOTO_G20_RAMOOPS_RECORD_SIZE);
  munmap (WorkspaceMap, WORKSPACE_SIZE);
  return (gPassed == gTotal) ? 0 : 1;
}

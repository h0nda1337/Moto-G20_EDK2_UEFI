#define _GNU_SOURCE
#include <Base.h>
#include <Uefi/UefiBaseType.h>
#include <Library/ArmLib.h>
#include <AArch64/AArch64Mmu.h>
#include "../../Library/MotoG20TranslationTableLib/MotoG20TranslationTableLib.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>

#define PAGE_SIZE                 0x1000ULL
#define WORKSPACE_BASE            0x81882000ULL
#define WORKSPACE_END             0x81982000ULL
#define WORKSPACE_SIZE            0x00100000ULL
#define EXPECTED_TABLE_PAGES      8U
#define EXPECTED_FINAL_TOP        0x8197A000ULL
#define RAMOOPS_BASE              0xFFF80000ULL
#define RAMOOPS_SIZE              0x00040000ULL
#define RAMOOPS_END               (RAMOOPS_BASE + RAMOOPS_SIZE)
#define RAMOOPS_ZONE5_BASE        0xFFFA8000ULL
#define RAMOOPS_ZONE5_END         0xFFFB0000ULL
#define MAX_TABLE_PAGES           128U

static unsigned char *gWorkspace;
static UINT64 gTop = WORKSPACE_END;
static UINTN gAllocCount;
static UINTN gZeroedPages;
static UINTN gZeroBeforePoisonFailures;
static VOID *gAllocations[MAX_TABLE_PAGES];
static VOID *gSeen[MAX_TABLE_PAGES];
static UINTN gSeenCount;
static int gGraphOk;

VOID *EFIAPI
AllocatePages (IN UINTN Pages)
{
  UINT64 NewTop;

  if ((Pages != 1) || (gAllocCount >= MAX_TABLE_PAGES)) {
    return NULL;
  }

  NewTop = gTop - PAGE_SIZE;
  if ((NewTop < WORKSPACE_BASE) || ((NewTop & (PAGE_SIZE - 1)) != 0)) {
    return NULL;
  }

  gTop = NewTop;
  gAllocations[gAllocCount++] = (VOID *)(UINTN)NewTop;
  return (VOID *)(UINTN)NewTop;
}

VOID *EFIAPI
ZeroMem (OUT VOID *Buffer, IN UINTN Length)
{
  UINTN Index;
  UINTN ByteIndex;
  BOOLEAN Found;

  Found = FALSE;
  for (Index = 0; Index < gAllocCount; Index++) {
    if (gAllocations[Index] != Buffer) {
      continue;
    }

    Found = TRUE;
    if (Length != PAGE_SIZE) {
      gZeroBeforePoisonFailures++;
    }

    for (ByteIndex = 0; ByteIndex < PAGE_SIZE; ByteIndex++) {
      if (((unsigned char *)Buffer)[ByteIndex] != 0xA5) {
        gZeroBeforePoisonFailures++;
        break;
      }
    }

    if (Length == PAGE_SIZE) {
      gZeroedPages++;
    }

    break;
  }

  if (!Found) {
    gZeroBeforePoisonFailures++;
  }

  return memset (Buffer, 0, Length);
}

static int
Check (int Condition, const char *What)
{
  if (!Condition) {
    fprintf (stderr, "FAIL: %s\n", What);
    return 0;
  }

  return 1;
}

static int
Seen (VOID *Table)
{
  UINTN Index;

  for (Index = 0; Index < gSeenCount; Index++) {
    if (gSeen[Index] == Table) {
      return 1;
    }
  }

  return 0;
}

static void
WalkTableGraph (UINT64 *Table, UINTN Level)
{
  UINTN Index;
  UINT64 Descriptor;
  UINT64 ChildAddress;

  if ((((UINTN)Table & (PAGE_SIZE - 1)) != 0) ||
      ((UINT64)(UINTN)Table < WORKSPACE_BASE) ||
      ((UINT64)(UINTN)Table >= WORKSPACE_END) ||
      Seen (Table) ||
      (gSeenCount >= MAX_TABLE_PAGES))
  {
    gGraphOk = 0;
    return;
  }

  gSeen[gSeenCount++] = Table;
  if (Level >= 3) {
    return;
  }

  for (Index = 0; Index < TT_ENTRY_COUNT; Index++) {
    Descriptor = Table[Index];
    if ((Descriptor & TT_TYPE_MASK) != TT_TYPE_TABLE_ENTRY) {
      continue;
    }

    ChildAddress = Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE;
    WalkTableGraph ((UINT64 *)(UINTN)ChildAddress, Level + 1);
  }
}

static ARM_MEMORY_REGION_DESCRIPTOR gMemoryTable[] = {
  { 0x80000000ULL, 0x80000000ULL, 0x01FF0000ULL,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK },
  { 0x82000000ULL, 0x82000000ULL, 0x00A0B000ULL,
    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK },
  { 0x9E000000ULL, 0x9E000000ULL, 0x009E4000ULL,
    ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED },
  { RAMOOPS_BASE, RAMOOPS_BASE, RAMOOPS_SIZE,
    ARM_MEMORY_REGION_ATTRIBUTE_DEVICE },
  { 0, 0, 0, 0 }
};

static int
CheckRange (
  VOID    *Root,
  UINT64  Start,
  UINT64  End,
  UINT64  AttributeIndex,
  BOOLEAN RequireXn,
  UINTN   ExpectedLevel,
  const char *Name
  )
{
  UINT64 Va;
  MOTO_G20_TRANSLATION_LOOKUP Result;
  EFI_STATUS Status;

  for (Va = Start; Va < End; Va += PAGE_SIZE) {
    Status = MotoG20LookupTranslation (Root, Va, &Result);
    if (EFI_ERROR (Status) || !Result.Mapped ||
        (Result.PhysicalAddress != Va) ||
        ((Result.Descriptor & TT_AF) == 0) ||
        ((Result.Descriptor & TT_ATTR_INDX_MASK) != AttributeIndex) ||
        (RequireXn && ((Result.Descriptor & TT_XN_MASK) == 0)) ||
        (Result.Level != ExpectedLevel))
    {
      fprintf (
        stderr,
        "FAIL: %s VA=0x%llx mapped=%u level=%u PA=0x%llx desc=0x%llx status=0x%llx\n",
        Name,
        (unsigned long long)Va,
        Result.Mapped,
        (unsigned)Result.Level,
        (unsigned long long)Result.PhysicalAddress,
        (unsigned long long)Result.Descriptor,
        (unsigned long long)Status
        );
      return 0;
    }
  }

  return 1;
}

static int
CheckUnmapped (VOID *Root, UINT64 Address, const char *Name)
{
  MOTO_G20_TRANSLATION_LOOKUP Result;
  EFI_STATUS Status;

  Status = MotoG20LookupTranslation (Root, Address, &Result);
  if (EFI_ERROR (Status) || Result.Mapped) {
    fprintf (
      stderr,
      "FAIL: %s VA=0x%llx mapped=%u desc=0x%llx status=0x%llx\n",
      Name,
      (unsigned long long)Address,
      Result.Mapped,
      (unsigned long long)Result.Descriptor,
      (unsigned long long)Status
      );
    return 0;
  }

  return 1;
}

static UINT64 *
ChildTable (UINT64 *Table, UINTN Index)
{
  UINT64 Descriptor;

  Descriptor = Table[Index];
  if ((Descriptor & TT_TYPE_MASK) != TT_TYPE_TABLE_ENTRY) {
    return NULL;
  }

  return (UINT64 *)(UINTN)(Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE);
}

int
main (void)
{
  VOID *Root;
  UINTN TableSize;
  UINTN AllocatedPages;
  EFI_STATUS Status;
  UINTN Index;
  UINTN Slot;
  UINTN Passed;
  UINTN Total;
  UINTN PoisonDescriptors;
  UINT64 *L1;
  UINT64 *LegacyL2;
  UINT64 *RamoopsL2;
  UINT64 *RamoopsL3;
  MOTO_G20_TRANSLATION_LOOKUP Lookup;
  const UINT64 Critical[] = {
    0x80080000ULL, 0x81882000ULL, 0x8197A000ULL,
    0x82000000ULL, 0x82A0A000ULL, 0x9E000000ULL,
    0x9E9E3000ULL, RAMOOPS_BASE, RAMOOPS_ZONE5_BASE,
    RAMOOPS_ZONE5_END - 1, RAMOOPS_END - 1
  };
  const UINT64 Guards[] = {
    0x00000000ULL, 0x7FFFF000ULL, 0x81FF0000ULL,
    0x82A0B000ULL, 0x9E9E4000ULL, 0xFFE00000ULL,
    RAMOOPS_BASE - PAGE_SIZE, RAMOOPS_END, 0xFFFFF000ULL,
    0x100000000ULL
  };

  Passed = 0;
  Total = 0;
  gWorkspace = mmap (
                 (void *)(UINTN)WORKSPACE_BASE,
                 WORKSPACE_SIZE,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                 -1,
                 0
                 );
  if ((gWorkspace == MAP_FAILED) ||
      ((UINT64)(UINTN)gWorkspace != WORKSPACE_BASE))
  {
    fprintf (stderr, "WORKSPACE_MMAP_FAIL errno=%d\n", errno);
    return 2;
  }

  memset (gWorkspace, 0xA5, WORKSPACE_SIZE);
  Root = NULL;
  TableSize = 0;
  AllocatedPages = 0;
  Status = MotoG20BuildTranslationTables (
             gMemoryTable,
             &Root,
             &TableSize,
             &AllocatedPages
             );

  Total++; Passed += Check (!EFI_ERROR (Status), "builder success");
  Total++; Passed += Check (
                       (UINT64)(UINTN)Root == 0x81981000ULL,
                       "root base"
                       );
  Total++; Passed += Check (TableSize == 16, "logical root size");
  Total++; Passed += Check (
                       AllocatedPages == EXPECTED_TABLE_PAGES,
                       "exact allocated page count"
                       );
  Total++; Passed += Check (
                       gAllocCount == EXPECTED_TABLE_PAGES,
                       "allocator exact page count"
                       );
  Total++; Passed += Check (gTop == EXPECTED_FINAL_TOP, "exact final top");
  Total++; Passed += Check (
                       gZeroedPages == EXPECTED_TABLE_PAGES,
                       "all pages zeroed"
                       );
  Total++; Passed += Check (
                       gZeroBeforePoisonFailures == 0,
                       "zeroing precedes population"
                       );

  gSeenCount = 0;
  gGraphOk = 1;
  WalkTableGraph ((UINT64 *)Root, 0);
  Total++; Passed += Check (gGraphOk, "table graph valid");
  Total++; Passed += Check (
                       gSeenCount == EXPECTED_TABLE_PAGES,
                       "all allocated pages reachable"
                       );
  for (Index = 0; Index < EXPECTED_TABLE_PAGES; Index++) {
    Total++; Passed += Check (Seen (gAllocations[Index]), "no orphan table");
  }

  L1 = ChildTable ((UINT64 *)Root, 0);
  LegacyL2 = (L1 == NULL) ? NULL : ChildTable (L1, 2);
  RamoopsL2 = (L1 == NULL) ? NULL : ChildTable (L1, 3);
  RamoopsL3 = (RamoopsL2 == NULL) ? NULL : ChildTable (RamoopsL2, 511);
  Total++; Passed += Check (
                       (UINT64)(UINTN)L1 == 0x81980000ULL,
                       "L1 allocation unchanged"
                       );
  Total++; Passed += Check (
                       (UINT64)(UINTN)LegacyL2 == 0x8197F000ULL,
                       "legacy L2 allocation unchanged"
                       );
  Total++; Passed += Check (
                       (UINT64)(UINTN)RamoopsL2 == 0x8197B000ULL,
                       "ramoops L2 allocation"
                       );
  Total++; Passed += Check (
                       (UINT64)(UINTN)RamoopsL3 == 0x8197A000ULL,
                       "ramoops L3 allocation"
                       );

  Total++; Passed += Check (
                       CheckRange (
                         Root,
                         0x80000000ULL,
                         0x81E00000ULL,
                         TT_ATTR_INDX_MEMORY_WRITE_BACK,
                         FALSE,
                         2,
                         "legacy region 1 blocks"
                         ),
                       "legacy region 1 blocks exhaustive"
                       );
  Total++; Passed += Check (
                       CheckRange (
                         Root,
                         0x81E00000ULL,
                         0x81FF0000ULL,
                         TT_ATTR_INDX_MEMORY_WRITE_BACK,
                         FALSE,
                         3,
                         "legacy region 1 tail"
                         ),
                       "legacy region 1 tail exhaustive"
                       );
  Total++; Passed += Check (
                       CheckRange (
                         Root,
                         0x82000000ULL,
                         0x82A00000ULL,
                         TT_ATTR_INDX_MEMORY_WRITE_BACK,
                         FALSE,
                         2,
                         "legacy region 2 blocks"
                         ),
                       "legacy region 2 blocks exhaustive"
                       );
  Total++; Passed += Check (
                       CheckRange (
                         Root,
                         0x82A00000ULL,
                         0x82A0B000ULL,
                         TT_ATTR_INDX_MEMORY_WRITE_BACK,
                         FALSE,
                         3,
                         "legacy region 2 tail"
                         ),
                       "legacy region 2 tail exhaustive"
                       );
  Total++; Passed += Check (
                       CheckRange (
                         Root,
                         0x9E000000ULL,
                         0x9E800000ULL,
                         TT_ATTR_INDX_MEMORY_NON_CACHEABLE,
                         FALSE,
                         2,
                         "framebuffer blocks"
                         ),
                       "framebuffer blocks exhaustive"
                       );
  Total++; Passed += Check (
                       CheckRange (
                         Root,
                         0x9E800000ULL,
                         0x9E9E4000ULL,
                         TT_ATTR_INDX_MEMORY_NON_CACHEABLE,
                         FALSE,
                         3,
                         "framebuffer tail"
                         ),
                       "framebuffer tail exhaustive"
                       );
  Total++; Passed += Check (
                       CheckRange (
                         Root,
                         RAMOOPS_BASE,
                         RAMOOPS_END,
                         TT_ATTR_INDX_DEVICE_MEMORY,
                         TRUE,
                         3,
                         "ramoops device XN"
                         ),
                       "ramoops exhaustive device XN"
                       );

  for (Index = 0; Index < sizeof (Critical) / sizeof (Critical[0]); Index++) {
    Status = MotoG20LookupTranslation (Root, Critical[Index], &Lookup);
    Total++; Passed += Check (
                         !EFI_ERROR (Status) && Lookup.Mapped &&
                         (Lookup.PhysicalAddress == Critical[Index]),
                         "critical identity mapping"
                         );
  }

  for (Index = 0; Index < sizeof (Guards) / sizeof (Guards[0]); Index++) {
    Total++; Passed += Check (
                         CheckUnmapped (Root, Guards[Index], "guard"),
                         "guard unmapped"
                         );
  }

  if (RamoopsL3 != NULL) {
    for (Slot = 0; Slot < TT_ENTRY_COUNT; Slot++) {
      BOOLEAN ShouldBeMapped;
      BOOLEAN IsMapped;

      ShouldBeMapped = (BOOLEAN)((Slot >= 384) && (Slot < 448));
      IsMapped = (BOOLEAN)((RamoopsL3[Slot] & TT_TYPE_MASK) != 0);
      Total++; Passed += Check (
                           IsMapped == ShouldBeMapped,
                           "exact ramoops L3 envelope"
                           );
    }
  } else {
    Total++; Passed += Check (0, "ramoops L3 unavailable");
  }

  PoisonDescriptors = 0;
  for (Index = 0; Index < gAllocCount; Index++) {
    for (Slot = 0; Slot < TT_ENTRY_COUNT; Slot++) {
      if (((UINT64 *)gAllocations[Index])[Slot] ==
          0xA5A5A5A5A5A5A5A5ULL)
      {
        PoisonDescriptors++;
      }
    }
  }
  Total++; Passed += Check (PoisonDescriptors == 0, "no poison survives");

  printf (
    "MMU3C2D_RAMOOPS_MAP pages=%u final_top=0x%llx "
    "base=0x%llx end=0x%llx zone5=0x%llx attr=DEVICE xn=1\n",
    (unsigned)AllocatedPages,
    (unsigned long long)gTop,
    (unsigned long long)RAMOOPS_BASE,
    (unsigned long long)RAMOOPS_END,
    (unsigned long long)RAMOOPS_ZONE5_BASE
    );
  printf (
    "MMU3C2D_POST_MMU_RAMOOPS_MAP_RESULT PASS=%u TOTAL=%u\n",
    (unsigned)Passed,
    (unsigned)Total
    );

  munmap (gWorkspace, WORKSPACE_SIZE);
  return (Passed == Total) ? 0 : 1;
}

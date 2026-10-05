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

#define PAGE_SIZE 0x1000ULL
#define WORKSPACE_BASE 0x81882000ULL
#define WORKSPACE_END  0x81982000ULL
#define WORKSPACE_SIZE 0x100000ULL
#define MAX_TABLE_PAGES 128U

static unsigned char *gWorkspace;
static UINT64 gTop = WORKSPACE_END;
static UINTN gAllocCount;
static UINTN gZeroedPages;
static UINTN gZeroBeforePoisonFailures;
static VOID *gAllocations[MAX_TABLE_PAGES];
static VOID *gZeroed[MAX_TABLE_PAGES];

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
  UINTN I;
  UINTN J;
  BOOLEAN IsAllocated = FALSE;

  for (I = 0; I < gAllocCount; I++) {
    if (gAllocations[I] == Buffer) {
      IsAllocated = TRUE;
      if (Length != PAGE_SIZE) {
        gZeroBeforePoisonFailures++;
      }
      for (J = 0; J < PAGE_SIZE; J++) {
        if (((unsigned char *)Buffer)[J] != 0xA5) {
          gZeroBeforePoisonFailures++;
          break;
        }
      }
      if (Length == PAGE_SIZE) {
        gZeroed[gZeroedPages++] = Buffer;
      }
      break;
    }
  }
  (void)IsAllocated;
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

static void
ResetAllocator (void)
{
  memset (gWorkspace, 0xA5, WORKSPACE_SIZE);
  memset (gAllocations, 0, sizeof (gAllocations));
  memset (gZeroed, 0, sizeof (gZeroed));
  gTop = WORKSPACE_END;
  gAllocCount = 0;
  gZeroedPages = 0;
  gZeroBeforePoisonFailures = 0;
}

static ARM_MEMORY_REGION_DESCRIPTOR gMemoryTable[] = {
  { 0x80000000ULL, 0x80000000ULL, 0x01FF0000ULL, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK },
  { 0x82000000ULL, 0x82000000ULL, 0x00A0B000ULL, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK },
  { 0x9E000000ULL, 0x9E000000ULL, 0x009E4000ULL, ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED },
  { 0, 0, 0, 0 }
};

static int
Lookup (VOID *Root, UINT64 Va, BOOLEAN Mapped, UINT64 Attr, UINTN *LevelOut)
{
  MOTO_G20_TRANSLATION_LOOKUP Result;
  EFI_STATUS Status;
  Status = MotoG20LookupTranslation (Root, Va, &Result);
  if (LevelOut != NULL) {
    *LevelOut = Result.Level;
  }
  return Check (!EFI_ERROR (Status) && Result.Mapped == Mapped &&
                (!Mapped || (Result.PhysicalAddress == Va &&
                 (Result.Descriptor & TT_AF) != 0 &&
                 (Result.Descriptor & TT_ATTR_INDX_MASK) == Attr)),
                "software lookup");
}

static UINTN gSeenCount;
static VOID *gSeen[MAX_TABLE_PAGES];
static int gGraphOk;

static int
Seen (VOID *Table)
{
  UINTN I;
  for (I = 0; I < gSeenCount; I++) {
    if (gSeen[I] == Table) return 1;
  }
  return 0;
}

static void
WalkTableGraph (UINT64 *Table, UINTN Level, const char *Prefix)
{
  UINTN I;
  UINT64 Descriptor;
  UINT64 ChildAddress;
  UINT64 *Child;
  char NextPrefix[128];

  if ((((UINTN)Table & (PAGE_SIZE - 1)) != 0) ||
      ((UINT64)(UINTN)Table < WORKSPACE_BASE) ||
      ((UINT64)(UINTN)Table >= WORKSPACE_END) || Seen (Table) ||
      (gSeenCount >= MAX_TABLE_PAGES)) {
    gGraphOk = 0;
    return;
  }
  gSeen[gSeenCount++] = Table;
  printf ("%sL%u table 0x%llx\n", Prefix, (unsigned)Level,
          (unsigned long long)(UINTN)Table);
  if (Level >= 3) return;
  for (I = 0; I < TT_ENTRY_COUNT; I++) {
    Descriptor = Table[I];
    if ((Descriptor & TT_TYPE_MASK) != TT_TYPE_TABLE_ENTRY) continue;
    ChildAddress = Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE;
    Child = (UINT64 *)(UINTN)ChildAddress;
    snprintf (NextPrefix, sizeof (NextPrefix), "%s  -> ", Prefix);
    printf ("%sentry[%u] -> 0x%llx\n", NextPrefix, (unsigned)I,
            (unsigned long long)ChildAddress);
    WalkTableGraph (Child, Level + 1, NextPrefix);
  }
}

static int
CheckExhaustiveRange (VOID *Root, UINT64 Start, UINT64 End, UINT64 Attr,
                      const char *Name, UINTN Levels[4])
{
  UINT64 Va;
  MOTO_G20_TRANSLATION_LOOKUP Result;
  EFI_STATUS Status;
  for (Va = Start; Va < End; Va += PAGE_SIZE) {
    Status = MotoG20LookupTranslation (Root, Va, &Result);
    if (EFI_ERROR (Status) || !Result.Mapped || Result.PhysicalAddress != Va ||
        (Result.Descriptor & TT_AF) == 0 ||
        (Result.Descriptor & TT_ATTR_INDX_MASK) != Attr || Result.Level > 3) {
      fprintf (stderr, "FAIL: %s at 0x%llx\n", Name, (unsigned long long)Va);
      return 0;
    }
    Levels[Result.Level]++;
  }
  return 1;
}

int
main (void)
{
  VOID *Root;
  UINTN TableSize;
  UINTN AllocatedPages;
  EFI_STATUS Status;
  UINTN I;
  UINTN J;
  UINTN Levels1[4] = {0};
  UINTN Levels2[4] = {0};
  UINTN Levels3[4] = {0};
  UINTN LevelsTables[4] = {0};
  UINTN Passed = 0;
  UINTN Total = 0;
  UINTN ReachableBefore;
  UINTN ZeroSlots;
  UINTN ZeroFailures = 0;
  UINT64 Descriptor;
  MOTO_G20_TRANSLATION_LOOKUP Result;
  const UINT64 Critical[] = {
    0x80080000ULL, 0x80081000ULL, 0x81000000ULL, 0x811D0000ULL,
    0x811F0000ULL, 0x81882000ULL, 0x81981000ULL, 0x82000000ULL,
    0x82045EE0ULL, 0x82100000ULL, 0x82A0A000ULL, 0x9E000000ULL,
    0x9E9E3000ULL
  };
  const UINT64 Guards[] = {
    0x00000000ULL, 0x7FFFF000ULL, 0x81FF0000ULL, 0x81FFF000ULL,
    0x82A0B000ULL, 0x9D000000ULL, 0x9DFFF000ULL, 0x9E9E4000ULL,
    0x94000000ULL, 0xF4100000ULL, 0x100000000ULL
  };

  gWorkspace = mmap ((void *)(UINTN)WORKSPACE_BASE, WORKSPACE_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                      -1, 0);
  if ((gWorkspace == MAP_FAILED) || ((UINT64)(UINTN)gWorkspace != WORKSPACE_BASE)) {
    fprintf (stderr, "REAL_WORKSPACE_MMAP_FAIL errno=%d\n", errno);
    return 2;
  }
  ResetAllocator ();
  Root = NULL;
  TableSize = 0;
  AllocatedPages = 0;
  Status = MotoG20BuildTranslationTables (gMemoryTable, &Root, &TableSize,
                                           &AllocatedPages);
  Total++; Passed += Check (!EFI_ERROR (Status), "builder success");
  Total++; Passed += Check ((UINT64)(UINTN)Root == 0x81981000ULL, "root base");
  Total++; Passed += Check (TableSize == 16, "root logical size");
  Total++; Passed += Check (gAllocCount == AllocatedPages, "allocator count");
  printf ("ALLOCATED_TABLE_PAGES=%u\n", (unsigned)AllocatedPages);
  for (I = 0; I < gAllocCount; I++) {
    printf ("ALLOCATION[%u]=0x%llx\n", (unsigned)I,
            (unsigned long long)(UINTN)gAllocations[I]);
  }
  printf ("FINAL_FREE_MEMORY_TOP=0x%llx\n", (unsigned long long)gTop);

  gSeenCount = 0;
  gGraphOk = 1;
  WalkTableGraph ((UINT64 *)Root, 0, "");
  ReachableBefore = gSeenCount;
  Total++; Passed += Check (gGraphOk, "table graph bounds/cycles");
  Total++; Passed += Check (ReachableBefore == AllocatedPages, "reachable page count");
  for (I = 0; I < gAllocCount; I++) {
    Total++; Passed += Check (Seen (gAllocations[I]), "no orphan table");
  }
  printf ("REACHABLE_TABLE_PAGES=%u\n", (unsigned)ReachableBefore);

  Total++; Passed += Check (CheckExhaustiveRange (Root, 0x80000000ULL, 0x81FF0000ULL,
                                                   TT_ATTR_INDX_MEMORY_WRITE_BACK,
                                                   "REGION1", Levels1), "region1 exhaustive");
  Total++; Passed += Check (CheckExhaustiveRange (Root, 0x82000000ULL, 0x82A0B000ULL,
                                                   TT_ATTR_INDX_MEMORY_WRITE_BACK,
                                                   "DTB_INITRD", Levels2), "region2 exhaustive");
  Total++; Passed += Check (CheckExhaustiveRange (Root, 0x9E000000ULL, 0x9E9E4000ULL,
                                                   TT_ATTR_INDX_MEMORY_NON_CACHEABLE,
                                                   "FRAMEBUFFER", Levels3), "framebuffer exhaustive");
  printf ("REGION1_LEVELS=%u/%u/%u/%u\n", (unsigned)Levels1[0], (unsigned)Levels1[1], (unsigned)Levels1[2], (unsigned)Levels1[3]);
  printf ("REGION2_LEVELS=%u/%u/%u/%u\n", (unsigned)Levels2[0], (unsigned)Levels2[1], (unsigned)Levels2[2], (unsigned)Levels2[3]);
  printf ("FRAMEBUFFER_LEVELS=%u/%u/%u/%u\n", (unsigned)Levels3[0], (unsigned)Levels3[1], (unsigned)Levels3[2], (unsigned)Levels3[3]);

  for (I = 0; I < sizeof (Critical) / sizeof (Critical[0]); I++) {
    Status = MotoG20LookupTranslation (Root, Critical[I], &Result);
    printf ("CRITICAL 0x%llx mapped=%u level=%u pa=0x%llx desc=0x%llx status=0x%llx\n",
            (unsigned long long)Critical[I], Result.Mapped, (unsigned)Result.Level,
            (unsigned long long)Result.PhysicalAddress,
            (unsigned long long)Result.Descriptor, (unsigned long long)Status);
    Total++; Passed += Check (!EFI_ERROR (Status) && Result.Mapped &&
                              Result.PhysicalAddress == Critical[I], "critical address");
  }

  for (I = 0; I < gAllocCount; I++) {
    Status = MotoG20LookupTranslation (Root, (UINT64)(UINTN)gAllocations[I], &Result);
    LevelsTables[Result.Level]++;
    Total++; Passed += Check (!EFI_ERROR (Status) && Result.Mapped &&
                              Result.PhysicalAddress == (UINT64)(UINTN)gAllocations[I] &&
                              (Result.Descriptor & TT_ATTR_INDX_MASK) == TT_ATTR_INDX_MEMORY_WRITE_BACK,
                              "table page mapped write-back");
    printf ("TABLE_PAGE 0x%llx level=%u\n", (unsigned long long)(UINTN)gAllocations[I], (unsigned)Result.Level);
  }
  printf ("TABLE_PAGE_LEVELS=%u/%u/%u/%u\n", (unsigned)LevelsTables[0], (unsigned)LevelsTables[1], (unsigned)LevelsTables[2], (unsigned)LevelsTables[3]);

  for (I = 0; I < sizeof (Guards) / sizeof (Guards[0]); I++) {
    Status = MotoG20LookupTranslation (Root, Guards[I], &Result);
    Total++; Passed += Check (!EFI_ERROR (Status) && !Result.Mapped, "guard unmapped");
    printf ("GUARD 0x%llx mapped=%u status=0x%llx\n",
            (unsigned long long)Guards[I], Result.Mapped, (unsigned long long)Status);
  }
  Total++; Passed += Check (gAllocCount == AllocatedPages && gTop >= WORKSPACE_BASE && gTop <= WORKSPACE_END,
                            "workspace bounds");
  Total++; Passed += Check (gZeroedPages == gAllocCount && gZeroBeforePoisonFailures == 0,
                            "full page zeroing before population");
  for (I = 0; I < gAllocCount; I++) {
    ZeroSlots = 0;
    for (J = 0; J < PAGE_SIZE / sizeof (UINT64); J++) {
      Descriptor = ((UINT64 *)gAllocations[I])[J];
      if (Descriptor == 0) ZeroSlots++;
      if (Descriptor == 0xA5A5A5A5A5A5A5A5ULL) ZeroFailures++;
    }
    Total++; Passed += Check (ZeroSlots > 0, "unused descriptor slots zero");
  }
  Total++; Passed += Check (ZeroFailures == 0, "no poison survives");

  printf ("MMU2A_REAL_HARNESS_RESULT PASS=%u TOTAL=%u\n", (unsigned)Passed, (unsigned)Total);
  printf ("WORKSPACE_SIZE=0x%llx ALLOCATED_BYTES=0x%llx REMAINING_BYTES=0x%llx FINAL_TOP=0x%llx\n",
          (unsigned long long)WORKSPACE_SIZE,
          (unsigned long long)(gAllocCount * PAGE_SIZE),
          (unsigned long long)(WORKSPACE_SIZE - (gAllocCount * PAGE_SIZE)),
          (unsigned long long)gTop);
  munmap (gWorkspace, WORKSPACE_SIZE);
  return (Passed == Total) ? 0 : 1;
}

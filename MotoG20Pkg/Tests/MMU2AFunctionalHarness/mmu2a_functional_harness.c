#include <Base.h>
#include <Uefi/UefiBaseType.h>
#include <Library/ArmLib.h>
#include <AArch64/AArch64Mmu.h>
#include "../../Library/MotoG20TranslationTableLib/MotoG20TranslationTableLib.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_SIZE 4096U
#define POOL_BASE ((uintptr_t)0x10000000ULL)
#define POOL_PAGES 64U
#define POOL_BYTES ((size_t)POOL_PAGES * PAGE_SIZE)

static unsigned char *gPool;
static UINTN gNextPage;
static UINTN gAllocCalls;
static UINTN gZeroCalls;
static UINTN gZeroedPages;
static UINTN gZeroedLengthErrors;
static VOID *gAllocated[POOL_PAGES];

VOID *EFIAPI
AllocatePages (IN UINTN Pages)
{
  VOID *Result;
  if (Pages != 1 || gNextPage >= POOL_PAGES) {
    return NULL;
  }
  Result = gPool + (gNextPage * PAGE_SIZE);
  memset (Result, 0xA5, PAGE_SIZE);
  gAllocated[gNextPage] = Result;
  gNextPage++;
  gAllocCalls++;
  return Result;
}

VOID *EFIAPI
ZeroMem (OUT VOID *Buffer, IN UINTN Length)
{
  UINTN Index;
  BOOLEAN IsPoolPage = FALSE;
  gZeroCalls++;
  for (Index = 0; Index < gNextPage; Index++) {
    if (gAllocated[Index] == Buffer) {
      IsPoolPage = TRUE;
      if ((Length != PAGE_SIZE) || ((((uintptr_t)Buffer) & (PAGE_SIZE - 1)) != 0)) {
        gZeroedLengthErrors++;
      } else {
        gZeroedPages++;
      }
      break;
    }
  }
  (void)IsPoolPage;
  return memset (Buffer, 0, Length);
}

static int
Check (int Condition, const char *Name)
{
  if (!Condition) {
    fprintf (stderr, "FAIL: %s\n", Name);
    return 0;
  }
  return 1;
}

static void
ResetPool (void)
{
  memset (gPool, 0xA5, POOL_BYTES);
  memset (gAllocated, 0, sizeof (gAllocated));
  gNextPage = 0;
  gAllocCalls = 0;
  gZeroCalls = 0;
  gZeroedPages = 0;
  gZeroedLengthErrors = 0;
}

static ARM_MEMORY_REGION_DESCRIPTOR gMap[] = {
  { 0x0000000040000000ULL, 0x0000000040000000ULL, 0x0000000040000000ULL, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK },
  { 0x0000000100000000ULL, 0x0000000100000000ULL, 0x0000000000200000ULL, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE },
  { 0x0000000200001000ULL, 0x0000000200001000ULL, 0x0000000000001000ULL, ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED },
  { 0x0000000200002000ULL, 0x0000000200002000ULL, 0x0000000000001000ULL, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP },
  { 0, 0, 0, 0 }
};

static int
ExpectLookup (VOID *Root, UINT64 Va, UINTN Level, UINT64 Pa)
{
  MOTO_G20_TRANSLATION_LOOKUP Result;
  EFI_STATUS Status = MotoG20LookupTranslation (Root, Va, &Result);
  if (EFI_ERROR (Status) || !Result.Mapped || Result.Level != Level || Result.PhysicalAddress != Pa) {
    fprintf (stderr, "lookup va=%" PRIx64 " status=%" PRIx64 " mapped=%u level=%u pa=%" PRIx64 " desc=%" PRIx64 "\n",
             Va, (UINT64)Status, Result.Mapped, (unsigned)Result.Level,
             Result.PhysicalAddress, Result.Descriptor);
  }
  return Check (!EFI_ERROR (Status) && Result.Mapped && Result.Level == Level &&
                Result.PhysicalAddress == Pa, "positive lookup");
}

static int
ExpectUnmapped (VOID *Root, UINT64 Va)
{
  MOTO_G20_TRANSLATION_LOOKUP Result;
  EFI_STATUS Status = MotoG20LookupTranslation (Root, Va, &Result);
  if (EFI_ERROR (Status) || Result.Mapped) {
    fprintf (stderr, "unmapped va=%" PRIx64 " status=%" PRIx64 " mapped=%u level=%u desc=%" PRIx64 "\n",
             Va, (UINT64)Status, Result.Mapped, (unsigned)Result.Level, Result.Descriptor);
  }
  return Check (!EFI_ERROR (Status) && !Result.Mapped, "unmapped lookup");
}

static int
ExpectRejected (ARM_MEMORY_REGION_DESCRIPTOR *Table)
{
  VOID *Root;
  UINTN Size;
  UINTN Pages;
  EFI_STATUS Status;
  ResetPool ();
  Root = (VOID *)(UINTN)0xDEADBEEF;
  Size = 0xDEAD;
  Pages = 0xBEEF;
  Status = MotoG20BuildTranslationTables (Table, &Root, &Size, &Pages);
  return Check (EFI_ERROR (Status) && Root == NULL && Size == 0 && Pages == 0,
                "negative builder rejection");
}

static int
RunPartialRegression (
  const char                    *Name,
  UINT64                         Base,
  UINT64                         Length,
  ARM_MEMORY_REGION_ATTRIBUTES   Attributes,
  UINTN                          ExpectedLevel
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR Table[2] = {
    { Base, Base, Length, Attributes },
    { 0, 0, 0, 0 }
  };
  MOTO_G20_TRANSLATION_LOOKUP Result;
  VOID *Root;
  UINTN Size;
  UINTN Pages;
  EFI_STATUS Status;
  int Good;

  ResetPool ();
  Root = NULL;
  Size = 0;
  Pages = 0;
  Status = MotoG20BuildTranslationTables (Table, &Root, &Size, &Pages);
  Good = !EFI_ERROR (Status);
  if (Good) {
    Status = MotoG20LookupTranslation (Root, Base, &Result);
    Good = !EFI_ERROR (Status) && Result.Mapped && Result.Level == ExpectedLevel &&
           Result.PhysicalAddress == Base;
  }
  if (Good) {
    Status = MotoG20LookupTranslation (Root, Base + Length, &Result);
    Good = !EFI_ERROR (Status) && !Result.Mapped;
  }
  if (!Good) {
    fprintf (stderr, "FAIL: partial regression %s\n", Name);
  }
  return Good;
}

int
main (void)
{
  VOID *Root;
  UINTN Size;
  UINTN Pages;
  EFI_STATUS Status;
  UINTN I;
  UINTN FunctionalChecks = 0;
  UINTN PassedChecks = 0;
  UINT64 Descriptor;
  MOTO_G20_TRANSLATION_LOOKUP Result;
  ARM_MEMORY_REGION_DESCRIPTOR Bad[6];
  unsigned char *Page;
  UINTN ZeroEntries;
  int RootUntouchedZero;
  int ChildrenUntouchedZero;
  UINTN PartialPassed = 0;
  const UINTN PartialTotal = 6;

  gPool = mmap ((void *)POOL_BASE, POOL_BYTES, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (gPool == MAP_FAILED || (uintptr_t)gPool != POOL_BASE) {
    fprintf (stderr, "FAIL: low-address mmap errno=%d\n", errno);
    return 2;
  }
  ResetPool ();

  Root = NULL;
  Size = 0;
  Pages = 0;
  Status = MotoG20BuildTranslationTables (gMap, &Root, &Size, &Pages);
  fprintf (stderr, "builder status=%" PRIx64 " root=%p size=%u pages=%u alloc=%u zero=%u zeroed=%u\n",
           (UINT64)Status, Root, (unsigned)Size, (unsigned)Pages,
           (unsigned)gAllocCalls, (unsigned)gZeroCalls, (unsigned)gZeroedPages);
  FunctionalChecks++;
  PassedChecks += Check (!EFI_ERROR (Status), "builder success");
  FunctionalChecks++;
  PassedChecks += Check (((uintptr_t)Root & (PAGE_SIZE - 1)) == 0, "root aligned");
  FunctionalChecks++;
  PassedChecks += Check (Size == 16, "root logical size");
  FunctionalChecks++;
  PassedChecks += Check (Pages == 5 && gAllocCalls == 5, "five allocated pages");
  FunctionalChecks++;
  PassedChecks += Check (gZeroCalls == 5 && gZeroedPages == 5 && gZeroedLengthErrors == 0,
                        "full-page zero calls");

  PassedChecks += ExpectLookup (Root, 0x40000000ULL, 1, 0x40000000ULL); FunctionalChecks++;
  PassedChecks += ExpectLookup (Root, 0x52345000ULL, 1, 0x52345000ULL); FunctionalChecks++;
  PassedChecks += ExpectLookup (Root, 0x7FFFF000ULL, 1, 0x7FFFF000ULL); FunctionalChecks++;
  PassedChecks += ExpectLookup (Root, 0x100000000ULL, 2, 0x100000000ULL); FunctionalChecks++;
  PassedChecks += ExpectLookup (Root, 0x100001000ULL, 2, 0x100001000ULL); FunctionalChecks++;
  PassedChecks += ExpectLookup (Root, 0x1001FF000ULL, 2, 0x1001FF000ULL); FunctionalChecks++;
  PassedChecks += ExpectLookup (Root, 0x200001000ULL, 3, 0x200001000ULL); FunctionalChecks++;
  PassedChecks += ExpectLookup (Root, 0x200002000ULL, 3, 0x200002000ULL); FunctionalChecks++;

  { const UINT64 Unmapped[] = { 0, 0x3FFFF000ULL, 0x80000000ULL, 0xFFFFFFFFULL,
                                0x100200000ULL, 0x200000000ULL, 0x200003000ULL };
    for (I = 0; I < sizeof (Unmapped) / sizeof (Unmapped[0]); I++) {
      PassedChecks += ExpectUnmapped (Root, Unmapped[I]); FunctionalChecks++;
    }
  }
  Status = MotoG20LookupTranslation (Root, 0xFFFFFFFFFFULL, &Result);
  FunctionalChecks++; PassedChecks += Check (!EFI_ERROR (Status), "last valid VA accepted");
  Status = MotoG20LookupTranslation (Root, 0x10000000000ULL, &Result);
  FunctionalChecks++; PassedChecks += Check (EFI_ERROR (Status), "one TiB rejected");

  Descriptor = Root == NULL ? 0 : ((UINT64 *)Root)[0];
  FunctionalChecks++; PassedChecks += Check ((Descriptor & TT_TYPE_MASK) == TT_TYPE_TABLE_ENTRY, "root table descriptor");
  Status = MotoG20LookupTranslation (Root, 0x40000000ULL, &Result);
  FunctionalChecks++; PassedChecks += Check ((Result.Descriptor & TT_AF) != 0 &&
    (Result.Descriptor & TT_ATTR_INDX_MASK) == TT_ATTR_INDX_MEMORY_WRITE_BACK &&
    (Result.Descriptor & TT_SH_MASK) == TT_SH_INNER_SHAREABLE, "write-back attributes");
  Status = MotoG20LookupTranslation (Root, 0x100000000ULL, &Result);
  FunctionalChecks++; PassedChecks += Check ((Result.Descriptor & TT_AF) != 0 &&
    (Result.Descriptor & TT_ATTR_INDX_MASK) == TT_ATTR_INDX_DEVICE_MEMORY &&
    (Result.Descriptor & TT_XN_MASK) != 0, "device attributes");
  Status = MotoG20LookupTranslation (Root, 0x200001000ULL, &Result);
  FunctionalChecks++; PassedChecks += Check ((Result.Descriptor & TT_AF) != 0 &&
    (Result.Descriptor & TT_ATTR_INDX_MASK) == TT_ATTR_INDX_MEMORY_NON_CACHEABLE, "non-cacheable attributes");
  Status = MotoG20LookupTranslation (Root, 0x200002000ULL, &Result);
  FunctionalChecks++; PassedChecks += Check ((Result.Descriptor & TT_AF) != 0 &&
    (Result.Descriptor & TT_ATTR_INDX_MASK) == TT_ATTR_INDX_MEMORY_WRITE_BACK &&
    (Result.Descriptor & TT_XN_MASK) != 0, "write-back XP XN attributes");

  RootUntouchedZero = 1;
  ChildrenUntouchedZero = 1;
  for (I = 0; I < gNextPage; I++) {
    Page = (unsigned char *)gAllocated[I];
    ZeroEntries = 0;
    for (UINTN J = 0; J < PAGE_SIZE / sizeof (UINT64); J++) {
      if (((UINT64 *)Page)[J] == 0) ZeroEntries++;
    }
    if (ZeroEntries == 0) {
      if (I == 0) RootUntouchedZero = 0;
      else ChildrenUntouchedZero = 0;
    }
  }
  FunctionalChecks++; PassedChecks += Check (RootUntouchedZero, "root full-page zeroing");
  FunctionalChecks++; PassedChecks += Check (ChildrenUntouchedZero, "child full-page zeroing");
  FunctionalChecks++; PassedChecks += Check (gZeroedPages == gAllocCalls, "zero call count");
  FunctionalChecks++; PassedChecks += Check (gZeroedLengthErrors == 0, "zero call shape");

  { ARM_MEMORY_REGION_DESCRIPTOR NullTable[1] = {{0,0,0,0}};
    Status = MotoG20BuildTranslationTables (NULL, &Root, &Size, &Pages);
    FunctionalChecks++; PassedChecks += Check (EFI_ERROR (Status), "NULL memory table rejection");
    ResetPool (); Root = (VOID *)1; Size = 1; Pages = 1;
    Status = MotoG20BuildTranslationTables (gMap, NULL, &Size, &Pages);
    FunctionalChecks++; PassedChecks += Check (EFI_ERROR(Status), "NULL root output rejection");
    Status = MotoG20BuildTranslationTables (gMap, &Root, NULL, &Pages);
    FunctionalChecks++; PassedChecks += Check (EFI_ERROR(Status), "NULL size output rejection");
    Status = MotoG20BuildTranslationTables (gMap, &Root, &Size, NULL);
    FunctionalChecks++; PassedChecks += Check (EFI_ERROR(Status), "NULL page output rejection");
    (void)NullTable;
  }
  memcpy (Bad, gMap, sizeof (Bad)); Bad[0].PhysicalBase++;
  PassedChecks += ExpectRejected (Bad); FunctionalChecks++;
  memcpy (Bad, gMap, sizeof (Bad)); Bad[0].PhysicalBase = 0x40000001ULL;
  PassedChecks += ExpectRejected (Bad); FunctionalChecks++;
  memcpy (Bad, gMap, sizeof (Bad)); Bad[0].VirtualBase = 0x40000001ULL;
  PassedChecks += ExpectRejected (Bad); FunctionalChecks++;
  memcpy (Bad, gMap, sizeof (Bad)); Bad[0].Length = 0x40000001ULL;
  PassedChecks += ExpectRejected (Bad); FunctionalChecks++;
  memcpy (Bad, gMap, sizeof (Bad)); Bad[0].VirtualBase = 0xFFFFFFFFF000ULL; Bad[0].PhysicalBase = Bad[0].VirtualBase; Bad[0].Length = 0x2000ULL;
  PassedChecks += ExpectRejected (Bad); FunctionalChecks++;
  memcpy (Bad, gMap, sizeof (Bad)); Bad[0].PhysicalBase = 0xFFFFFFFFFFFFF000ULL; Bad[0].VirtualBase = Bad[0].PhysicalBase; Bad[0].Length = 0x2000ULL;
  PassedChecks += ExpectRejected (Bad); FunctionalChecks++;
  memcpy (Bad, gMap, sizeof (Bad)); Bad[0].PhysicalBase = 0x10000000000ULL; Bad[0].VirtualBase = Bad[0].PhysicalBase; Bad[0].Length = 0x1000ULL;
  PassedChecks += ExpectRejected (Bad); FunctionalChecks++;

  PartialPassed += RunPartialRegression (
                    "A 4K at 1GiB boundary",
                    0x40000000ULL, 0x1000ULL,
                    ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED, 3
                    );
  PartialPassed += RunPartialRegression (
                    "B 2MiB at 1GiB boundary",
                    0x80000000ULL, 0x200000ULL,
                    ARM_MEMORY_REGION_ATTRIBUTE_DEVICE, 2
                    );
  PartialPassed += RunPartialRegression (
                    "C exact 1GiB",
                    0xC0000000ULL, 0x40000000ULL,
                    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK, 1
                    );
  PartialPassed += RunPartialRegression (
                    "D unaligned 2MiB",
                    0x50001000ULL, 0x200000ULL,
                    ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED, 3
                    );
  PartialPassed += RunPartialRegression (
                    "E ends before 1GiB boundary",
                    0x40000000ULL, 0x3FFFF000ULL,
                    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK, 2
                    );
  PartialPassed += RunPartialRegression (
                    "F crosses 1GiB boundary",
                    0x7FFFF000ULL, 0x2000ULL,
                    ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP, 3
                    );

  printf ("MMU2A_HARNESS_RESULT PASS=%u TOTAL=%u\n", (unsigned)PassedChecks, (unsigned)FunctionalChecks);
  printf ("MMU2A_PARTIAL_REGRESSIONS PASS=%u TOTAL=%u\n", (unsigned)PartialPassed, (unsigned)PartialTotal);
  printf ("ROOT=%p SIZE=%u PAGES=%u ALLOC=%u ZERO_CALLS=%u ZEROED_PAGES=%u\n",
          Root, (unsigned)Size, (unsigned)Pages, (unsigned)gAllocCalls,
          (unsigned)gZeroCalls, (unsigned)gZeroedPages);
  munmap (gPool, POOL_BYTES);
  return (PassedChecks == FunctionalChecks && PartialPassed == PartialTotal) ? 0 : 1;
}

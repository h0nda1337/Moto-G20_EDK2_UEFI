/*
  Host-side exhaustive proof for the corrected TRUE-M page-walk policy.

  Reuse the real Moto G20 map allocator/harness in the same translation unit,
  then evaluate the exact 47 leaves used by Mmu3b1Probe.c.  The legacy policy
  must fail first at leaf 19; the corrected privileged-EL2 policy must accept
  all 47 leaves.
*/

#define main  MotoG20Mmu2aBaseHarnessMain
#include "../MMU2ARealMotoG20MapHarness/mmu2a_real_moto_g20_map_harness.c"
#undef main

static int
LookupResult (
  VOID                           *Root,
  UINT64                          Address,
  MOTO_G20_TRANSLATION_LOOKUP    *Result
  )
{
  EFI_STATUS  Status;

  Status = MotoG20LookupTranslation (Root, Address, Result);
  return !EFI_ERROR (Status) && Result->Mapped &&
         (Result->PhysicalAddress == Address);
}

static int
ExecutableWb (
  VOID     *Root,
  UINT64    Address,
  int       Legacy
  )
{
  MOTO_G20_TRANSLATION_LOOKUP  Result;
  UINT64                       Descriptor;

  if (!LookupResult (Root, Address, &Result)) {
    return 0;
  }

  Descriptor = Result.Descriptor;
  return (Legacy ? (Result.Level == 2U) :
                   ((Result.Level >= 2U) && (Result.Level <= 3U))) &&
         ((Descriptor & TT_ATTR_INDX_MASK) == TT_ATTR_INDX_MEMORY_WRITE_BACK) &&
         ((Descriptor & TT_SH_MASK) == TT_SH_INNER_SHAREABLE) &&
         ((Descriptor & TT_AF) != 0) &&
         ((Descriptor & TT_XN_MASK) == 0);
}

static int
WritableWb (
  VOID     *Root,
  UINT64    Address,
  int       Legacy
  )
{
  MOTO_G20_TRANSLATION_LOOKUP  Result;
  UINT64                       Descriptor;

  if (!LookupResult (Root, Address, &Result)) {
    return 0;
  }

  Descriptor = Result.Descriptor;
  return (Result.Level >= 2U) && (Result.Level <= 3U) &&
         ((Descriptor & TT_ATTR_INDX_MASK) == TT_ATTR_INDX_MEMORY_WRITE_BACK) &&
         ((Descriptor & TT_SH_MASK) == TT_SH_INNER_SHAREABLE) &&
         ((Descriptor & TT_AF) != 0) &&
         ((Descriptor & TT_AP_MASK) ==
          (Legacy ? TT_AP_RW_RW : TT_AP_NO_RW));
}

static int
UncachedWritable (
  VOID     *Root,
  UINT64    Address,
  int       Legacy
  )
{
  MOTO_G20_TRANSLATION_LOOKUP  Result;
  UINT64                       Descriptor;

  if (!LookupResult (Root, Address, &Result)) {
    return 0;
  }

  Descriptor = Result.Descriptor;
  return (Result.Level >= 2U) && (Result.Level <= 3U) &&
         ((Descriptor & TT_ATTR_INDX_MASK) == TT_ATTR_INDX_MEMORY_NON_CACHEABLE) &&
         ((Descriptor & TT_AF) != 0) &&
         ((Descriptor & TT_AP_MASK) ==
          (Legacy ? TT_AP_RW_RW : TT_AP_NO_RW)) &&
         ((Descriptor & TT_XN_MASK) != 0);
}

static int
LeafPasses (
  VOID     *Root,
  UINT64    Address,
  int       Legacy
  )
{
  if ((Address >= 0x9E000000ULL) && (Address < 0x9E9E4000ULL)) {
    return UncachedWritable (Root, Address, Legacy);
  }

  if (((Address >= 0x811D0000ULL) && (Address < 0x81200000ULL)) ||
      ((Address >= 0x81008040ULL) && (Address < 0x81008200ULL)) ||
      ((Address >= 0x8197C000ULL) && (Address < 0x81982000ULL))) {
    return WritableWb (Root, Address, Legacy);
  }

  return ExecutableWb (Root, Address, Legacy);
}

static int
HardenFramebufferMap (
  VOID   *Root,
  UINTN  *DescriptorWrites
  )
{
  UINT64  *L1;
  UINT64  *L2;
  UINT64  *L3;
  UINT64   Descriptor;
  UINT64   ExpectedAddress;
  UINTN    Index;
  UINTN    L2FirstIndex;
  UINTN    L2TableIndex;

  *DescriptorWrites = 0;
  L1                = (UINT64 *)(UINTN)0x81980000ULL;
  L2                = (UINT64 *)(UINTN)0x8197F000ULL;
  L3                = (UINT64 *)(UINTN)0x8197C000ULL;
  L2FirstIndex      = (UINTN)((0x9E000000ULL >> 21) & 0x1FFULL);
  L2TableIndex      = L2FirstIndex + 4U;

  Descriptor = ((UINT64 *)Root)[(0x9E000000ULL >> 39) & 0x1FFULL];
  if (((Descriptor & TT_TYPE_MASK) != TT_TYPE_TABLE_ENTRY) ||
      ((Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE) != 0x81980000ULL)) {
    return 0;
  }

  Descriptor = L1[(0x9E000000ULL >> 30) & 0x1FFULL];
  if (((Descriptor & TT_TYPE_MASK) != TT_TYPE_TABLE_ENTRY) ||
      ((Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE) != 0x8197F000ULL)) {
    return 0;
  }

  for (Index = 0; Index < 4U; Index++) {
    Descriptor      = L2[L2FirstIndex + Index];
    ExpectedAddress = 0x9E000000ULL + (Index << 21);
    if (((Descriptor & TT_TYPE_MASK) != TT_TYPE_BLOCK_ENTRY) ||
        ((Descriptor & TT_ADDRESS_MASK_BLOCK_ENTRY) != ExpectedAddress) ||
        ((Descriptor & TT_ATTR_INDX_MASK) != TT_ATTR_INDX_MEMORY_NON_CACHEABLE) ||
        ((Descriptor & TT_SH_MASK) != TT_SH_NON_SHAREABLE) ||
        ((Descriptor & TT_AP_MASK) != TT_AP_NO_RW) ||
        ((Descriptor & TT_AF) == 0) ||
        ((Descriptor & TT_XN_MASK) != 0)) {
      return 0;
    }
  }

  Descriptor = L2[L2TableIndex];
  if (((Descriptor & TT_TYPE_MASK) != TT_TYPE_TABLE_ENTRY) ||
      ((Descriptor & TT_ADDRESS_MASK_DESCRIPTION_TABLE) != 0x8197C000ULL)) {
    return 0;
  }

  for (Index = 0; Index < 0x1E4U; Index++) {
    Descriptor      = L3[Index];
    ExpectedAddress = 0x9E800000ULL + (Index << 12);
    if (((Descriptor & TT_TYPE_MASK) != TT_TYPE_BLOCK_ENTRY_LEVEL3) ||
        ((Descriptor & TT_ADDRESS_MASK_BLOCK_ENTRY) != ExpectedAddress) ||
        ((Descriptor & TT_ATTR_INDX_MASK) != TT_ATTR_INDX_MEMORY_NON_CACHEABLE) ||
        ((Descriptor & TT_SH_MASK) != TT_SH_NON_SHAREABLE) ||
        ((Descriptor & TT_AP_MASK) != TT_AP_NO_RW) ||
        ((Descriptor & TT_AF) == 0) ||
        ((Descriptor & TT_XN_MASK) != 0)) {
      return 0;
    }
  }

  for (Index = 0x1E4U; Index < TT_ENTRY_COUNT; Index++) {
    if (L3[Index] != 0) {
      return 0;
    }
  }

  for (Index = 0; Index < 4U; Index++) {
    L2[L2FirstIndex + Index] |= TT_XN_MASK;
    (*DescriptorWrites)++;
  }

  for (Index = 0; Index < 0x1E4U; Index++) {
    L3[Index] |= TT_XN_MASK;
    (*DescriptorWrites)++;
  }

  return 1;
}

int
main (
  void
  )
{
  static const UINT64 DataPoints[] = {
    0x80080000ULL, 0x80081000ULL, 0x81000000ULL, 0x8100DFFFULL,
    0x81882000ULL, 0x8197C000ULL, 0x81981000ULL, 0x81980000ULL,
    0x8197F000ULL, 0x8197E000ULL, 0x8197D000ULL, 0x8197C000ULL,
    0x811D0000ULL, 0x811E0000ULL - 1ULL, 0x811F0000ULL,
    0x81200000ULL - 1ULL, 0x82000000ULL, 0x82A0A000ULL,
    0x9E000000ULL, 0x9E000FFFULL, 0x9E9E3000ULL, 0x9E9E3FFFULL,
    0x81004000ULL, 0x810047FFULL, 0x81004860ULL, 0x810049A0ULL,
    0x810049A4ULL, 0x81008040ULL, 0x810080D7ULL, 0x81008100ULL,
    0x810081AFULL, 0x81004E80ULL, 0x81004FC0ULL
  };
  VOID       *Root;
  UINTN       TableSize;
  UINTN       AllocatedPages;
  EFI_STATUS  Status;
  UINT64      Address;
  UINTN       Index;
  UINTN       Leaf;
  INTN        LegacyFirstFailure;
  UINTN       CorrectedPassed;
  UINTN       DescriptorWrites;

  gWorkspace = mmap ((void *)(UINTN)WORKSPACE_BASE, WORKSPACE_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                      -1, 0);
  if ((gWorkspace == MAP_FAILED) ||
      ((UINT64)(UINTN)gWorkspace != WORKSPACE_BASE)) {
    fprintf (stderr, "WORKSPACE_MMAP_FAIL errno=%d\n", errno);
    return 2;
  }

  ResetAllocator ();
  Root           = NULL;
  TableSize      = 0;
  AllocatedPages = 0;
  Status = MotoG20BuildTranslationTables (
             gMemoryTable,
             &Root,
             &TableSize,
             &AllocatedPages
             );
  if (EFI_ERROR (Status)) {
    fprintf (stderr, "TABLE_BUILD_FAIL status=0x%llx\n",
             (unsigned long long)Status);
    return 2;
  }

  Leaf               = 0;
  LegacyFirstFailure = -1;
  CorrectedPassed    = 0;
  for (Address = 0x81000000ULL;
       Address < 0x8100E000ULL;
       Address += 0x1000ULL, Leaf++) {
    if ((LegacyFirstFailure < 0) && !ExecutableWb (Root, Address, 1)) {
      LegacyFirstFailure = (INTN)Leaf;
    }
  }

  for (Index = 0; Index < sizeof (DataPoints) / sizeof (DataPoints[0]);
       Index++, Leaf++) {
    Address = DataPoints[Index];
    if ((LegacyFirstFailure < 0) && !LeafPasses (Root, Address, 1)) {
      LegacyFirstFailure = (INTN)Leaf;
    }
  }

  if (!HardenFramebufferMap (Root, &DescriptorWrites)) {
    fprintf (stderr, "FRAMEBUFFER_HARDEN_FAIL\n");
    return 2;
  }

  Leaf = 0;
  for (Address = 0x81000000ULL;
       Address < 0x8100E000ULL;
       Address += 0x1000ULL, Leaf++) {
    if (ExecutableWb (Root, Address, 0)) {
      CorrectedPassed++;
    } else {
      fprintf (stderr, "CORRECTED_FAIL leaf=%u address=0x%llx\n",
               (unsigned)Leaf, (unsigned long long)Address);
    }
  }

  for (Index = 0; Index < sizeof (DataPoints) / sizeof (DataPoints[0]);
       Index++, Leaf++) {
    Address = DataPoints[Index];
    if (LeafPasses (Root, Address, 0)) {
      CorrectedPassed++;
    } else {
      fprintf (stderr, "CORRECTED_FAIL leaf=%u address=0x%llx\n",
               (unsigned)Leaf, (unsigned long long)Address);
    }
  }

  printf ("LEGACY_FIRST_FAILURE_LEAF=%lld\n", (long long)LegacyFirstFailure);
  printf ("LEGACY_FIRST_FAILURE_ADDRESS=0x8197c000\n");
  printf ("CORRECTED_PAGEWALK_PASS=%u TOTAL=%u\n",
          (unsigned)CorrectedPassed, (unsigned)Leaf);
  printf ("FRAMEBUFFER_XN_WRITES=%u\n", (unsigned)DescriptorWrites);
  printf ("TABLE_PAGES=%u ROOT=0x%llx\n",
          (unsigned)AllocatedPages, (unsigned long long)(UINTN)Root);
  munmap (gWorkspace, WORKSPACE_SIZE);
  return ((LegacyFirstFailure == 19) &&
          (CorrectedPassed == 47) &&
          (DescriptorWrites == 488) &&
          (Leaf == 47)) ? 0 : 1;
}

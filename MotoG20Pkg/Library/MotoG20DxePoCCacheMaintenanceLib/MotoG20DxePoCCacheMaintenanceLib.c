/** @file

  DxeCore-only ARM cache maintenance adaptation for the Moto G20.

  The stock InvalidateInstructionCacheRange() cleans data to the Point of
  Unification with DC CVAU before IC IVAU.  Physical telemetry proved that the
  first dynamically loaded DXE image still raised EC=0 at its valid first
  instruction while SCTLR_EL2.I was clear.  This bounded variant changes only
  that clean operation to DC CVAC (Point of Coherency); the range traversal,
  barriers, instruction-cache invalidation, ABI, and every other API remain
  identical to ArmCacheMaintenanceLib.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>

STATIC
VOID
CacheRangeOperation (
  IN  VOID            *Start,
  IN  UINTN           Length,
  IN  LINE_OPERATION  LineOperation,
  IN  UINTN           LineLength
  )
{
  UINTN  ArmCacheLineAlignmentMask;
  UINTN  AlignedAddress;
  UINTN  EndAddress;

  ArmCacheLineAlignmentMask = LineLength - 1;
  AlignedAddress            = (UINTN)Start - ((UINTN)Start & ArmCacheLineAlignmentMask);
  EndAddress                = (UINTN)Start + Length;

  while (AlignedAddress < EndAddress) {
    LineOperation (AlignedAddress);
    AlignedAddress += LineLength;
  }

  ArmDataSynchronizationBarrier ();
}

VOID
EFIAPI
InvalidateInstructionCache (
  VOID
  )
{
  ASSERT (FALSE);
}

VOID
EFIAPI
InvalidateDataCache (
  VOID
  )
{
  ASSERT (FALSE);
}

VOID *
EFIAPI
InvalidateInstructionCacheRange (
  IN  VOID   *Address,
  IN  UINTN  Length
  )
{
  //
  // Bounded Moto G20 delta: publish dirty code to PoC (DC CVAC) rather than
  // only to PoU (DC CVAU).  IC IVAU plus DSB/ISB remain unchanged below.
  //
  CacheRangeOperation (
    Address,
    Length,
    ArmCleanDataCacheEntryByMVA,
    ArmDataCacheLineLength ()
    );
  CacheRangeOperation (
    Address,
    Length,
    ArmInvalidateInstructionCacheEntryToPoUByMVA,
    ArmInstructionCacheLineLength ()
    );

  ArmInstructionSynchronizationBarrier ();

  return Address;
}

VOID
EFIAPI
WriteBackInvalidateDataCache (
  VOID
  )
{
  ASSERT (FALSE);
}

VOID *
EFIAPI
WriteBackInvalidateDataCacheRange (
  IN  VOID   *Address,
  IN  UINTN  Length
  )
{
  CacheRangeOperation (
    Address,
    Length,
    ArmCleanInvalidateDataCacheEntryByMVA,
    ArmDataCacheLineLength ()
    );
  return Address;
}

VOID
EFIAPI
WriteBackDataCache (
  VOID
  )
{
  ASSERT (FALSE);
}

VOID *
EFIAPI
WriteBackDataCacheRange (
  IN  VOID   *Address,
  IN  UINTN  Length
  )
{
  CacheRangeOperation (
    Address,
    Length,
    ArmCleanDataCacheEntryByMVA,
    ArmDataCacheLineLength ()
    );
  return Address;
}

VOID *
EFIAPI
InvalidateDataCacheRange (
  IN  VOID   *Address,
  IN  UINTN  Length
  )
{
  CacheRangeOperation (
    Address,
    Length,
    ArmInvalidateDataCacheEntryByMVA,
    ArmDataCacheLineLength ()
    );
  return Address;
}

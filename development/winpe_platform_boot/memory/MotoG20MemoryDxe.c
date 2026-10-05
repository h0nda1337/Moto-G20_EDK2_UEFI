/** SPDX-License-Identifier: BSD-2-Clause-Patent
  Add only DTB-verified DRAM after CpuDxe can map it. This deliberately leaves
  the proven 66 MiB bootstrap and its fixed EL2 tables unchanged.
  A completed entry point means a published diagnostic, not a successful RAM
  expansion. Consumers MUST inspect Status and PublishedBytes.
**/
#include <Uefi.h>
#include <Guid/FdtHob.h>
#include <Protocol/Cpu.h>
#include <Library/HobLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include "memory_plan.h"
#include "MotoG20MemoryDiagnostic.h"

STATIC EFI_GUID mDiagnosticGuid = MOTO_G20_MEMORY_DIAGNOSTIC_GUID;
STATIC MOTO_G20_MEMORY_DIAGNOSTIC mDiagnostic;
STATIC EFI_HANDLE mHandle;

STATIC EFI_STATUS CheckGcdEmpty (UINT64 Base, UINT64 Length) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS Status;
  UINT64 Cursor = Base, End = Base + Length, Next;
  while (Cursor < End) {
    Status = gDS->GetMemorySpaceDescriptor (Cursor, &D);
    if (EFI_ERROR (Status)) return Status;
    if (D.GcdMemoryType != EfiGcdMemoryTypeNonExistent || D.ImageHandle != NULL ||
        D.DeviceHandle != NULL || D.BaseAddress > Cursor || D.Length == 0 ||
        D.BaseAddress > MAX_UINT64 - D.Length) return EFI_ACCESS_DENIED;
    Next = D.BaseAddress + D.Length;
    if (Next <= Cursor) return EFI_COMPROMISED_DATA;
    Cursor = Next;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ExpandMemory (VOID) {
  EFI_HOB_GUID_TYPE *Hob;
  EFI_CPU_ARCH_PROTOCOL *Cpu;
  EFI_STATUS Status;
  UINT64 FdtAddress, CurrentEl, Tcr;
  UINT32 Total, I;
  struct mp_plan Plan;
  enum mp_result PlanResult;
  CONST UINT8 *Fdt;
  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (CurrentEl));
  if (CurrentEl != 8) return EFI_UNSUPPORTED;
  __asm__ volatile ("mrs %0, TCR_EL2" : "=r" (Tcr));
  if ((Tcr & 63) > 31) return EFI_UNSUPPORTED;
  mDiagnostic.Stage = 1;
  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if (Hob == NULL || GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (UINT64)) return EFI_NOT_FOUND;
  CopyMem (&FdtAddress, GET_GUID_HOB_DATA (Hob), sizeof (FdtAddress));
  /* The board's existing bootstrap explicitly verified/mapped this handoff.
   * Do not dereference an arbitrary pointer from a corrupt or foreign HOB.
   */
  if (FdtAddress != 0x82000000ULL) return EFI_SECURITY_VIOLATION;
  Fdt = (CONST UINT8 *)(UINTN)FdtAddress;
  Total = ((UINT32)Fdt[4] << 24) | ((UINT32)Fdt[5] << 16) | ((UINT32)Fdt[6] << 8) | Fdt[7];
  if (Total < 40 || Total > 0x100000) return EFI_COMPROMISED_DATA;
  PlanResult = mp_make_plan (Fdt, Total, &Plan);
  mDiagnostic.PlanResult = (UINT32)PlanResult;
  if (PlanResult != MP_OK) return EFI_COMPROMISED_DATA;
  mDiagnostic.PlannedBytes = Plan.total;
  mDiagnostic.ReservedRanges = Plan.reserved_count;
  mDiagnostic.Stage = 2;
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status)) return Status;
  for (I = 0; I < Plan.count; ++I) {
    Status = CheckGcdEmpty (Plan.regions[I].base, Plan.regions[I].size);
    if (EFI_ERROR (Status)) return Status;
  }
  /* Map ALL candidates before publishing any byte to the page allocator.
   * CpuDxe uses the architecture MMU library and allocates its new tables
   * from the already existing 66 MiB. No raw TTBR edits and no huge memset.
   */
  mDiagnostic.Stage = 3;
  for (I = 0; I < Plan.count; ++I) {
    Status = Cpu->SetMemoryAttributes (Cpu, Plan.regions[I].base, Plan.regions[I].size, EFI_MEMORY_WB);
    if (EFI_ERROR (Status)) return Status;
    mDiagnostic.MappedBytes += Plan.regions[I].size;
  }
  mDiagnostic.Stage = 4;
  for (I = 0; I < Plan.count; ++I) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeSystemMemory, Plan.regions[I].base,
                                Plan.regions[I].size, EFI_MEMORY_WB);
    if (EFI_ERROR (Status)) return Status;
    /* AddMemorySpace publishes these pages immediately. Never roll back or
     * unmap them after this call: another driver may have allocated them.
     */
    mDiagnostic.PublishedBytes += Plan.regions[I].size;
    mDiagnostic.PublishedRegions++;
    Status = gDS->SetMemorySpaceAttributes (Plan.regions[I].base, Plan.regions[I].size, EFI_MEMORY_WB);
    if (EFI_ERROR (Status)) return Status;
  }
  mDiagnostic.Stage = 5;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI MotoG20MemoryEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
  (VOID)ImageHandle; (VOID)SystemTable;
  ZeroMem (&mDiagnostic, sizeof (mDiagnostic));
  mDiagnostic.Revision = 1; mDiagnostic.Status = EFI_NOT_READY;
  Status = gBS->InstallProtocolInterface (&mHandle, &mDiagnosticGuid, EFI_NATIVE_INTERFACE, &mDiagnostic);
  if (EFI_ERROR (Status)) return Status;
  mDiagnostic.Status = ExpandMemory ();
  return EFI_SUCCESS;
}

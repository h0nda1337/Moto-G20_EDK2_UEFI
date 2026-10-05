/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Optional BDS helper, deliberately NOT an auto-running DXE entry point.
 * Integrate and call after RAM/platform diagnostics and immediately before
 * Windows StartImage. Existing USB/SD diagnostic boots need not call it.
 */
#include <Uefi.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include "cpu_boot_plan.h"

EFI_STATUS EFIAPI MotoG20PrepareWindowsCpu(VOID) {
  struct moto_cpu_state S;struct moto_cpu_plan P;
  ARM_SMC_ARGS Args;UINT32 I;UINT64 Verify,Zero=0;
  ZeroMem(&S,sizeof(S));
  __asm__ volatile("mrs %0, CurrentEL":"=r"(S.el));
  if(S.el!=8) return EFI_UNSUPPORTED;
  __asm__ volatile("mrs %0, SCTLR_EL2":"=r"(S.sctlr));
  __asm__ volatile("mrs %0, TCR_EL2":"=r"(S.tcr));
  __asm__ volatile("mrs %0, CPTR_EL2":"=r"(S.cptr));
  __asm__ volatile("mrs %0, CPACR_EL1":"=r"(S.cpacr));
  __asm__ volatile("mrs %0, CNTHCTL_EL2":"=r"(S.cnthctl));
  __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(S.cntfrq));
  __asm__ volatile("mrs %0, HCR_EL2":"=r"(S.hcr));
  __asm__ volatile("mrs %0, DAIF":"=r"(S.daif));
  __asm__ volatile("mov %0, sp":"=r"(S.sp));
  S.secondaries_off=1;
  for(I=1;I<8;++I) {
    ZeroMem(&Args,sizeof(Args));Args.Arg0=0xc4000004;Args.Arg1=I*0x100;
    ArmCallSmc(&Args);if(Args.Arg0!=1) S.secondaries_off=0;
  }
  if(moto_cpu_plan(&S,&P)!=0) return EFI_NOT_READY;
  /* Publish dirty instruction bytes before invalidating/enabling I-cache.
   * This is not a replacement for the loader's per-image PoC maintenance.
   */
  WriteBackDataCache();
  __asm__ volatile("dsb sy\n\tic iallu\n\tdsb sy\n\tisb":::"memory");
  __asm__ volatile("msr CPTR_EL2, %0\n\tisb"::"r"(P.cptr):"memory");
  __asm__ volatile("msr CPACR_EL1, %0\n\tisb"::"r"(P.cpacr):"memory");
  __asm__ volatile("msr FPCR, %0"::"r"(Zero):"memory");
  __asm__ volatile("msr CNTHCTL_EL2, %0\n\tisb"::"r"(P.cnthctl):"memory");
  __asm__ volatile("msr SCTLR_EL2, %0\n\tisb"::"r"(P.sctlr):"memory");
  __asm__ volatile("mrs %0, SCTLR_EL2":"=r"(Verify));if(Verify!=P.sctlr)return EFI_DEVICE_ERROR;
  __asm__ volatile("mrs %0, CPTR_EL2":"=r"(Verify));if(Verify!=P.cptr)return EFI_DEVICE_ERROR;
  __asm__ volatile("mrs %0, CPACR_EL1":"=r"(Verify));if(Verify!=P.cpacr)return EFI_DEVICE_ERROR;
  __asm__ volatile("mrs %0, CNTHCTL_EL2":"=r"(Verify));if(Verify!=P.cnthctl)return EFI_DEVICE_ERROR;
  __asm__ volatile("mrs %0, FPCR":"=r"(Verify));if(Verify!=0)return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

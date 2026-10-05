/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MOTO_BOOT_PLATFORM_DIAGNOSTIC_H
#define MOTO_BOOT_PLATFORM_DIAGNOSTIC_H
#include <Uefi.h>
#define MOTO_G20_BOOT_PLATFORM_DIAGNOSTIC_GUID \
  { 0x91358670, 0x2318, 0x443b, { 0x9a, 0x8d, 0xd7, 0x49, 0x56, 0x31, 0x31, 0x61 } }
typedef struct _MOTO_G20_BOOT_PLATFORM_DIAGNOSTIC MOTO_G20_BOOT_PLATFORM_DIAGNOSTIC;
struct _MOTO_G20_BOOT_PLATFORM_DIAGNOSTIC {
  UINT32 Revision, Stage;
  EFI_STATUS Status;
  EFI_STATUS RollbackStatus;
  UINT32 InstalledTables, SmbiosRecords, BootStateIssues, PsciVersion;
  UINT64 CurrentEl, Sctlr, Tcr, Cptr, Cpacr, Cnthctl, Cntfrq, Hcr, Mpidr, Daif, Fpcr;
  INT64 SecondaryState[7];
  /* Read-only architecture/PSCI probe. Call immediately before StartImage
   * because an entry-time snapshot does not prove later execution state.
   */
  EFI_STATUS (EFIAPI *RefreshBootState)(MOTO_G20_BOOT_PLATFORM_DIAGNOSTIC *This);
  /* Explicit opt-in only. Refuses missing platform tables; prepares narrow
   * architecture bindings (I/SA/FP/timer) and re-reads the resulting state.
   * Must be called at TPL_APPLICATION after memory is validated, before the
   * Windows loader. It is never invoked by this driver's entry point.
   */
  EFI_STATUS (EFIAPI *PrepareWindowsCpu)(MOTO_G20_BOOT_PLATFORM_DIAGNOSTIC *This);
};
#define MOTO_BOOT_EL_INVALID       BIT0
#define MOTO_BOOT_SCTLR_INVALID    BIT1
#define MOTO_BOOT_TCR_INVALID      BIT2
#define MOTO_BOOT_FP_TRAPS         BIT3
#define MOTO_BOOT_TIMER_ACCESS     BIT4
#define MOTO_BOOT_FREQ_INVALID     BIT5
#define MOTO_BOOT_STAGE2_ACTIVE    BIT6
#define MOTO_BOOT_IRQ_MASKED       BIT7
#define MOTO_BOOT_FPCR_INVALID     BIT8
#define MOTO_BOOT_SECONDARY_ACTIVE BIT9
#endif

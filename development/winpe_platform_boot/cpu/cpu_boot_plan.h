/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MOTO_CPU_BOOT_PLAN_H
#define MOTO_CPU_BOOT_PLAN_H
#include "../memory/memory_plan.h"
struct moto_cpu_state {
  mp_u64 el,sctlr,tcr,cptr,cpacr,cnthctl,cntfrq,hcr,sp,daif;
  mp_u32 secondaries_off;
};
struct moto_cpu_plan { mp_u64 sctlr,cptr,cpacr,cnthctl; };
/* Computes the narrow architectural changes needed before Windows StartImage.
 * No MMIO, SMCCC calls, register writes, cache work, or target interaction.
 */
int moto_cpu_plan(const struct moto_cpu_state *s,struct moto_cpu_plan *out);
#endif

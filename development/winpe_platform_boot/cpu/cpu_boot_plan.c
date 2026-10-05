/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "cpu_boot_plan.h"
int moto_cpu_plan(const struct moto_cpu_state *s,struct moto_cpu_plan *out) {
  if(!s || !out) return 1;
  /* Keep the established EL2/TTBR/cache type/translation regime. Refuse to
   * paper over a missing MMU, disabled data cache, big-endian execution,
   * stage-2 virtualization, VHE, unknown timer frequency or live secondary.
   */
  if(s->el!=8 || (s->sctlr&5)!=5 || (s->sctlr&(1ULL<<25)) ||
     (s->tcr&((3ULL<<14)|(1ULL<<20))) || (s->tcr&63)>31 ||
     (s->hcr&((1ULL<<34)|1)) || !(s->hcr&(1ULL<<31)) ||
     (s->sp&15) || (s->daif&128) || s->cntfrq!=26000000 || !s->secondaries_off) return 2;
  out->sctlr=(s->sctlr|(1ULL<<12)|(1ULL<<3))&~2ULL;
  out->cptr=s->cptr&~(1ULL<<10);
  out->cpacr=s->cpacr|(3ULL<<20);
  out->cnthctl=s->cnthctl|3;
  return 0;
}

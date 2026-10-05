/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdio.h>
#include <string.h>
#include "../cpu/cpu_boot_plan.h"
int main(void) {
  struct moto_cpu_state S={8,0x30c50835,0x80823518,0x400,0,0,26000000,0x80000000,0x87400000,0,1},T;
  struct moto_cpu_plan P;
  unsigned i;
  if(moto_cpu_plan(&S,&P)!=0 || P.sctlr!=(S.sctlr|0x1008) || P.cptr!=0 || P.cpacr!=0x300000 || P.cnthctl!=3) return 1;
  /* All unspecified fields must be preserved, including CNTHCTL gates not
   * owned here and platform implementation-specific SCTLR/CPTR bits.
   */
  T=S;T.cnthctl=0xab00;T.cptr=0x80300400;T.cpacr=0xdead0000;
  if(moto_cpu_plan(&T,&P)!=0 || P.cnthctl!=0xab03 || P.cptr!=0x80300000 || P.cpacr!=(T.cpacr|0x300000)) return 1;
  for(i=0;i<13;++i) {
    T=S;
    switch(i) {
      case 0:T.el=4;break;case 1:T.sctlr&=~1ULL;break;case 2:T.sctlr&=~4ULL;break;
      case 3:T.sctlr|=1ULL<<25;break;case 4:T.tcr|=1ULL<<14;break;case 5:T.tcr|=1ULL<<20;break;
      case 6:T.tcr=(T.tcr&~63ULL)|32;break;case 7:T.hcr|=1;break;case 8:T.hcr|=1ULL<<34;break;
      case 9:T.hcr&=~(1ULL<<31);break;case 10:T.sp|=8;break;case 11:T.cntfrq=24000000;break;
      case 12:T.secondaries_off=0;break;
    }
    memset(&P,0,sizeof(P));if(moto_cpu_plan(&T,&P)==0 || P.sctlr!=0) return 1;
  }
  T=S;T.daif=128;if(moto_cpu_plan(&T,&P)==0) return 1;
  if(moto_cpu_plan(NULL,&P)==0 || moto_cpu_plan(&S,NULL)==0) return 1;
  puts("PASS CPU plan: baseline, field preservation, 14 unsafe preconditions, null arguments");return 0;
}

// Redirect only the VBAR installation callsite. The isolated assembly wrapper
// initializes X18 and tail-branches to the physically frozen helper.
#define MotoG20Mmu3b1WriteVbarReadback MotoG20Mmu3c2hWriteVbarReadback
#include "../MMU3B1/Mmu3b1Probe.c"

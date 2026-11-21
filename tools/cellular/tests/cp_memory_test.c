#include <assert.h>
#include <stdio.h>
#include "../../../xnu/files/bsd/sys/redwood_cp_memory.h"
int main(void) {
 assert(cp_memory_certificate(1,CP_IPC_BASE,CP_IPC_SIZE));
 assert(!cp_memory_certificate(0,CP_IPC_BASE,CP_IPC_SIZE));
 assert(!cp_memory_certificate(1,CP_IPC_BASE+4096,CP_IPC_SIZE));
 assert(!cp_memory_certificate(1,CP_IPC_BASE,CP_IPC_SIZE-4096));
 for(uint64_t p=CP_IPC_BASE;p<CP_IPC_BASE+CP_IPC_SIZE;p+=4096) {
  uint64_t par=(UINT64_C(0x44)<<56)|p;
  assert(cp_memory_par_nc(par,p));
  assert(!cp_memory_par_nc(par|1,p));
  assert(!cp_memory_par_nc(par,p+4096));
  assert(!cp_memory_par_nc((UINT64_C(0xff)<<56)|p,p));
  assert(!cp_memory_par_nc(p,p));
 }
 puts("CP memory certificate/PAR: all2048 IPC pages, faults, wrong PA, cached and Device aliases passed");
}

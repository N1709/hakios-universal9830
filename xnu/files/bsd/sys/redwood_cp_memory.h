/* MIT. S20 retained IPC-memory contract. No memory access or DMA grants. */
#ifndef REDWOOD_CP_MEMORY_H
#define REDWOOD_CP_MEMORY_H
#include <stdint.h>
#define CP_IPC_BASE UINT64_C(0xf0000000)
#define CP_IPC_SIZE UINT64_C(0x800000)
#define CP_MEMORY_VERSION 1u
static inline int cp_memory_certificate(uint64_t version,uint64_t base,uint64_t size)
{return version==CP_MEMORY_VERSION && base==CP_IPC_BASE && size==CP_IPC_SIZE;}
static inline int cp_memory_par_matches(uint64_t par,uint64_t physical)
{
    return !(par&1) && (par&UINT64_C(0x0000fffffffff000))==(physical&UINT64_C(0x0000fffffffff000));
}
static inline int cp_memory_par_nc(uint64_t par,uint64_t physical)
{return cp_memory_par_matches(par,physical) && (par>>56)==0x44;}
#endif

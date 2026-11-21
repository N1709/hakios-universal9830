/* MIT. Fixed S20 IPC debug ABI. Root only; no power or DMA controls. */
#ifndef REDWOOD_CP_IPC_H
#define REDWOOD_CP_IPC_H
#include <stddef.h>
#include <stdint.h>
#define CP_IPC_API 1u
#define CP_IPC_CHUNK 2080u
#define CP_IPC_SPAN 0x800000u
/* offset bit 31 selects the second window: the CP's packet processor area (pktproc, 0xd0000000, 2 MiB:
 * descriptors and DL buffers), so a packet is one call instead of a page-mapping debug peek per 256 bytes */
#define CP_IPC_PP_FLAG 0x80000000u
#define CP_IPC_PP_BASE 0xd0000000ull
#define CP_IPC_PP_SPAN 0x200000u
enum cp_ipc_op { CP_IPC_INFO=0, CP_IPC_MAP=1, CP_IPC_UNMAP=2,
    CP_IPC_READ=3, CP_IPC_WRITE=4, CP_IPC_LOAD32=5, CP_IPC_STORE32=6 };
struct cp_ipc_request { uint32_t version,op,offset,length; uint8_t data[CP_IPC_CHUNK]; };
struct cp_ipc_reply { uint32_t version,mapped,length,reserved; uint8_t data[CP_IPC_CHUNK]; };
#define CP_IPC_HEADER 16u
static inline int cp_ipc_request_valid(const struct cp_ipc_request *q,size_t bytes)
{
    if(!q || bytes<CP_IPC_HEADER || q->version!=CP_IPC_API || q->op>CP_IPC_STORE32) return 0;
    if(q->op<=CP_IPC_UNMAP)
        return bytes==CP_IPC_HEADER && !q->offset && !q->length;
    uint32_t off=q->offset&~CP_IPC_PP_FLAG,span=(q->offset&CP_IPC_PP_FLAG)?CP_IPC_PP_SPAN:CP_IPC_SPAN;
    if(!q->length || q->length>CP_IPC_CHUNK || off>span ||
        q->length>span-off) return 0;
    if(q->op>=CP_IPC_LOAD32 && (q->length!=4 || (off&3))) return 0;
    return bytes==CP_IPC_HEADER+
        ((q->op==CP_IPC_WRITE || q->op==CP_IPC_STORE32)?q->length:0);
}
/* Avoid phystokv and cached direct-map aliases: only disjoint apertures.
 * Overflow/empty physical aperture is not sufficient mapping evidence. */
static inline int cp_ipc_aperture_disjoint(uint64_t base,uint64_t size)
{
    if(!size || size>UINT64_MAX-base) return 0;
    return base>=UINT64_C(0xf0800000) || base+size<=UINT64_C(0xf0000000);
}
#endif

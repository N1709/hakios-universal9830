#include <assert.h>
#include <stdio.h>
#include "../../../xnu/files/bsd/sys/redwood_cp_ipc.h"
int main(void)
{
    struct cp_ipc_request q={.version=CP_IPC_API};
    assert(sizeof(q)==CP_IPC_HEADER+CP_IPC_CHUNK);
    for(unsigned op=0;op<=2;op++) {q.op=op;assert(cp_ipc_request_valid(&q,16));}
    q.op=CP_IPC_WRITE;
    for(unsigned n=1;n<=CP_IPC_CHUNK;n++) {
        q.length=n;q.offset=CP_IPC_SPAN-n;
        assert(cp_ipc_request_valid(&q,16+n));
        assert(!cp_ipc_request_valid(&q,15+n));
        assert(!cp_ipc_request_valid(&q,17+n));
        q.offset++;assert(!cp_ipc_request_valid(&q,16+n));
    }
    q.length=4;q.offset=UINT32_MAX;assert(!cp_ipc_request_valid(&q,20));
    q.op=CP_IPC_LOAD32;q.offset=0x18;assert(cp_ipc_request_valid(&q,16));
    q.offset=0x19;assert(!cp_ipc_request_valid(&q,16));
    q.op=CP_IPC_STORE32;q.offset=0x24;assert(cp_ipc_request_valid(&q,20));
    assert(!cp_ipc_request_valid(&q,16));
    q.version=2;assert(!cp_ipc_request_valid(&q,20));
    assert(!cp_ipc_request_valid(NULL,16));
    assert(cp_ipc_aperture_disjoint(0x880000000ULL,0x17e000000ULL));
    assert(cp_ipc_aperture_disjoint(0xe0000000,0x10000000));
    assert(cp_ipc_aperture_disjoint(0xf0800000,0x1000));
    assert(!cp_ipc_aperture_disjoint(0xf0000000,0x800000));
    assert(!cp_ipc_aperture_disjoint(0xf0000000-1,2));
    assert(!cp_ipc_aperture_disjoint(0xf0800000-1,2));
    assert(!cp_ipc_aperture_disjoint(UINT64_MAX,1));
    assert(!cp_ipc_aperture_disjoint(0,0));
    puts("CP IPC ABI: all2080 chunk limits, exact input sizes, atomic alignment, aperture overlaps and overflow passed");
}

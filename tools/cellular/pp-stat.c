/* MIT. Print the pktproc control words and the descriptors around fore/rear (through the kernel's packet window). */
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include "../../xnu/files/bsd/sys/redwood_cp_ipc.h"
static int rd(uint32_t off,void *out,uint32_t n)
{
    struct cp_ipc_request q={.version=CP_IPC_API,.op=CP_IPC_READ,.offset=CP_IPC_PP_FLAG|off,.length=n};
    struct cp_ipc_reply r;size_t sz=sizeof(r);
    if (sysctlbyname("kern.redwood.cp_ipc",&r,&sz,&q,CP_IPC_HEADER)) return -1;
    memcpy(out,r.data,n);return 0;
}
int main(void)
{
    uint32_t w[6];if (rd(0,w,24)) {perror("cp_ipc");return 1;}
    printf("info %08x desc_base %08x num %u data_base %08x fore %u rear %u\n",w[0],w[1],w[2],w[3],w[4],w[5]);
    unsigned n=w[2]?w[2]:256;
    for (unsigned k=0;k<6;k++) {
        unsigned idx=(w[5]+n-3+k)%n;uint32_t d[4];rd(0x100+16*idx,d,16);
        printf("desc %3u: addr %08x ctl %08x len %08x ch %08x%s\n",idx,d[0],d[1],d[2],d[3],idx==w[5]?"  <- rear":idx==w[4]?"  <- fore":"");
    }
    return 0;
}

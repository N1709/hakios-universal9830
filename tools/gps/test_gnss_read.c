/* Host refusal checks: never sends to a device. */
#define main gnss_capture_main
#include "gnss-read.c"
#undef main
#include <assert.h>
int main(void)
{
    uint8_t bytes[16]={0};dev=-1;
    errno=0;assert(read_memory(0,17,bytes)==-1 && errno==EINVAL);
    errno=0;assert(read_memory(0,0,bytes)==-1 && errno==EINVAL);
    /* Actual live RPC8 vector reply prefix and request byte ordering. */
    const uint8_t reply[]={0,0,0,0,0x10,0,0x30,0x3a,0x91,0};
    assert(le32(reply)==0 && le16(reply+4)==16 && le32(reply+6)==0x913a30);
    put32(bytes,0x20000);put16(bytes+4,16);
    const uint8_t expected[]={0,0,2,0,16,0};
    assert(!memcmp(bytes,expected,sizeof(expected)));
    puts("GNSS read: invalid/oversized reads rejected before I/O; captured reply fields verified");
    return 0;
}

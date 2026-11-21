#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../s5123-udl.h"

int main(void)
{
    uint8_t wire[S5123_UDL_WIRE_MAX+1], data[2048];
    for(unsigned i=0;i<sizeof(data);i++) data[i]=(uint8_t)(i*73+9);
    const uint8_t toc_start[]={0xfc,0xd7,8,0,0,0x91,0,0};
    const uint8_t main_crc[]={0xfc,0xd7,12,0,0x0c,0x92,0,0,0x78,0x56,0x34,0x12,0,0,0,0};
    assert(s5123_udl_command(wire,sizeof(wire),1,S5123_UDL_START,0)==8);
    assert(!memcmp(wire,toc_start,sizeof(toc_start)));
    assert(s5123_udl_command(wire,sizeof(wire),2,S5123_UDL_CRC,0x12345678)==16);
    assert(!memcmp(wire,main_crc,sizeof(main_crc)));
    assert(s5123_udl_command(wire,sizeof(wire),15,S5123_UDL_START,0)==8);
    assert(wire[5]==0x9f);
    /* Exhaust every partial final chunk; compare to an independent layout. */
    for(unsigned len=1;len<=2048;len++) {
        memset(wire,0xa5,sizeof(wire));
        size_t n=s5123_udl_data(wire,sizeof(wire),2,2048+len,2,data,len);
        size_t expected=((20+len+7)/8)*8;
        const uint8_t prefix[]={0xfc,0xd7,0,0,1,0x92,0,0,2,0,0,0,2,0,0,0};
        assert(n==expected && wire[n]==0xa5);
        assert(!memcmp(wire,prefix,2) && !memcmp(wire+4,prefix+4,12));
        assert(wire[2]==(uint8_t)(20+len) && wire[3]==(20+len)/256);
        assert(wire[16]==(uint8_t)len && wire[17]==len/256 && !wire[18] && !wire[19]);
        assert(!memcmp(wire+20,data,len));
        for(size_t i=20+len;i<n;i++) assert(wire[i]==0);
        memset(wire,0xa5,sizeof(wire));
        assert(!s5123_udl_data(wire,n-1,2,2048+len,2,data,len));
        for(size_t i=0;i<sizeof(wire);i++) assert(wire[i]==0xa5);
    }
    /* Largest u32 image: ceiling count and offset must not wrap. */
    assert(s5123_udl_data(wire,sizeof(wire),2,UINT32_MAX,2097152,data,2047));
    assert(s5123_udl_get32(wire+8)==2097152);
    assert(!s5123_udl_data(wire,sizeof(wire),2,1,0,data,1));
    assert(!s5123_udl_data(wire,sizeof(wire),2,1,2,data,1));
    assert(!s5123_udl_data(wire,sizeof(wire),2,0,1,data,1));
    assert(!s5123_udl_data(wire,sizeof(wire),2,1,1,data,2));
    assert(!s5123_udl_data(wire,sizeof(wire),2,1,1,NULL,1));
    assert(!s5123_udl_data(NULL,sizeof(wire),2,1,1,data,1));
    for(unsigned stage=0;stage<256;stage++) {
        if(stage>=1 && stage<=12) continue;
        assert(!s5123_udl_data(wire,sizeof(wire),stage,1,1,data,1));
        assert(!s5123_udl_command(wire,sizeof(wire),stage,S5123_UDL_DONE,0));
    }
    assert(!s5123_udl_command(wire,sizeof(wire),1,(enum s5123_udl_op)2,0));
    uint8_t ack[]={0xfc,0xd7,8,0,0,0xa1,0,0};
    assert(s5123_udl_response(ack,8,1,S5123_UDL_START)==S5123_UDL_ACK);
    for(size_t n=0;n<8;n++) assert(s5123_udl_response(ack,n,1,S5123_UDL_START)==S5123_UDL_MALFORMED);
    assert(s5123_udl_response(ack,9,1,S5123_UDL_START)==S5123_UDL_MALFORMED);
    assert(s5123_udl_response(NULL,8,1,S5123_UDL_START)==S5123_UDL_MALFORMED);
    assert(s5123_udl_response(ack,8,2,S5123_UDL_START)==S5123_UDL_UNEXPECTED);
    ack[4]=15;assert(s5123_udl_response(ack,8,1,S5123_UDL_START)==S5123_UDL_PEER_FAIL);
    ack[4]=1;assert(s5123_udl_response(ack,8,1,S5123_UDL_SEND)==S5123_UDL_ACK);
    ack[0]=0xf8;assert(s5123_udl_response(ack,8,1,S5123_UDL_SEND)==S5123_UDL_ACK);
    ack[0]=0xfe;assert(s5123_udl_response(ack,8,1,S5123_UDL_SEND)==S5123_UDL_MALFORMED);
    ack[0]=0xfc;ack[1]=0;assert(s5123_udl_response(ack,8,1,S5123_UDL_SEND)==S5123_UDL_MALFORMED);
    ack[1]=215;ack[2]=7;assert(s5123_udl_response(ack,8,1,S5123_UDL_SEND)==S5123_UDL_MALFORMED);
    puts("SIPC UDL: golden commands, all2048 final lengths/padding/capacity guards, u32 maximum and malformed/failed ACKs passed");
}

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../../xnu/files/bsd/sys/redwood_spi8_debug.h"
int main(void)
{
    uint8_t data[64];char hex[131];
    assert(spi8_decode("007f80FF",data)==4 && data[0]==0 && data[1]==127 && data[2]==128 && data[3]==255);
    memset(hex,'a',128);hex[128]=0;assert(spi8_decode(hex,data)==64);
    hex[128]='a';hex[129]=0;assert(!spi8_decode(hex,data));
    assert(!spi8_decode("",data) && !spi8_decode("0",data) && !spi8_decode("00 ",data) && !spi8_decode("0g",data));
    assert(spi8_fifo_ready(2,0,0,0,64,1));
    assert(!spi8_fifo_ready(2,0,3,0,1,1));
    assert(!spi8_fifo_ready(2,0,0,1u<<6,64,1));
    assert(spi8_fifo_ready(2,0,0,1u<<6,63,1));
    assert(spi8_fifo_ready(2,0,3,64u<<15,64,0));
    assert(!spi8_fifo_ready(2,0,3,63u<<15,64,0));
    for (unsigned tx=0;tx<2;tx++) {
        assert(!spi8_fifo_ready(0,0,0,64u<<15,1,tx));
        assert(!spi8_fifo_ready(2,1u<<29,0,64u<<15,1,tx));
        assert(!spi8_fifo_ready(2,2u<<17,0,64u<<15,1,tx));
        assert(!spi8_fifo_ready(2,2,0,64u<<15,1,tx));
        assert(!spi8_fifo_ready(2,0,0,(64u<<15)|4,1,tx));
        assert(!spi8_fifo_ready(2,0,0,65u<<15,1,tx));
        assert(!spi8_fifo_ready(2,0,0,65u<<6,1,tx));
        assert(!spi8_fifo_ready(2,0,0,0,0,tx));
        assert(!spi8_fifo_ready(2,0,0,0,65,tx));
    }
    puts("SPI8 debug contract: strict hex, FIFO bounds, inactive TX, byte/no-DMA mode and error guards passed");
}

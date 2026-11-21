/* MIT. SPI16 debug FIFO command bounds/decoder, shared with host tests. */
#ifndef REDWOOD_SPI8_DEBUG_H
#define REDWOOD_SPI8_DEBUG_H
#include <stdint.h>
#include <stddef.h>
static inline int spi8_hex_digit(char c)
{
    return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
}
static inline unsigned spi8_decode(const char *hex,uint8_t data[64])
{
    size_t length=0;
    while (hex[length]) {if (++length>128) return 0;}
    if (!length || length&1) return 0;
    for (unsigned i=0;i<length/2;i++) {
        int hi=spi8_hex_digit(hex[i*2]),lo=spi8_hex_digit(hex[i*2+1]);
        if (hi<0 || lo<0) return 0;
        data[i]=(uint8_t)((hi<<4)|lo);
    }
    return (unsigned)length/2;
}
static inline int spi8_fifo_ready(uint32_t usi,uint32_t mode,uint32_t channel,uint32_t status,
    unsigned count,int transmit)
{
    unsigned tx=(status>>6)&0x7f,rx=(status>>15)&0x7f;
    if (usi!=2 || (mode&((3u<<29)|(3u<<17)|6u)) || (status&0x3c) ||
        count<1 || count>64 || tx>64 || rx>64) return 0;
    return transmit ? !(channel&3) && count<=64-tx : count<=rx;
}
#endif

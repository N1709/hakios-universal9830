/* MIT. Controller model: byte order, FIFO bounds, chip select, RX draining,
 * failures/deadlines and one-attempt latch. No firmware or physical MMIO.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../s5123-spi.h"

enum { NORMAL, RX_STALL, NO_TX_DONE, ERROR_FLAG, BAD_FIFO, RESET_STALL, FROZEN_CLOCK };
struct model {
    uint8_t fifo[64], wire[131072];
    uint32_t fifo_count, rx_count, packet_count, ch, cs;
    uint32_t writes, reads, rx_drained, max_fifo, packets, auto_cs, mode_writes;
    size_t wire_length;
    uint64_t time;
    int fault;
};

static uint32_t read32(void *context, uint32_t offset)
{
    struct model *m=context;
    assert(offset==S5123_STATUS);
    m->reads++;
    if (m->ch & 0x20) return m->fault==RESET_STALL ? (1u<<6) : 0;
    if (m->fault==ERROR_FLAG) return 1u<<3;
    if (m->fault==BAD_FIFO) return 65u<<15;
    uint32_t status=m->rx_count<<15;
    if (m->fault==RX_STALL || m->fault==FROZEN_CLOCK) status=0;
    if (m->fault!=NO_TX_DONE) status|=1u<<25;
    return status;
}
static uint8_t read8(void *context, uint32_t offset)
{
    struct model *m=context;
    assert(offset==S5123_RX && m->rx_count);
    m->rx_count--;
    m->rx_drained++;
    return 0xa5;
}
static void write8(void *context, uint32_t offset, uint8_t value)
{
    struct model *m=context;
    assert(offset==S5123_TX && m->ch==0 && m->fifo_count<64);
    m->fifo[m->fifo_count++]=value;
    if (m->fifo_count>m->max_fifo) m->max_fifo=m->fifo_count;
    m->writes++;
}
static void write32(void *context, uint32_t offset, uint32_t value)
{
    struct model *m=context;
    m->writes++;
    switch (offset) {
    case S5123_CH:
        assert(value==0 || value==3 || value==0x20); /* Master/mode0, no HS. */
        m->ch=value;
        if (value==0x20) {m->fifo_count=0;m->rx_count=0;}
        if (value==3) {
            assert(m->cs==0x23 && m->packet_count==m->fifo_count && m->fifo_count);
            assert(m->wire_length+m->fifo_count<=sizeof(m->wire));
            memcpy(m->wire+m->wire_length,m->fifo,m->fifo_count);
            m->wire_length+=m->fifo_count;
            m->rx_count=m->fifo_count;
            m->fifo_count=0;
            m->packets++;
        }
        break;
    case S5123_MODE:
        assert(value==(0x3ffu<<19)); /* No DMA/burst/loopback; 8-bit bus/channel. */
        m->mode_writes++;
        break;
    case S5123_CS:
        assert(value==1 || value==0x23);
        m->cs=value;
        m->auto_cs+=value==0x23;
        break;
    case S5123_PACKET:
        assert(value==0 || ((value&0xffff)>0 && (value&0xffff)<=64 && !(value&~0x1ffffu)));
        m->packet_count=value&0xffff;
        break;
    case S5123_PENDING: assert(value==0 || value==0x1f);break;
    case S5123_IRQ: case S5123_SWAP: case S5123_FEEDBACK: case S5123_USI_CON:
        assert(value==0);break;
    default: assert(!"unexpected register write (including CLK_CFG)");
    }
}
static uint64_t now_us(void *context)
{
    struct model *m=context;
    return m->fault==FROZEN_CLOCK ? 0 : m->time++;
}
static struct s5123_spi_ops operations(struct model *m)
{
    return (struct s5123_spi_ops){m,read32,read8,write32,write8,now_us};
}
static void success(size_t length, int near_timer_wrap)
{
    struct model *m=calloc(1,sizeof(*m));assert(m);
    if (near_timer_wrap) m->time=UINT64_MAX-20;
    struct s5123_spi_ops ops=operations(m);
    struct s5123_spi_result result={0};
    uint8_t *data=malloc(length);assert(data);
    for (size_t i=0;i<length;i++)data[i]=(uint8_t)(i*37+11);
    assert(s5123_spi_send_boot(&ops,&result,data,length,1625000,2000000,1)==0);
    assert(result.completed_bytes==length && result.queued_bytes==length);
    assert(result.packets==(length+63)/64 && m->packets==result.packets);
    assert(m->wire_length==length && memcmp(m->wire,data,length)==0);
    assert(m->rx_drained==length && m->max_fifo<=64 && m->ch==0 && m->cs==1);
    assert(m->auto_cs==result.packets && m->mode_writes==1);
    unsigned writes=m->writes;
    assert(s5123_spi_send_boot(&ops,&result,data,length,1625000,2000000,1)==S5123_SPI_ALREADY_ATTEMPTED);
    assert(m->writes==writes);
    free(data);free(m);
}
static void failure(int fault)
{
    struct model *m=calloc(1,sizeof(*m));assert(m);m->fault=fault;
    struct s5123_spi_ops ops=operations(m);
    struct s5123_spi_result result={0};
    uint8_t data[64]={0};
    int expected=(fault==ERROR_FLAG || fault==BAD_FIFO) ? S5123_SPI_IO : S5123_SPI_TIMEOUT;
    assert(s5123_spi_send_boot(&ops,&result,data,sizeof(data),1625000,1000,1)==expected);
    assert(result.error==expected && result.attempted && !result.completed_bytes);
    assert(m->ch==0 && m->cs==1 && !m->packet_count);
    assert(m->reads<=16777218);
    unsigned writes=m->writes;
    assert(s5123_spi_send_boot(&ops,&result,data,sizeof(data),1625000,1000,1)==S5123_SPI_ALREADY_ATTEMPTED);
    assert(m->writes==writes);
    free(m);
}
int main(void)
{
    const size_t lengths[]={1,63,64,65,64496,65536,131072};
    for (size_t i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++)success(lengths[i],0);
    success(65,1);
    for (int fault=RX_STALL;fault<=FROZEN_CLOCK;fault++)failure(fault);
    struct model *m=calloc(1,sizeof(*m));assert(m);
    struct s5123_spi_ops ops=operations(m);
    struct s5123_spi_result result={0};uint8_t data[1]={0};
    assert(s5123_spi_send_boot(&ops,&result,data,1,1625000,1000,0)==S5123_SPI_NOT_READY);
    assert(s5123_spi_send_boot(&ops,&result,data,0,1625000,1000,1)==S5123_SPI_ARGUMENT);
    assert(s5123_spi_send_boot(&ops,&result,data,131073,1625000,2000000,1)==S5123_SPI_ARGUMENT);
    assert(s5123_spi_send_boot(&ops,&result,data,1,2166667,1000,1)==S5123_SPI_ARGUMENT);
    assert(s5123_spi_send_boot(&ops,&result,data,1,1625000,0,1)==S5123_SPI_ARGUMENT);
    assert(s5123_spi_send_boot(&ops,&result,data,1,1625000,5000001,1)==S5123_SPI_ARGUMENT);
    assert(!m->writes && !m->reads && !result.attempted);
    free(m);
    puts("SPI PIO model: 8 complete byte-stream cases, 6 failure/cleanup cases, input/readiness guards passed");
}

/* MIT. Live SPI16 INTERNAL loopback through fixed byte-FIFO debug commands.
 * No CP power, GPIO writes, firmware, DMA or external SPI pad ownership.
 * The callback core is the same byte-transport core proven in UEFI.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef LIVE_SPI_TEST
#include <sys/sysctl.h>
#else
int sysctlbyname(const char *,void *,size_t *,void *,size_t);
#endif
#include "s5123-spi.h"
#include "../../xnu/files/bsd/sys/redwood_spi8_debug.h"
static volatile sig_atomic_t stopped;
static int cleanup;
static void stop_signal(int s) {(void)s;stopped=1;}
static int request(const char *command,char response[8192])
{
    if (stopped && !cleanup) return -1;
    size_t length=8191;
    if (sysctlbyname("kern.redwood.poke",response,&length,(void *)command,strlen(command))) return -1;
    response[length<8192?length:8191]=0;return 0;
}
static int read_word(uint64_t address,uint32_t *value)
{
    char command[96],response[8192];unsigned long long got;int used=0;
    snprintf(command,sizeof(command),"r 0x%llx 1",(unsigned long long)address);
    return request(command,response) || sscanf(response,"%llx: %x %n",&got,value,&used)!=2 ||
        got!=address || response[used] ? -1:0;
}
static int write_word(uint64_t address,uint32_t value,int verify)
{
    char command[96],response[8192];unsigned long long got;unsigned old,next,back;int used=0;
    int allowed=address==0x10701820 || address==0x10700690 || address==0x10721048;
    static const unsigned offsets[]={0,8,12,16,32,36,40,44,196};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++)
        if (address==0x10960000+offsets[i]) allowed=1;
    if (!allowed) return -1;
    snprintf(command,sizeof(command),"w 0x%llx 0x%x",(unsigned long long)address,value);
    return request(command,response) || sscanf(response,"%llx: %x -> %x, reads %x %n",
        &got,&old,&next,&back,&used)!=4 || got!=address || response[used] ||
        next!=value || (verify && back!=value) ? -1:0;
}
static int clock_wait(uint64_t address,uint32_t mask,uint32_t value)
{
    for (unsigned i=0;i<100;i++) {uint32_t got;if (read_word(address,&got)) return 0;
        if ((got&mask)==value) return 1;if (usleep(10)) return 0;}
    return 0;
}
static int restore_word(uint64_t address,uint32_t value,int verify)
{
    for (unsigned i=0;i<3;i++) if (!write_word(address,value,verify)) return 0;
    return -1;
}
struct spi_state {
    uint8_t tx[64],rx[64];unsigned tx_length,rx_length,rx_at;
    uint32_t status,received,mismatches;int io_error,active,internal;
    const uint8_t *expected;size_t length;
};
static int fifo_tx(struct spi_state *s)
{
    if (!s->tx_length) return 0;
    char command[160]="spi8.tx ",response[8192];unsigned wrote;int used=0;
    for (unsigned i=0;i<s->tx_length;i++) snprintf(command+8+2*i,sizeof(command)-8-2*i,"%02x",s->tx[i]);
    int error=request(command,response) || sscanf(response,"spi8.tx: wrote %u %n",&wrote,&used)!=1 ||
        response[used] || wrote!=s->tx_length;
    s->tx_length=0;return error?-1:0;
}
static uint32_t spi_read32(void *context,uint32_t offset)
{
    struct spi_state *s=context;uint32_t value;
    if (s->io_error || read_word(0x10960000+offset,&value)) {s->io_error=1;return 0x3c;}
    if (offset==S5123_STATUS) s->status=value;
    return value;
}
static uint8_t spi_read8(void *context,uint32_t offset)
{
    struct spi_state *s=context;
    if (offset!=S5123_RX || s->io_error) {s->io_error=1;return 0;}
    if (s->rx_at==s->rx_length) {
        unsigned count=(s->status>>15)&0x7f;char command[40],response[8192];
        if (!count || count>64) {s->io_error=1;return 0;}
        snprintf(command,sizeof(command),"spi8.rx %u",count);
        if (request(command,response) || strlen(response)!=count*2+1 || response[count*2]!='\n') {s->io_error=1;return 0;}
        response[count*2]=0;
        if (spi8_decode(response,s->rx)!=count) {s->io_error=1;return 0;}
        s->rx_length=count;s->rx_at=0;
    }
    uint8_t value=s->rx[s->rx_at++];
    if (s->active) {
        if (s->expected && (s->received>=s->length || value!=s->expected[s->received])) s->mismatches++;
        s->received++;
    }
    return value;
}
static void spi_write32(void *context,uint32_t offset,uint32_t value)
{
    struct spi_state *s=context;
    if (offset==S5123_MODE && s->internal) value|=8;
    if (offset==S5123_CH) {
        if (value==3 && fifo_tx(s)) s->io_error=1;
        if (!value || value==0x20) {s->tx_length=0;s->rx_length=0;s->rx_at=0;}
        s->active=value==3;
        if (s->io_error && value==3) return;
    }
    if (write_word(0x10960000+offset,value,offset!=S5123_PENDING)) s->io_error=1;
}
static void spi_write8(void *context,uint32_t offset,uint8_t value)
{
    struct spi_state *s=context;
    if (offset!=S5123_TX || s->io_error || s->tx_length==64) {s->io_error=1;return;}
    s->tx[s->tx_length++]=value;
}
static uint64_t now_us(void *context)
{
    (void)context;struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000+(uint64_t)ts.tv_nsec/1000;
}
#ifndef LIVE_SPI_ADAPTER_ONLY
static int loopback(size_t length)
{
    const unsigned regs[]={S5123_CH,S5123_MODE,S5123_CS,S5123_IRQ,S5123_PACKET,S5123_SWAP,S5123_FEEDBACK,S5123_USI_CON};
    uint32_t saved[8],mux,divider,mode,pins,power,active,g0,g1;
    int changed_clock=0,changed_mode=0,saved_controller=0,passed=0,restored=1;
    struct spi_state state={0};struct s5123_spi_result result={0};
    uint8_t *pattern=malloc(length);if (!pattern) return 1;
    for (size_t i=0;i<length;i++) pattern[i]=(uint8_t)(0x35+17*i);
    state.expected=pattern;state.length=length;state.internal=1;
    puts("cellular live SPI: read owner/clock/CP-off guards");fflush(stdout);
    if (read_word(0x10700690,&mux) || read_word(0x10701820,&divider) || read_word(0x10721048,&mode) ||
        read_word(0x10730060,&pins) || read_word(0x15c30284,&power) || read_word(0x15c30384,&active) ||
        read_word(0x107020fc,&g0) || read_word(0x1070208c,&g1)) goto finish;
    if (mode || (pins&0xffff0000u) || (power&1) || (active&1) ||
        (mux&(1u<<16)) || (divider&(1u<<16)) || !(g0&(1u<<21)) || !(g1&(1u<<21))) goto finish;
    changed_clock=1;
    if (write_word(0x10701820,(divider&~15u)|3,0) || !clock_wait(0x10701820,(1u<<16)|15,3) ||
        write_word(0x10700690,mux&~16u,0) || !clock_wait(0x10700690,(1u<<16)|16,0)) goto finish;
    changed_mode=1;
    if (write_word(0x10721048,2,1)) goto finish;
    for (unsigned i=0;i<8;i++) if (read_word(0x10960000+regs[i],&saved[i])) goto finish;
    saved_controller=1;
    if ((saved[0]&3) || saved[3] || saved[4]) goto finish;
    puts("cellular live SPI: internal pattern transfer; no external pin/control writes");fflush(stdout);
    struct s5123_spi_ops ops={&state,spi_read32,spi_read8,spi_write32,spi_write8,now_us};
    uint64_t start=now_us(NULL);
    int error=s5123_spi_send_boot(&ops,&result,pattern,length,1625000,5000000,1);
    passed=!error && !state.io_error && result.completed_bytes==length && state.received==length && !state.mismatches;
    printf("cellular live SPI: error %d io_error %d completed %u received %u mismatch %u packets %u\n",
        error,state.io_error,result.completed_bytes,state.received,state.mismatches,result.packets);fflush(stdout);
    printf("cellular live SPI: elapsed_us %llu\n",(unsigned long long)(now_us(NULL)-start));fflush(stdout);
finish:
    cleanup=1;puts("cellular live SPI: restore controller/USI/clocks");fflush(stdout);
    if (saved_controller) {
        if (restore_word(0x10960000,0,1)) restored=0;
        if (restore_word(0x10960020,0,1)) restored=0;
        if (restore_word(0x10960000,0x20,0)) restored=0;
        if (restore_word(0x10960000,0,1)) restored=0;
        uint32_t status;
        if (read_word(0x10960014,&status) || (status&((0x7fu<<6)|(0x7fu<<15)))) restored=0;
        for (unsigned i=1;i<8;i++) if (restore_word(0x10960000+regs[i],saved[i],1)) restored=0;
        if (restore_word(0x10960000,saved[0],1)) restored=0;
    }
    if (changed_mode && restore_word(0x10721048,mode,1)) restored=0;
    if (changed_clock) {
        if (restore_word(0x10701820,divider,0)) restored=0;
        if (!clock_wait(0x10701820,(1u<<16)|15,divider&15)) restored=0;
        if (restore_word(0x10700690,mux,0)) restored=0;
        if (!clock_wait(0x10700690,(1u<<16)|16,mux&16)) restored=0;
        uint32_t value;if (read_word(0x10730060,&value) || value!=pins) restored=0;
    }
    printf("cellular live SPI: loopback_pass %d cleanup_verified %d; CP unchanged\n",passed,restored);fflush(stdout);
    free(pattern);return restored?(passed?0:1):2;
}
int main(int argc,char **argv)
{
    signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);signal(SIGHUP,stop_signal);
    size_t length=16;
    if (argc==2) {char *end;unsigned long value=strtoul(argv[1],&end,10);
        if (*end || !value || value>131072) return 2;length=(size_t)value;}
    else if (argc!=1) return 2;
    char response[8192];
    if (request("spi8.rx 0",response) || strcmp(response,"spi8: invalid byte count/hex\n")) {
        fputs("SPI8 kernel hooks unavailable; no hardware changes\n",stderr);return 1;
    }
    return loopback(length);
}
#endif

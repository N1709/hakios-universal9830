/* Host tests for the real userspace byte-burst adapter and cleanup. */
#define _DEFAULT_SOURCE
#define LIVE_SPI_TEST 1
#include <assert.h>
#define main live_spi_program_main
#include "../live-spi.c"
#undef main
static struct {uint64_t address;uint32_t value;} regs[32];
static unsigned nregs,calls,fail_at,failed,tx_count,rx_count,rx_at;
static uint8_t tx[64],rx[64];static size_t wire_bytes;
static int after_effect,bad_rx;
static uint32_t *reg(uint64_t address)
{
    for (unsigned i=0;i<nregs;i++) if (regs[i].address==address) return &regs[i].value;
    assert(nregs<32);regs[nregs].address=address;return &regs[nregs++].value;
}
static uint32_t read_model(uint64_t address)
{
    if (address==0x10960014) return (rx_count<<15)|(tx_count<<6)|(1u<<25);
    return *reg(address);
}
int sysctlbyname(const char *name,void *old,size_t *length,void *new_data,size_t new_length)
{
    assert(!strcmp(name,"kern.redwood.poke") && new_length<160);
    char command[160],response[8192];memcpy(command,new_data,new_length);command[new_length]=0;
    int fault=++calls==fail_at;if (fault) failed++;
    if (fault && !after_effect) {errno=EIO;return -1;}
    if (!strncmp(command,"spi8.tx ",8)) {
        assert(!tx_count && !(*reg(0x10960000)&3));
        tx_count=spi8_decode(command+8,tx);assert(tx_count);
        snprintf(response,sizeof(response),"spi8.tx: wrote %u\n",tx_count);
    } else if (!strncmp(command,"spi8.rx ",8)) {
        unsigned count=(unsigned)strtoul(command+8,NULL,10);assert(count && count<=rx_count);
        for (unsigned i=0;i<count;i++) snprintf(response+2*i,sizeof(response)-2*i,"%02x",rx[rx_at++]);
        if (bad_rx) response[0]='g';
        strcat(response,"\n");rx_count-=count;
    } else {
        char op;unsigned long long address;unsigned value=0;
        int fields=sscanf(command,"%c %llx %x",&op,&address,&value);assert(fields>=2);
        uint32_t before=read_model(address);
        if (op=='r') snprintf(response,sizeof(response),"0x%llx: %08x\n",address,before);
        else {
            assert(op=='w' && fields==3);
            *reg(address)=address==0x10960024?0:value; /* W1C readback0 is allowed. */
            if (address==0x10960000) {
                if (value==0x20) tx_count=rx_count=rx_at=0;
                if (value==3) {
                    assert((*reg(0x10960008)&8) && tx_count && tx_count<=64);
                    assert((*reg(0x10960020)&0xffff)==tx_count);
                    memcpy(rx,tx,tx_count);rx_count=tx_count;rx_at=0;wire_bytes+=tx_count;tx_count=0;
                }
            }
            snprintf(response,sizeof(response),"0x%llx: %08x -> %08x, reads %08x\n",address,before,value,read_model(address));
        }
    }
    if (fault) {errno=EIO;return -1;}
    assert(*length>=strlen(response)+1);memcpy(old,response,strlen(response)+1);*length=strlen(response)+1;return 0;
}
static void setup(void)
{
    memset(regs,0,sizeof(regs));nregs=0;calls=failed=tx_count=rx_count=rx_at=0;
    wire_bytes=0;bad_rx=0;stopped=0;cleanup=0;
    *reg(0x10700690)=0x10;*reg(0x107020fc)=*reg(0x1070208c)=1u<<21;
}
static void restored(void)
{
    assert(*reg(0x10700690)==0x10 && *reg(0x10701820)==0 && *reg(0x10721048)==0);
    assert(*reg(0x10730060)==0 && *reg(0x15c30284)==0 && *reg(0x15c30384)==0);
    assert(*reg(0x10960000)==0 && *reg(0x10960008)==0 && !rx_count && !tx_count);
}
int main(void)
{
    assert(freopen("/dev/null","w",stdout));setup();fail_at=0;
    assert(loopback(16)==0 && wire_bytes==16);restored();unsigned stages=calls;
    for (unsigned effect=0;effect<2;effect++) for (unsigned at=1;at<=stages;at++) {
        setup();fail_at=at;after_effect=(int)effect;
        int result=loopback(16);assert(failed==1 && result>=0 && result<=2);
        if (result!=2) restored();
    }
    const size_t sizes[]={1,63,64,65,64496};
    for (unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
        setup();fail_at=0;assert(loopback(sizes[i])==0 && wire_bytes==sizes[i]);restored();
    }
    setup();fail_at=0;bad_rx=1;assert(loopback(16)!=0);restored();
    setup();*reg(0x10721048)=1;assert(loopback(16)==1 && wire_bytes==0);
    fprintf(stderr,"Live SPI adapter: byte-order/burst cases through64496 bytes, %u I/O faults, invalid RX and owner refusal passed\n",stages*2);
}

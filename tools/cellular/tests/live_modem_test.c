/* Host tests for the real userspace byte-burst adapter and cleanup. */
#define _DEFAULT_SOURCE
#define LIVE_SPI_TEST 1
#include <assert.h>
#define LIVE_MODEM_TEST 1
#define nanosleep modem_test_sleep
#include "../live-modem.c"
#undef nanosleep
int modem_test_sleep(const struct timespec *a,struct timespec *b) {(void)a;(void)b;return 0;}
static struct {uint64_t address;uint32_t value;} regs[64];
static unsigned nregs,calls,fail_at,failed,tx_count,rx_count,rx_at;
static uint8_t tx[64],rx[64];static size_t wire_bytes;
static int after_effect,bad_rx;
static uint32_t *reg(uint64_t address)
{
    for (unsigned i=0;i<nregs;i++) if (regs[i].address==address) return &regs[i].value;
    assert(nregs<64);regs[nregs].address=address;return &regs[nregs++].value;
}
static uint32_t read_model(uint64_t address)
{
    if (address==0x10960014) return (rx_count<<15)|(tx_count<<6)|(1u<<25);
    if (address==0x15c30124 && !(*reg(0x15c30120)&15)) return *reg(address)|1;
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
        char op;unsigned long long address;unsigned value=0,or_value=0;
        int fields=sscanf(command,"%c %llx %x %x",&op,&address,&value,&or_value);assert(fields>=2);
        uint32_t before=read_model(address);
        if (op=='r') snprintf(response,sizeof(response),"0x%llx: %08x\n",address,before);
        else {
            assert((op=='w' && fields==3) || (op=='m' && fields==4));
            if (op=='m') value=(before&value)|or_value;
            *reg(address)=address==0x10960024?0:value; /* W1C readback0 is allowed. */
            if (address==0x10960000) {
                if (value==0x20) tx_count=rx_count=rx_at=0;
                if (value==3) {
                    assert(!(*reg(0x10960008)&8) && tx_count && tx_count<=64);
                    assert((*reg(0x15c30284)&1) && (*reg(0x15c30124)&1));
                    assert((*reg(0x10730060)&0xffff0000u)==0x22220000u);
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
    *reg(0x10700690)=0x10;
    *reg(0x107300c0)=0x20;*reg(0x107300c4)=2; /* Bluetooth neighbor preserved */
    *reg(0x10730068)=0x55550000;
    *reg(0x15c30124)=1; /* reset input is initially pulled high */*reg(0x107020fc)=*reg(0x1070208c)=1u<<21;
}
static int attempt(struct s5123_boot_result *result)
{
    uint8_t image[16];for (unsigned i=0;i<sizeof(image);i++) image[i]=(uint8_t)i;
    struct modem m={0};struct s5123_boot_ops ops={&m,ready,prepare,output,delay,route,send_boot,park,restore,quiescent};
    return s5123_boot_once(&ops,result,image,sizeof(image),s5123_boot_crc32(image,sizeof(image)));
}
static void restored(const struct s5123_boot_result *result)
{
    assert(*reg(0x10700690)==0x10 && *reg(0x10701820)==0 && *reg(0x10721048)==0);
    assert(*reg(0x10730060)==0 && *reg(0x10730068)==0x55550000);
    assert(*reg(0x107300c0)==0x20 && *reg(0x107300c4)==2);
    assert(*reg(0x10960000)==0 && *reg(0x10960008)==0 && !rx_count && !tx_count);
    if (result->power_requested) {
        assert((*reg(0x15c30120)&15)==1 && !(*reg(0x15c30124)&1));
        assert((*reg(0x15c30280)&15)==1 && !(*reg(0x15c30284)&1));
    }
}
int main(void)
{
    signal(SIGINT,stop_signal);
    assert(freopen("/dev/null","w",stdout));setup();fail_at=0;
    struct s5123_boot_result result={0};
    assert(attempt(&result)==0 && wire_bytes==16 && result.cleanup_safe);restored(&result);
    unsigned stages=calls;
    for (unsigned effect=0;effect<2;effect++) for (unsigned at=1;at<=stages;at++) {
        setup();fail_at=at;after_effect=(int)effect;result=(struct s5123_boot_result){0};
        int error=attempt(&result);assert(failed==1 && error<=0);
        if (result.cleanup_safe) restored(&result);
        /* Every transient injected failure after power must still shut CP down. */
        if (result.power_requested) assert(quiescent(NULL));
    }
    setup();fail_at=0;*reg(0x10721048)=1;result=(struct s5123_boot_result){0};
    assert(attempt(&result)==S5123_BOOT_GUARD && !wire_bytes && !result.attempted);
    setup();*reg(0x15c30120)=1;*reg(0x15c30124)=0;*reg(0x15c30280)=1;
    result=(struct s5123_boot_result){0};assert(attempt(&result)==0);restored(&result);
    setup();bad_rx=1;result=(struct s5123_boot_result){0};assert(attempt(&result)!=0);restored(&result);
    fprintf(stderr,"Live modem adapter: %u before/after I/O faults, CP shutdown, pad/BT preservation, owner guard and held-low restart passed\n",stages*2);
}

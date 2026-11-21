/* Host failure injection for the real live helper; no phone access. */
#define _DEFAULT_SOURCE
#define LIVE_PCIE_TEST 1
#include <assert.h>
#define main live_program_main
#include "../live-pcie.c"
#undef main

static struct { uint64_t address;uint32_t value; } registers[256];
static unsigned register_count,calls,fail_at,failed;
static int after_effect,permanent_restore_fault;
static uint32_t *reg(uint64_t address)
{
    for (unsigned i=0;i<register_count;i++) if (registers[i].address==address) return &registers[i].value;
    assert(register_count<256);
    registers[register_count].address=address;
    return &registers[register_count++].value;
}
int sysctlbyname(const char *name,void *old,size_t *old_length,void *new_data,size_t new_length)
{
    assert(!strcmp(name,"kern.redwood.poke") && new_length<128);
    char command[128],operation;unsigned long long address;unsigned a=0,b=0;
    memcpy(command,new_data,new_length);command[new_length]=0;
    int fields=sscanf(command,"%c %llx %x %x",&operation,&address,&a,&b);
    assert(fields>=2);
    uint32_t *value=reg(address);uint32_t before=*value;
    int fault=(++calls==fail_at);
    if (fault) failed++;
    if (permanent_restore_fault && cleaning && operation=='m' && address==0x15860720) fault=1;
    if (fault && !after_effect) {errno=EIO;return -1;}
    char response[256];
    if (operation=='r') snprintf(response,sizeof(response),"0x%llx: %08x\n",address,*value);
    else {
        assert(operation=='m' && fields==4);
        *value=(*value&a)|b;
        snprintf(response,sizeof(response),"0x%llx: %08x -> %08x, reads %08x\n",address,before,*value,*value);
    }
    if (fault) {errno=EIO;return -1;}
    assert(*old_length>=strlen(response)+1);
    memcpy(old,response,strlen(response)+1);*old_length=strlen(response)+1;
    return 0;
}
static void setup(void)
{
    memset(registers,0,sizeof(registers));register_count=0;calls=0;failed=0;
    interrupted=0;cleaning=0;permanent_restore_fault=0;
    phy_requested=0;
    for (uint64_t a=0x13c02018;a<=0x13c02034;a+=4) *reg(a)=1u<<21;
    *reg(0x13800000)=0x9831144d;
    *reg(0x13d500d8)=0x6d;
    for (unsigned i=0;i<sizeof(s5123_phy_evt1_ops)/sizeof(s5123_phy_evt1_ops[0]);i++) {
        uint64_t address=s5123_phy_evt1_ops[i].address;
        *reg(address)=0x11000000u|(uint32_t)(address&0xffff);
    }
}
static void restored(void)
{
    assert(*reg(0x15860720)==0 && *reg(0x13c30000)==0 && *reg(0x13c30004)==0 && *reg(0x13d500d8)==0x6d);
    for (unsigned i=0;i<sizeof(s5123_phy_evt1_ops)/sizeof(s5123_phy_evt1_ops[0]);i++) {
        uint64_t address=s5123_phy_evt1_ops[i].address;
        assert(*reg(address)==(0x11000000u|(uint32_t)(address&0xffff)));
    }
}
int main(void)
{
    assert(freopen("/dev/null","w",stdout));
    setup();fail_at=0;assert(root_id()==0);restored();unsigned stages=calls;
    for (unsigned effect=0;effect<2;effect++) for (unsigned at=1;at<=stages;at++) {
        setup();fail_at=at;after_effect=(int)effect;
        int result=root_id();
        assert(failed==1);restored();
        /* A final readback failure reports unverified cleanup even when the
         * model's underlying register values were successfully restored. */
        assert(result>=0 && result<=2);
    }
    setup();fail_at=0;phy_requested=1;assert(root_id()==0);restored();unsigned phy_stages=calls;
    for (unsigned effect=0;effect<2;effect++) for (unsigned at=1;at<=phy_stages;at++) {
        setup();fail_at=at;after_effect=(int)effect;phy_requested=1;
        int result=root_id();assert(failed==1);restored();assert(result>=0 && result<=2);
    }
    setup();fail_at=0;*reg(0x15c30284)=1;assert(root_id()==1);assert(*reg(0x15860720)==0);
    setup();permanent_restore_fault=1;after_effect=0;assert(root_id()==2);assert(*reg(0x15860720)==1);
    fprintf(stderr,"Live PCIe helper: ROOT %u and PHY %u before/after-side-effect faults, owner refusal and permanent cleanup failure passed\n",stages*2,phy_stages*2);
}

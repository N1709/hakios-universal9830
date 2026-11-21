/* SPDX-License-Identifier: GPL-2.0-only
 * Live S20 PCIe diagnostics. Original diagnostic code is MIT; the combined
 * helper includes Samsung's GPL-2.0-only PHY sequence (s5123-phy-evt1.h).
 * Default: prerequisite reads only. --root-id: CP off, temporary PERST/isolation/
 * PHY input-clock changes, ROOT config reads, then verified cleanup. No link,
 * ATU, firmware, endpoint, DMA or shared HSI2/UFS clock writes.
 * Every phase is flushed to stdout BEFORE I/O for the host evidence file.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef LIVE_PCIE_TEST
#include <sys/sysctl.h>
#else
int sysctlbyname(const char *,void *,size_t *,void *,size_t);
#endif
#include <unistd.h>
#include "s5123-phy-evt1.h"

static volatile sig_atomic_t interrupted;
static int cleaning;
static int phy_requested;
static void on_signal(int signal_number) { (void)signal_number;interrupted=1; }
static int request(const char *command,char response[8192])
{
    if (interrupted && !cleaning) return -1;
    size_t length=8191;
    if (sysctlbyname("kern.redwood.poke",response,&length,(void *)command,strlen(command))) return -1;
    response[length<8192?length:8191]=0;
    return 0;
}
static int read_word(uint64_t address,uint32_t *value)
{
    char command[96],response[8192];unsigned long long got;int consumed=0;
    snprintf(command,sizeof(command),"r 0x%llx 1",(unsigned long long)address);
    if (request(command,response) || sscanf(response,"%llx: %x %n",&got,value,&consumed)!=2 ||
        got!=address || response[consumed]) return -1;
    return 0;
}
static int mask_word(uint64_t address,uint32_t and_mask,uint32_t or_mask)
{
    /* Single kernel-side RMW, protected by dbgport mutex. */
    char command[128],response[8192];unsigned long long got;unsigned old,next,readback;int consumed=0;
    int allowed=address==0x13c30000 || address==0x13c30004 || address==0x15860720 || address==0x13d500d8;
    for (unsigned i=0;i<sizeof(s5123_phy_evt1_ops)/sizeof(s5123_phy_evt1_ops[0]);i++)
        if (s5123_phy_evt1_ops[i].address==address) allowed=1;
    if (!allowed) return -1;
    snprintf(command,sizeof(command),"m 0x%llx 0x%x 0x%x",(unsigned long long)address,and_mask,or_mask);
    if (request(command,response) || sscanf(response,"%llx: %x -> %x, reads %x %n",
        &got,&old,&next,&readback,&consumed)!=4 || got!=address || response[consumed] ||
        next!=((old&and_mask)|or_mask) || readback!=next) return -1;
    return 0;
}
static void phase(const char *text) { printf("cellular live: %s\n",text);fflush(stdout); }
static int restore_word(uint64_t address,uint32_t and_mask,uint32_t or_mask)
{
    for (unsigned attempt=0;attempt<3;attempt++)
        if (!mask_word(address,and_mask,or_mask)) return 0;
    return -1;
}
static int phy_exercise(void)
{
    struct {uint64_t address;uint32_t value;int touched;} saved[128]={0};
    unsigned count=0;int error=1,safe=1;
    const unsigned operations=sizeof(s5123_phy_evt1_ops)/sizeof(s5123_phy_evt1_ops[0]);
    for (unsigned i=0;i<operations;i++) {
        uint64_t address=s5123_phy_evt1_ops[i].address;unsigned j;
        for (j=0;j<count;j++) if (saved[j].address==address) break;
        if (j==count) {
            if (count==128) return 1;
            saved[count++].address=address;
        }
    }
    /* Clear all-powerdown before other PHY reads, matching Samsung poweron.
     * Keep its original value for last-in-cleanup restoration. */
    phase("PHY snapshot powerdown control");
    if (read_word(saved[0].address,&saved[0].value)) return 1;
    saved[0].touched=1;
    if (mask_word(saved[0].address,0,s5123_phy_evt1_ops[0].value)) goto restore;
    for (unsigned i=1;i<count;i++) {
        printf("cellular live: PHY snapshot %llx\n",(unsigned long long)saved[i].address);fflush(stdout);
        if (read_word(saved[i].address,&saved[i].value)) goto restore;
    }
    for (unsigned i=1;i<operations;i++) {
        const struct s5123_phy_op *op=&s5123_phy_evt1_ops[i];
        unsigned j=0;while (j<count && saved[j].address!=op->address) j++;
        if (j==count) goto restore;
        saved[j].touched=1;
        printf("cellular live: PHY step %u write %llx=%x delay_us=%u\n",i,
            (unsigned long long)op->address,op->value,op->delay_us);fflush(stdout);
        if (mask_word(op->address,0,op->value) || (op->delay_us && usleep(op->delay_us))) goto restore;
    }
    error=0;
restore:;
    int was_cleaning=cleaning;cleaning=1;
    phase("PHY restore saved registers, powerdown control last");
    for (unsigned i=count;i>0;i--) if (saved[i-1].touched) {
        printf("cellular live: PHY restore %llx\n",(unsigned long long)saved[i-1].address);fflush(stdout);
        if (restore_word(saved[i-1].address,0,saved[i-1].value)) safe=0;
    }
    cleaning=was_cleaning;
    printf("cellular live: PHY sequence_pass %d restore_verified %d operations %u\n",!error,safe,operations);fflush(stdout);
    return safe?error:2;
}
static int root_id(void)
{
    uint32_t pmu,con,dat,power,active,clock=0,value,id=0,ltssm;
    int pins_changed=0,pmu_changed=0,clock_changed=0,error=1,safe=1;
    phase("read ownership/power guards");
    if (read_word(0x15860720,&pmu) || read_word(0x13c30000,&con) || read_word(0x13c30004,&dat) ||
        read_word(0x15c30284,&power) || read_word(0x15c30384,&active)) return 1;
    if ((pmu&1) || (con&0xff) || (power&1) || (active&1)) {
        phase("guard refused: owner or powered modem; no changes");return 1;
    }
    for (uint64_t a=0x13c02018;a<=0x13c02034;a+=4) {
        if (read_word(a,&value) || !(value&(1u<<21))) {
            phase("clock guard refused; no changes");return 1;
        }
    }
    printf("cellular live: saved PMU %08x CON %08x DAT %08x\n",pmu,con,dat);fflush(stdout);
    phase("assert PERST low, preserve CLKREQ/other pins");
    pins_changed=1; /* A failed response can follow a real write. */
    if (mask_word(0x13c30004,~2u,0) || mask_word(0x13c30000,~0xf0u,0x10u)) goto cleanup;
    phase("temporarily bypass PHY isolation; CP power remains off");
    pmu_changed=1;
    if (mask_word(0x15860720,~1u,1)) goto cleanup;
    if (usleep(1000)) goto cleanup;
    phase("before ELBI LTSSM read13d20054");
    if (read_word(0x13d20054,&ltssm) || ltssm) goto cleanup;
    phase("before PHY input-clock read13d500d8");
    if (read_word(0x13d500d8,&clock)) goto cleanup;
    phase("select PHY input-clock28");
    clock_changed=1;
    if (mask_word(0x13d500d8,0,0x28)) goto cleanup;
    phase("before ROOT identity read13800000");
    if (read_word(0x13800000,&id)) goto cleanup;
    printf("cellular live: ROOT identity %08x (not modem endpoint)\n",id);fflush(stdout);
    if (!id || id==0xffffffffu) goto cleanup;
    if (phy_requested) {
        int result=phy_exercise();
        if (result==2) safe=0;
        if (result) goto cleanup;
    }
    phase("before ROOT command/status read13800004");
    if (read_word(0x13800004,&value)) goto cleanup;
    printf("cellular live: ROOT command/status %08x\n",value);fflush(stdout);
    phase("before ELBI link-state read13d202c8");
    if (read_word(0x13d202c8,&value)) goto cleanup;
    printf("cellular live: link state %08x\n",value);fflush(stdout);
    error=0;
cleanup:
    cleaning=1;
    phase("cleanup: restore input clock/isolation/PERST ownership");
    if (clock_changed && restore_word(0x13d500d8,0,clock)) safe=0;
    if (pmu_changed && restore_word(0x15860720,~1u,pmu&1)) safe=0;
    if (pins_changed) {
        if (restore_word(0x13c30000,~0xf0u,con&0xf0u)) safe=0;
        if (restore_word(0x13c30004,~2u,dat&2)) safe=0;
    }
    if (read_word(0x15860720,&value) || value!=pmu) safe=0;
    if (read_word(0x13c30000,&value) || value!=con) safe=0;
    if (read_word(0x13c30004,&value) || (value&2)!=(dat&2)) safe=0;
    printf("cellular live: access_pass %d cleanup_verified %d; CP/SIM/LTE unproven\n",!error,safe);fflush(stdout);
    return safe ? error : 2;
}
static int status(void)
{
    static const uint64_t addresses[]={0x15860720,0x13c30000,0x13c30004,0x13c30008,
        0x13c00600,0x13c00610,0x13c02018,0x13c0201c,0x13c02020,0x13c02024,
        0x13c02028,0x13c0202c,0x13c02030,0x13c02034,0x15c30284,0x15c30384};
    phase("prerequisite reads only; no isolated controller access");
    for (unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);i++) {
        uint32_t value;
        if (read_word(addresses[i],&value)) return 1;
        printf("0x%llx: %08x\n",(unsigned long long)addresses[i],value);fflush(stdout);
    }
    return 0;
}
int main(int argc,char **argv)
{
    signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGHUP,on_signal);
    if (argc==1 || (argc==2 && !strcmp(argv[1],"--status"))) return status();
    if (argc==2 && !strcmp(argv[1],"--root-id")) return root_id();
    if (argc==2 && !strcmp(argv[1],"--phy-test")) {phy_requested=1;return root_id();}
    fprintf(stderr,"usage: live-pcie [--status | --root-id | --phy-test]\n");return 2;
}

/* SPDX-License-Identifier: GPL-2.0-only
 * Samsung Exynos9830 bounded link bring-up adaptation; see NOTICE.
 * Derived from Samsung Electronics 2019 PCIe driver and DesignWare setup.
 * Pinned: ExtremeXT/android_kernel_samsung_exynos990@69515fbb7a4395898c05a8624f76a12afbac11c5.
 * No DMA, inbound ATU, BME enable or IRQ enable. Endpoint BME may be cleared. Caller powers CP and
 * guarantees CP off before restore. An MMIO stall is not recoverable here.
 */
#ifndef S5123_LIVE_LINK_H
#define S5123_LIVE_LINK_H
#include "s5123-phy-evt1.h"
static const struct s5123_phy_op s5123_ia_ops[]={
    {0x13d20388,0x400,0},
    {0x13d00030,0x13d20000,0},
    {0x13d00034,0x13d50000,0},
    {0x13d00038,0x13d00000,0},
    {0x13d00040,0x00023000,0},
    {0x13d00100,0x50000004,0},
    {0x13d00104,0x00000100,0},
    {0x13d00108,0x20420008,0},
    {0x13d0010c,0x00000100,0},
    {0x13d00110,0x50000004,0},
    {0x13d00114,0x00000400,0},
    {0x13d00118,0x10000008,0},
    {0x13d0011c,0x00000000,0},
    {0x13d00120,0x40020008,0},
    {0x13d00124,0x00000100,0},
    {0x13d00128,0x50000004,0},
    {0x13d0012c,0x000000f0,0},
    {0x13d00130,0x20d10fc0,0},
    {0x13d00134,0x000000f0,0},
    {0x13d00138,0x40010a48,0},
    {0x13d0013c,0x00000020,0},
    {0x13d00140,0x40010a48,0},
    {0x13d00144,0x00000030,0},
    {0x13d00148,0x40010a48,0},
    {0x13d0014c,0x00000000,0},
    {0x13d00150,0x40010bf4,0},
    {0x13d00154,0x00000005,0},
    {0x13d00158,0x40010bf4,0},
    {0x13d0015c,0x00000004,0},
    {0x13d00160,0x30010fc0,0},
    {0x13d00164,0x000000f0,0},
    {0x13d00168,0x40010bf4,0},
    {0x13d0016c,0x00000005,0},
    {0x13d00170,0x40000008,0},
    {0x13d00174,0x00000400,0},
    {0x13d00178,0x80000000,0},
    {0x13d0017c,0x00000000,0},
    {0x13d00000,0x00000001,0},
};
struct s5123_link_io {
    int (*read)(uint64_t,uint32_t *);
    int (*write)(uint64_t,uint32_t,uint32_t,int); /* address, mask, bits, verify */
    int (*wait)(uint32_t);
    void (*phase)(const char *);
};
struct s5123_link_saved {uint64_t address;uint32_t mask,value;};
struct s5123_link {
    struct s5123_link_io io;
    struct s5123_link_saved saved[192];unsigned count;
    int pins_changed,isolated_changed,ltssm_touched,ia_touched,parked;
    uint32_t endpoint_id,endpoint_command,endpoint_header,endpoint_class,root_command,root_header,link_status;
};
static const uint64_t s5123_link_extra_addresses[]={
    0x13c30000,0x13c30004,0x13c30008,0x13c3000c,0x15860720,0x13d500d8,0x13d40180,
    0x13d20054,0x13d203a4,0x13d21404,0x13d21408,
    0x13800890,0x13d203bc,0x13d203a0,0x13d203a8,0x13d20474,
    0x13800710,0x1380080c,0x138000a0,0x13800018,0x13800b40,0x13800b44,
    0x13b00000,0x13b00004,0x13b00008,0x13b0000c,0x13b00010,0x13b00014,0x13b00018
};
static int s5123_link_allowed(uint64_t address)
{
    for (unsigned i=0;i<sizeof(s5123_link_extra_addresses)/sizeof(s5123_link_extra_addresses[0]);i++)
        if (s5123_link_extra_addresses[i]==address) return 1;
    for (unsigned i=0;i<sizeof(s5123_phy_evt1_ops)/sizeof(s5123_phy_evt1_ops[0]);i++)
        if (s5123_phy_evt1_ops[i].address==address) return 1;
    for (unsigned i=0;i<sizeof(s5123_ia_ops)/sizeof(s5123_ia_ops[0]);i++)
        if (s5123_ia_ops[i].address==address) return 1;
    return 0;
}
static int s5123_link_save(struct s5123_link *l,uint64_t address,uint32_t mask)
{
    if (!s5123_link_allowed(address)) return -1;
    unsigned i;for (i=0;i<l->count;i++) if (l->saved[i].address==address) break;
    if (i==l->count) {
        if (l->count==192) return -1;
        uint32_t original;if (l->io.read(address,&original)) return -1;
        l->saved[l->count++]=(struct s5123_link_saved){address,mask,original};
    } else if (mask&~l->saved[i].mask) return -1;
    return 0;
}
static int s5123_link_change(struct s5123_link *l,uint64_t address,uint32_t mask,uint32_t value,int verify)
{
    if ((value&~mask) || s5123_link_save(l,address,mask)) return -1;
    /* Snapshot precedes write; a failed response may follow a real effect. */
    return l->io.write(address,mask,value,verify);
}
static int s5123_link_guard(struct s5123_link *l)
{
    uint32_t v;
    if (l->io.read(0x15860720,&v) || (v&1) ||
        l->io.read(0x13c30000,&v) || (v&0xff)) return -1;
    for (uint64_t a=0x13c02018;a<=0x13c02034;a+=4)
        if (l->io.read(a,&v) || !(v&(1u<<21))) return -1;
    return 0;
}
static int s5123_link_begin(struct s5123_link *l)
{
    uint32_t value;
    l->io.phase("PCIe assert PERST; bypass isolation; select known input clock");
    /* Park may touch both registers even after a failed first write/readback.
     * Snapshot both before allowing cleanup to take ownership. */
    if (s5123_link_save(l,0x13c30004,2) || s5123_link_save(l,0x13c30000,0xff)) return -1;
    l->pins_changed=1;
    if (s5123_link_change(l,0x13c30004,2,0,0) ||
        s5123_link_change(l,0x13c30008,0xf,3,1) ||
        s5123_link_change(l,0x13c3000c,0xff,0x33,1) ||
        s5123_link_change(l,0x13c30000,0xff,0x12,1)) return -1;
    if (l->io.read(0x13c30004,&value) || (value&2)) return -1;
    l->isolated_changed=1;
    if (s5123_link_change(l,0x15860720,1,1,1) || l->io.wait(1000) ||
        l->io.read(0x13d20054,&value) || value ||
        s5123_link_change(l,0x13d500d8,~0u,0x28,1)) return -1;
    l->io.phase("PCIe configure EVT1 PHY and IA CDR; CP still held in PERST");
    /* Clear powerdown before reading other PHY registers, matching proven live probe. */
    for (unsigned i=0;i<sizeof(s5123_phy_evt1_ops)/sizeof(s5123_phy_evt1_ops[0]);i++) {
        const struct s5123_phy_op *op=&s5123_phy_evt1_ops[i];
        if (s5123_link_change(l,op->address,~0u,op->value,1) || (op->delay_us && l->io.wait(op->delay_us))) return -1;
    }
    if (l->io.read(0x13d00000,&value) || value) return -1;
    /* Every IA instruction is saved before writes; disable engine before restore. */
    for (unsigned i=0;i<sizeof(s5123_ia_ops)/sizeof(s5123_ia_ops[0]);i++) {
        const struct s5123_phy_op *op=&s5123_ia_ops[i];
        if (op->address==0x13d00000) l->ia_touched=1;
        if (s5123_link_change(l,op->address,~0u,op->value,1)) return -1;
    }
    l->io.phase("PCIe release PCS/PHY reset and configure Gen1 x2 root; no busmaster enable");
    if (s5123_link_change(l,0x13d40180,~0u,0xd,1) ||
        s5123_link_change(l,0x13d203a4,2,0,1) || l->io.wait(20) ||
        s5123_link_change(l,0x13d203a4,2,2,1) || l->io.wait(1000) ||
        s5123_link_change(l,0x13d21404,~0u,1,1) || l->io.wait(2000) ||
        s5123_link_change(l,0x13d21408,~0u,1,1) || l->io.wait(1000) ||
        s5123_link_change(l,0x13800890,~0u,0x12000,1) ||
        s5123_link_change(l,0x13800710,0x3f0000,0x30000,1) ||
        s5123_link_change(l,0x1380080c,0x1f00,0x200,1) ||
        s5123_link_change(l,0x138000a0,0xf,1,1) ||
        s5123_link_change(l,0x13800018,0xffffff,0x010100,1) ||
        s5123_link_change(l,0x13800b40,~0u,0x1a,1) ||
        s5123_link_change(l,0x13800b44,~0u,0xea,1)) return -1;
    /* Root must remain a Type1 bridge with bus mastering disabled. */
    if (l->io.read(0x13800004,&l->root_command) || (l->root_command&4) ||
        l->io.read(0x1380000c,&l->root_header) || ((l->root_header>>16)&0x7f)!=1) return -1;
    /* AP wake precedes this callback by5ms, mirroring modem driver. */
    if (s5123_link_change(l,0x13c30004,2,2,1) || l->io.wait(20000) ||
        /* Live S20 reads01 after Samsung's11 write: bit4 does not latch.
         * Keep exact source request, verify the actual exit-L1 mode bit0. */
        s5123_link_change(l,0x13d203bc,0x11,0x11,0) ||
        l->io.read(0x13d203bc,&value) || !(value&1) ||
        s5123_link_change(l,0x13d203a0,~0u,2,1) ||
        s5123_link_change(l,0x13d203a8,0xfff,0,1) ||
        s5123_link_change(l,0x13d20474,~0u,1,1) || l->io.wait(50000)) return -1;
    l->io.phase("PCIe enable LTSSM; one bounded L0 poll");
    l->ltssm_touched=1;
    if (s5123_link_change(l,0x13d20054,~0u,1,1)) return -1;
    for (unsigned i=0;i<200;i++) {
        if (l->io.read(0x13d202c8,&l->link_status)) return -1;
        if ((l->link_status&0x3f)==0x11) break;
        if (l->io.wait(1000)) return -1;
    }
    if ((l->link_status&0x3f)!=0x11) return -1;
    l->io.phase("PCIe L0; configure outbound CFG0 only; read actual bus1 endpoint");
    /* iATU region0 at DBI+300000, config window lower4KiB of14ffe000+8KiB. */
    if (s5123_link_change(l,0x13b00004,~0u,0,1) ||
        s5123_link_change(l,0x13b00008,~0u,0x14ffe000,1) ||
        s5123_link_change(l,0x13b0000c,~0u,0,1) ||
        s5123_link_change(l,0x13b00010,~0u,0x14ffefff,1) ||
        s5123_link_change(l,0x13b00014,~0u,0x01000000,1) ||
        s5123_link_change(l,0x13b00018,~0u,0,1) ||
        s5123_link_change(l,0x13b00000,~0u,4,1) ||
        s5123_link_change(l,0x13b00004,~0u,0x80000000,1)) return -1;
    if (l->io.read(0x14ffe000,&l->endpoint_id) || l->io.read(0x14ffe004,&l->endpoint_command) ||
        l->io.read(0x14ffe008,&l->endpoint_class) || l->io.read(0x14ffe00c,&l->endpoint_header)) return -1;
    if (!l->endpoint_id || (l->endpoint_id&0xffff)==0xffff || ((l->endpoint_header>>16)&0x7f)!=0) return -1;
    if (l->endpoint_command&4) {
        l->io.phase("PCIe endpoint busmaster already set; clear only, never enable DMA");
        /* Command low16 only, high status written0 to avoid W1C flags.
         * Do not restore BME: caller shuts CP down after metadata capture. */
        if (l->io.write(0x14ffe004,0xffff,(l->endpoint_command&0xffff)&~4u,1) ||
            l->io.read(0x14ffe004,&l->endpoint_command) || (l->endpoint_command&4)) return -1;
    }
    return 0;
}
static int s5123_link_write_retry(struct s5123_link *l,uint64_t a,uint32_t mask,uint32_t value,int verify)
{
    for (unsigned i=0;i<3;i++) if (!l->io.write(a,mask,value,verify)) return 0;
    return -1;
}
static int s5123_link_park(struct s5123_link *l)
{
    int error=0;
    if (l->ltssm_touched && s5123_link_write_retry(l,0x13d20054,~0u,0,1)) error=1;
    if (l->pins_changed) {
        if (s5123_link_write_retry(l,0x13c30004,2,0,0)) error=1;
        if (s5123_link_write_retry(l,0x13c30000,0xf0,0x10,1)) error=1;
        uint32_t v;if (l->io.read(0x13c30004,&v) || (v&2)) error=1;
    }
    if (l->ia_touched && s5123_link_write_retry(l,0x13d00000,~0u,0,1)) error=1;
    l->parked=!error;return error?-1:0;
}
static int s5123_link_restore(struct s5123_link *l)
{
    int error=0;
    if (l->count && !l->parked && s5123_link_park(l)) return -1;
    /* Called after CP reset+power are held LOW. Restore reverse snapshots,
     * including powerdown before PMU isolation and GPIO ownership last. */
    for (unsigned i=l->count;i>0;i--) {
        const struct s5123_link_saved *s=&l->saved[i-1];
        int verify=s->address!=0x13c30004;
        if (s5123_link_write_retry(l,s->address,s->mask,s->value&s->mask,verify)) error=1;
    }
    return error?-1:0;
}
#endif

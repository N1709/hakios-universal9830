/* MIT. Opt-in UEFI adapter for the tested S5123 lifecycle/PIO cores.
 * Included only by the original-loader preflight, after MMIO, internal-loopback
 * and GPIO-low checks. No NV/EFS or modem partition I/O. All addresses fixed.
 */
#ifndef S5123_UEFI_BOOT_H
#define S5123_UEFI_BOOT_H
#include "s5123-boot.h"
#include "s5123-spi.h"
#include "s5123-gpio-fields.h"

static const struct {uint64_t base;unsigned pin;} cp_ap_pins[S5123_OUTPUT_COUNT]={
    {0x15c30120,0},{0x15c30280,0},{0x15c30320,0},{0x107300c0,3},{0x10730060,6}
};
static const uint32_t cp_spi_registers[]={S5123_CH,S5123_MODE,S5123_CS,S5123_IRQ,
    S5123_PACKET,S5123_SWAP,S5123_FEEDBACK,S5123_USI_CON};
static struct {
    uint32_t gpio_con[5],gpio_dat[5],gpio_changed[5];
    uint32_t mux,divider,mode,pins,pulls,dat,spi[8];
    int clock_changed,mode_changed,controller_saved,parked,controller_restore_attempted,controller_restored;
    struct s5123_spi_result pio;
    struct s5123_boot_result result;
} cp_boot_hw;

static int cp_hw_ready(void *context)
{
    (void)context;
    report_cellular_memory();
    if (!cellular_memory_ready || !timer_freq() ||
        (cellular_reg_read(0x15860720)&1) ||
        (cellular_reg_read(0x15c30284)&1) || (cellular_reg_read(0x15c30384)&1)) return 0;
    cp_boot_hw.mux=cellular_reg_read(0x10700690);
    cp_boot_hw.divider=cellular_reg_read(0x10701820);
    cp_boot_hw.mode=cellular_reg_read(0x10721048);
    cp_boot_hw.pins=cellular_reg_read(0x10730060);
    cp_boot_hw.dat=cellular_reg_read(0x10730064);
    cp_boot_hw.pulls=cellular_reg_read(0x10730068);
    if (cp_boot_hw.mode || (cp_boot_hw.pins&0xffff0000u) ||
        (cp_boot_hw.mux&(1u<<16)) || (cp_boot_hw.divider&(1u<<16)) ||
        !(cellular_reg_read(0x107020fc)&(1u<<21)) || !(cellular_reg_read(0x1070208c)&(1u<<21))) return 0;
    for (unsigned i=0;i<5;i++) {
        cp_boot_hw.gpio_con[i]=cellular_reg_read(cp_ap_pins[i].base);
        cp_boot_hw.gpio_dat[i]=cellular_reg_read(cp_ap_pins[i].base+4);
        if ((cp_boot_hw.gpio_con[i]>>(4*cp_ap_pins[i].pin))&15) return 0;
    }
    return 1;
}
static int cp_hw_prepare(void *context)
{
    (void)context;
    cp_boot_hw.clock_changed=1;
    cellular_reg_write(0x10701820,(cp_boot_hw.divider&~15u)|3);
    if (!cellular_clock_wait(0x10701820,(1u<<16)|15,3)) return -1;
    cellular_reg_write(0x10700690,cp_boot_hw.mux&~(1u<<4));
    if (!cellular_clock_wait(0x10700690,(1u<<16)|(1u<<4),0)) return -1;
    cp_boot_hw.mode_changed=1;
    cellular_reg_write(0x10721048,2);
    if (cellular_reg_read(0x10721048)!=2) return -1;
    for (unsigned i=0;i<8;i++) cp_boot_hw.spi[i]=cellular_reg_read(0x10960000+cp_spi_registers[i]);
    cp_boot_hw.controller_saved=1;
    if ((cp_boot_hw.spi[0]&3) || cp_boot_hw.spi[3] || cp_boot_hw.spi[4]) return -1;
    cellular_reg_write(0x10960000+S5123_CH,0);
    cellular_reg_write(0x10960000+S5123_PACKET,0);
    cellular_reg_write(0x10960000+S5123_IRQ,0);
    cellular_reg_write(0x10960000+S5123_CS,1); /* Park CS before routing external pads. */
    cellular_reg_write(0x10960000+S5123_USI_CON,0);
    return cellular_reg_read(0x10960000+S5123_CS)==1 ? 0 : -1;
}
static int cp_hw_output(void *context,enum s5123_ap_output signal,int level)
{
    (void)context;
    if ((unsigned)signal>=5 || (unsigned)level>1) return -1;
    uint64_t base=cp_ap_pins[signal].base;
    unsigned pin=cp_ap_pins[signal].pin;
    uint32_t con,dat;
    uint64_t tpl=BS->raise_tpl(31);
    int valid=s5123_gpio_output_values(cellular_reg_read(base),cellular_reg_read(base+4),pin,(unsigned)level,&con,&dat);
    if (valid) {cellular_reg_write(base+4,dat);cellular_reg_write(base,con);}
    BS->restore_tpl(tpl);
    cp_boot_hw.gpio_changed[signal]=1;
    int okay=valid && ((cellular_reg_read(base)>>(4*pin))&15)==1 &&
        ((cellular_reg_read(base+4)>>pin)&1)==(unsigned)level;
    print("cellular CP BOOT: output %d level %d verified %d\n",(uint64_t)signal,(uint64_t)level,(uint64_t)okay);
    return okay ? 0 : -1;
}
static int cp_hw_sleep(void *context,uint32_t microseconds)
{
    (void)context;
    return EFI_ERROR(BS->stall(microseconds)) ? -1 : 0;
}
static int cp_hw_route(void *context)
{
    (void)context;
    uint32_t con=cellular_reg_read(0x10730060);
    if ((con&0xffff0000u)!=0x01000000u || (cellular_reg_read(0x10730064)&(1u<<6))) return -1;
    uint64_t tpl=BS->raise_tpl(31);
    cellular_reg_write(0x10730068,cellular_reg_read(0x10730068)&0xffffu);
    cellular_reg_write(0x10730060,(cellular_reg_read(0x10730060)&0xffffu)|0x22220000u);
    BS->restore_tpl(tpl);
    int okay=(cellular_reg_read(0x10730060)&0xffff0000u)==0x22220000u &&
        !(cellular_reg_read(0x10730068)&0xffff0000u);
    print("cellular CP BOOT: SPI pad ownership verified %d\n",(uint64_t)okay);
    return okay ? 0 : -1;
}
static uint32_t cp_hw_spi_read32(void *context,uint32_t offset)
{
    (void)context;return cellular_reg_read(0x10960000+offset);
}
static uint8_t cp_hw_spi_read8(void *context,uint32_t offset)
{
    (void)context;return *(volatile uint8_t *)(uintptr_t)(0x10960000+offset);
}
static void cp_hw_spi_write32(void *context,uint32_t offset,uint32_t value)
{
    (void)context;cellular_reg_write(0x10960000+offset,value); /* No loopback bit. */
}
static void cp_hw_spi_write8(void *context,uint32_t offset,uint8_t value)
{
    (void)context;*(volatile uint8_t *)(uintptr_t)(0x10960000+offset)=value;
}
static int cp_hw_send(void *context,const uint8_t *data,size_t length,uint32_t *completed)
{
    (void)context;
    cp_boot_hw.parked=0;
    struct s5123_spi_ops ops={NULL,cp_hw_spi_read32,cp_hw_spi_read8,cp_hw_spi_write32,cp_hw_spi_write8,cellular_spi_now};
    int error=s5123_spi_send_boot(&ops,&cp_boot_hw.pio,data,length,1625000,2000000,1);
    *completed=cp_boot_hw.pio.completed_bytes;
    print("cellular CP BOOT: PIO error %s%d queued %d completed %d packets %d status %x\n",
        error<0?"-":"",(uint64_t)(error<0?-error:error),(uint64_t)cp_boot_hw.pio.queued_bytes,
        (uint64_t)*completed,(uint64_t)cp_boot_hw.pio.packets,(uint64_t)cp_boot_hw.pio.last_status);
    if (!error) {
        /* Samsung polls wake at 20 ms up to 200 times. A same-modem Linux
         * reference observed ~460 ms after TX: one 20 ms snapshot is too early.
         * Our pulled-up inputs may start high, so record transitions separately
         * and never promote a level (or transition) to firmware acceptance.
         */
        uint32_t first=(cellular_reg_read(0x15c30384)&1) |
            ((cellular_reg_read(0x15c303c4)&1)<<1) |
            ((cellular_reg_read(0x15c303e4)&1)<<2);
        uint32_t previous=first, changed=0, rises=0;
        for (unsigned sample=0;sample<200;sample++) {
            if (EFI_ERROR(BS->stall(20000))) return -1;
            uint32_t current=(cellular_reg_read(0x15c30384)&1) |
                ((cellular_reg_read(0x15c303c4)&1)<<1) |
                ((cellular_reg_read(0x15c303e4)&1)<<2);
            changed |= previous ^ current;
            rises |= ~previous & current;
            previous=current;
        }
        print("cellular CP BOOT: GPIO observation 4000ms first %x last %x changed %x rising %x (bits: PHONE_ACTIVE,CP2AP_WAKE,PS_HOLD; not acceptance proof)\n",
            (uint64_t)first,(uint64_t)previous,(uint64_t)changed,(uint64_t)rises);
    }
    return error;
}
static int cp_hw_stop(void *context)
{
    (void)context;
    if (cp_boot_hw.parked) return 0;
    if (!cp_boot_hw.controller_saved) return 0;
    cellular_reg_write(0x10960000+S5123_CH,0);
    cellular_reg_write(0x10960000+S5123_PACKET,0);
    cellular_reg_write(0x10960000+S5123_CS,1);
    cp_boot_hw.parked=!(cellular_reg_read(0x10960000+S5123_CH)&3) &&
        !cellular_reg_read(0x10960000+S5123_PACKET) && (cellular_reg_read(0x10960000+S5123_CS)&1);
    return cp_boot_hw.parked ? 0 : -1;
}
static int cp_hw_restore(void *context)
{
    (void)context;
    int okay=1;
    if (cp_boot_hw.controller_saved && !cp_boot_hw.controller_restore_attempted) {
        cp_boot_hw.controller_restore_attempted=1;
        cp_boot_hw.controller_restored=1;
        cellular_reg_write(0x10960000+S5123_CH,0);
        for (unsigned i=1;i<8;i++) cellular_reg_write(0x10960000+cp_spi_registers[i],cp_boot_hw.spi[i]);
        cellular_reg_write(0x10960000+S5123_CH,cp_boot_hw.spi[0]);
        for (unsigned i=0;i<8;i++)
            if (cellular_reg_read(0x10960000+cp_spi_registers[i])!=cp_boot_hw.spi[i]) cp_boot_hw.controller_restored=0;
    }
    if (cp_boot_hw.controller_saved && !cp_boot_hw.controller_restored) okay=0;
    uint64_t tpl=BS->raise_tpl(31);
    cellular_reg_write(0x10730060,(cellular_reg_read(0x10730060)&0xffffu)|(cp_boot_hw.pins&0xffff0000u));
    cellular_reg_write(0x10730068,(cellular_reg_read(0x10730068)&0xffffu)|(cp_boot_hw.pulls&0xffff0000u));
    cellular_reg_write(0x10730064,(cellular_reg_read(0x10730064)&~0xf0u)|(cp_boot_hw.dat&0xf0u));
    /* Keep reset/power held output-low. Restore AP_STATUS/AP_WAKE ownership only. */
    for (unsigned i=S5123_AP_STATUS;i<=S5123_AP_WAKE;i++) if (cp_boot_hw.gpio_changed[i]) {
        uint64_t base=cp_ap_pins[i].base;unsigned pin=cp_ap_pins[i].pin;
        uint32_t mask=15u<<(4*pin),bit=1u<<pin;
        cellular_reg_write(base,(cellular_reg_read(base)&~mask)|(cp_boot_hw.gpio_con[i]&mask));
        cellular_reg_write(base+4,(cellular_reg_read(base+4)&~bit)|(cp_boot_hw.gpio_dat[i]&bit));
    }
    BS->restore_tpl(tpl);
    if (cp_boot_hw.mode_changed) cellular_reg_write(0x10721048,cp_boot_hw.mode);
    if (cp_boot_hw.clock_changed) {
        cellular_reg_write(0x10701820,cp_boot_hw.divider);
        if (!cellular_clock_wait(0x10701820,(1u<<16)|15,cp_boot_hw.divider&15)) okay=0;
        cellular_reg_write(0x10700690,cp_boot_hw.mux);
        if (!cellular_clock_wait(0x10700690,(1u<<16)|(1u<<4),cp_boot_hw.mux&(1u<<4))) okay=0;
    }
    if (cellular_reg_read(0x10721048)!=cp_boot_hw.mode ||
        cellular_reg_read(0x10730060)!=cp_boot_hw.pins || cellular_reg_read(0x10730068)!=cp_boot_hw.pulls) okay=0;
    for (unsigned i=S5123_AP_STATUS;i<=S5123_AP_WAKE;i++) if (cp_boot_hw.gpio_changed[i]) {
        uint32_t mask=15u<<(4*cp_ap_pins[i].pin);
        if ((cellular_reg_read(cp_ap_pins[i].base)&mask)!=(cp_boot_hw.gpio_con[i]&mask)) okay=0;
    }
    print("cellular CP BOOT: SPI/clock/pad/AP ownership restored %d; power requested %d\n",(uint64_t)okay,(uint64_t)cp_boot_hw.result.power_requested);
    return okay?0:-1;
}
static int cp_hw_quiescent(void *context)
{
    (void)context;
    return !(cellular_reg_read(0x15860720)&1) &&
        (cellular_reg_read(0x15c30120)&15)==1 && !(cellular_reg_read(0x15c30124)&1) &&
        (cellular_reg_read(0x15c30280)&15)==1 && !(cellular_reg_read(0x15c30284)&1);
}
static int cellular_cp_boot_can_reset(void)
{
    return !cp_boot_hw.result.attempted || !cp_boot_hw.result.power_requested ||
        (cp_boot_hw.parked && cp_hw_quiescent(NULL));
}
static int cellular_cp_retry_stop(void)
{
    if (!cp_boot_hw.result.attempted) return 1;
    int parked=cp_hw_stop(NULL)==0;
    int reset=cp_hw_output(NULL,S5123_RESET_N,0)==0;
    int power=cp_hw_output(NULL,S5123_POWER_ON,0)==0;
    int settled=!EFI_ERROR(BS->stall(50000));
    int status=cp_hw_output(NULL,S5123_AP_STATUS,0)==0;
    int wake=cp_hw_output(NULL,S5123_AP_WAKE,0)==0;
    int restored=cp_hw_restore(NULL)==0;
    cp_boot_hw.result.cleanup_safe=parked && reset && power && settled && status && wake && restored && cp_hw_quiescent(NULL);
    print("cellular CP BOOT: cleanup-only retry safe %d (never powers on or retransmits)\n",(uint64_t)cp_boot_hw.result.cleanup_safe);
    return cp_boot_hw.result.cleanup_safe;
}
static int cellular_cp_boot_preflight(const uint8_t *data,uint64_t length,const uint8_t *info,uint64_t info_length)
{
    if (!data || !info || info_length!=24 || memcmp(info,"S512",4) ||
        ((const uint32_t *)info)[1]!=1 || ((const uint32_t *)info)[2]!=length ||
        ((const uint32_t *)info)[4]!=0x40000000 || ((const uint32_t *)info)[5]!=1) {
        print("cellular CP BOOT: invalid/missing BOOT metadata; no attempt\n");return 1;
    }
    struct s5123_boot_ops ops={NULL,cp_hw_ready,cp_hw_prepare,cp_hw_output,cp_hw_sleep,
        cp_hw_route,cp_hw_send,cp_hw_stop,cp_hw_restore,cp_hw_quiescent};
    int error=s5123_boot_once(&ops,&cp_boot_hw.result,data,(size_t)length,((const uint32_t *)info)[3]);
    print("cellular CP BOOT: error %s%d phase %d completed %d cleanup safe %d; CP acceptance unproven\n",
        error<0?"-":"",(uint64_t)(error<0?-error:error),(uint64_t)cp_boot_hw.result.phase,
        (uint64_t)cp_boot_hw.result.spi_completed,(uint64_t)cp_boot_hw.result.cleanup_safe);
    return !cp_boot_hw.result.attempted || cp_boot_hw.result.cleanup_safe;
}
#endif

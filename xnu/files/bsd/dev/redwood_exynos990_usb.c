/*
 * The Exynos 990's (SM-G980F, x1slte) side of the USB device link (redwood_dwc3.c, from Scion's hakios.dwc3.c): the
 * USB 2.0 PHY brought up in device mode. The sequence is Mu-Silicium's, the one behind the loader's working fastboot on
 * this very phone (Silicon/Samsung/S5E9830Pkg/Library/PlatformUsbfnLib/BoardInit.c, UsbfnPlatformPhyInit, BSD-2-Clause):
 *   PMU USBDRD_PHY_CTRL isolation off, USBCON's Q-channel forced, the PHYs' soft reset asserted, UTMI out of suspend and
 *   pull-downs off, HSP COMMONONN + EN_UTMISUSPEND, the bus filter bypassed, VBUS and BVALID forced valid (the VBUS
 *   pad is not wired to the PHY), the tune values (TX pre-emphasis 3, TX VREF 0xF, SQRX 7, COMPDIS 7, TX RES 3), the PHY
 *   powered (SIDDQ off), the reset let go, the PIPE3 side forced idle (high speed only), the hub port's OCD selects.
 * The DWC3 after its soft reset as Mu-Silicium sets it: 16-bit UTMI (PHYIF 1, USBTRDTIM 5), no free-running clock,
 * GUSB3PIPECTL SUSPHY (no SuperSpeed), GFLADJ for the chip's EVT1 (30 MHz 0x20, 240 MHz decrement 0xC, LPM and SDBND
 * select), GUCTL's reference clock period left as the boot set it.
 * The clocks are left as the boot left them: UEFI's driver opens none either (usb_gadget_clk is not used here).
 * Every write is read back and said.
 */
#include <stdint.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include "redwood_usb_glue.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);

#define PMU_PA                  0x15860000ULL
#define PMU_USBDRD_PHY_CTRL     0x072Cu
#define  PMU_PHY_ENABLE         (1u << 0)
#define USBCON_PA               0x10C00000ULL
#define USBCON_SIZE             0x1000u
#define DWC3_PA                 0x10E00000ULL
#define DWC3_SIZE               0x10000u

#define CTRL_VER                0x00u
#define LINK_CTRL               0x04u
#define LINK_PORT               0x08u
#define CLKRST                  0x20u
#define PWR                     0x24u
#define COMBO_PMA_CTRL          0x48u
#define UTMI                    0x50u
#define HSP                     0x54u
#define HSP_TUNE                0x58u
#define HSP_TEST                0x5Cu

#define LINKCTRL_PIPE3_FORCE_RX_ELEC_IDLE (1u << 18)
#define LINKCTRL_PIPE3_FORCE_PHY_STATUS   (1u << 17)
#define LINKCTRL_PIPE3_FORCE_EN           (1u << 16)
#define LINKCTRL_DIS_QACT_LINKGATE        (1u << 12)
#define LINKCTRL_DIS_QACT_ID0             (1u << 11)
#define LINKCTRL_DIS_QACT_VBUS_VALID      (1u << 10)
#define LINKCTRL_DIS_QACT_BVALID          (1u << 9)
#define LINKCTRL_FORCE_QACT               (1u << 8)
#define LINKCTRL_BUS_FILTER_BYPASS(x)     (((x) & 0xFu) << 4)
#define LINKPORT_HUB_PORT_SEL_OCD_U3      (1u << 3)
#define LINKPORT_HUB_PORT_SEL_OCD_U2      (1u << 2)
#define CLKRST_PHY20_SW_RST               (1u << 13)
#define CLKRST_PHY20_RST_SEL              (1u << 12)
#define CLKRST_PHY30_SW_RST               (1u << 3)
#define CLKRST_PHY30_RST_SEL              (1u << 2)
#define CLKRST_PORT_RST                   (1u << 1)
#define PMA_LOW_PWR                       (1u << 4)
#define UTMI_FORCE_VBUSVALID              (1u << 5)
#define UTMI_FORCE_BVALID                 (1u << 4)
#define UTMI_DP_PULLDOWN                  (1u << 3)
#define UTMI_DM_PULLDOWN                  (1u << 2)
#define UTMI_FORCE_SUSPEND                (1u << 1)
#define UTMI_FORCE_SLEEP                  (1u << 0)
#define HSP_VBUSVLDEXTSEL                 (1u << 13)
#define HSP_VBUSVLDEXT                    (1u << 12)
#define HSP_EN_UTMISUSPEND                (1u << 9)
#define HSP_COMMONONN                     (1u << 8)
#define TUNE_TXVREF(x)                    (((uint32_t)(x) & 0xFu) << 28)
#define TUNE_TXRES(x)                     (((x) & 0x3u) << 21)
#define TUNE_TXPREEMPA(x)                 (((x) & 0x3u) << 18)
#define TUNE_SQRX(x)                      (((x) & 0x7u) << 8)
#define TUNE_COMPDIS(x)                   (((x) & 0x7u) << 0)
#define TUNE_MASK                         (TUNE_TXVREF(0xF) | TUNE_TXRES(3) | TUNE_TXPREEMPA(3) | TUNE_SQRX(7) | TUNE_COMPDIS(7))
#define HSP_TEST_SIDDQ                    (1u << 24)
#define HSP_TEST_LINESTATE(x)             (((x) >> 20) & 3u)

/* DWC3 (Mu-Silicium SamsungPkg/Include/Device/Dwc3.h) */
#define GUSB2_PHYIF_16                    (1u << 3)
#define GUSB2_USBTRDTIM(n)                ((uint32_t)(n) << 10)
#define GUSB2_USBTRDTIM_MASK              GUSB2_USBTRDTIM(0xF)
#define GUSB2_ULPI_UTMI                   (1u << 4)
#define GUSB3_SUSPHY                      (1u << 17)
#define GFLADJ_30MHZ_MASK                 0x3Fu
#define GFLADJ_30MHZ_SDBND_SEL            (1u << 7)
#define GFLADJ_REFCLK_FLADJ_MASK          (0x3FFFu << 8)
#define GFLADJ_REFCLK_LPM_SEL             (1u << 23)
#define GFLADJ_REFCLK_240MHZ_DECR(n)      ((uint32_t)(n) << 24)
#define GFLADJ_REFCLK_240MHZ_DECR_MASK    GFLADJ_REFCLK_240MHZ_DECR(0x7F)

static vm_offset_t usbcon, pmu;
static bool mapped;
static const char *const linestates[4] = { "SE0", "J", "K", "SE1" };

static inline uint32_t rd(vm_offset_t b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void
wr(vm_offset_t b, uint32_t o, uint32_t v)
{
	*(volatile uint32_t *)(b + o) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
map_all(usb_glue_say_t say)
{
	if (!mapped) {
		pmu = ml_io_map(PMU_PA, 0x1000);
		usbcon = ml_io_map(USBCON_PA, USBCON_SIZE);
		if (pmu == 0 || usbcon == 0) {
			say("usb: exynos990: could not map the PMU or USBCON\n");
			return false;
		}
		mapped = true;
	}
	return true;
}

static void
say_usbcon(usb_glue_say_t say, const char *when)
{
	uint32_t test = rd(usbcon, HSP_TEST);

	say("usb: exynos990: %s: PMU USBDRD_PHY_CTRL %08x; USBCON VER %08x LINK_CTRL %08x LINK_PORT %08x CLKRST %08x PWR %08x "
	    "PMA %08x\n", when, rd(pmu, PMU_USBDRD_PHY_CTRL), rd(usbcon, CTRL_VER), rd(usbcon, LINK_CTRL), rd(usbcon, LINK_PORT),
	    rd(usbcon, CLKRST), rd(usbcon, PWR), rd(usbcon, COMBO_PMA_CTRL));
	say("usb: exynos990: %s: UTMI %08x HSP %08x HSP_TUNE %08x HSP_TEST %08x (SIDDQ %u, line %s)\n", when, rd(usbcon, UTMI),
	    rd(usbcon, HSP), rd(usbcon, HSP_TUNE), test, (test & HSP_TEST_SIDDQ) ? 1 : 0, linestates[HSP_TEST_LINESTATE(test)]);
}

static bool
e990_look(usb_glue_say_t say, __unused uint32_t flags)
{
	uint32_t ver;

	if (!map_all(say)) {
		return false;
	}
	say_usbcon(say, "1: as the boot left it");
	ver = rd(usbcon, CTRL_VER);
	/* the DWC3's registers are only read when USBCON answers: its clock is the one the link's are behind */
	return ver != 0 && ver != 0xffffffffu;
}

static uint32_t
change(usb_glue_say_t say, const char *name, uint32_t off, uint32_t set, uint32_t clear)
{
	uint32_t before = rd(usbcon, off), want = (before & ~clear) | set, after;

	wr(usbcon, off, want);
	after = rd(usbcon, off);
	say("usb: exynos990: 2: %s %08x -> %08x%s\n", name, before, after, after == want ? "" : " (NOT as written)");
	return after;
}

static int
e990_phy_on(usb_glue_say_t say, __unused uint32_t flags)
{
	uint32_t ver, v;

	if (!map_all(say)) {
		return ENXIO;
	}
	v = rd(pmu, PMU_USBDRD_PHY_CTRL);
	wr(pmu, PMU_USBDRD_PHY_CTRL, v | PMU_PHY_ENABLE);
	say("usb: exynos990: 2: PMU USBDRD_PHY_CTRL %08x -> %08x (isolation off)\n", v, rd(pmu, PMU_USBDRD_PHY_CTRL));
	delay(10);
	ver = rd(usbcon, CTRL_VER);
	if (ver == 0 || ver == 0xffffffffu) {
		say("usb: exynos990: 2: USBCON VER %08x: the PHY's block does not answer, stopping\n", ver);
		return ENXIO;
	}

	/* the Q-channel forced (UsbPhyForceQChannel) */
	change(say, "LINK_CTRL (Q-channel, FORCE_QACT off)", LINK_CTRL, LINKCTRL_DIS_QACT_ID0 | LINKCTRL_DIS_QACT_VBUS_VALID |
	    LINKCTRL_DIS_QACT_BVALID | LINKCTRL_DIS_QACT_LINKGATE, LINKCTRL_FORCE_QACT);
	delay(500);
	change(say, "LINK_CTRL (FORCE_QACT)", LINK_CTRL, LINKCTRL_FORCE_QACT, 0);
	delay(500);

	change(say, "CLKRST (PHY resets asserted)", CLKRST, CLKRST_PHY20_SW_RST | CLKRST_PHY20_RST_SEL | CLKRST_PHY30_SW_RST |
	    CLKRST_PHY30_RST_SEL, 0);
	change(say, "UTMI (out of suspend, no pull-downs)", UTMI, 0, UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP | UTMI_DP_PULLDOWN |
	    UTMI_DM_PULLDOWN);
	change(say, "HSP (UTMI suspend, common on)", HSP, HSP_EN_UTMISUSPEND | HSP_COMMONONN, 0);
	delay(100);
	change(say, "LINK_CTRL (bus filter bypass)", LINK_CTRL, LINKCTRL_BUS_FILTER_BYPASS(0xF), 0);
	change(say, "UTMI (VBUS, BVALID forced)", UTMI, UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID, 0);
	change(say, "HSP (VBUS valid external)", HSP, HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT, 0);
	change(say, "HSP_TUNE", HSP_TUNE, TUNE_COMPDIS(7) | TUNE_SQRX(7) | TUNE_TXPREEMPA(3) | TUNE_TXRES(3) | TUNE_TXVREF(0xF),
	    TUNE_MASK);
	change(say, "HSP_TEST (SIDDQ off: powered)", HSP_TEST, 0, HSP_TEST_SIDDQ);
	delay(10);
	change(say, "CLKRST (resets let go)", CLKRST, CLKRST_PHY20_RST_SEL, CLKRST_PHY20_SW_RST | CLKRST_PHY30_SW_RST |
	    CLKRST_PORT_RST);
	delay(75);
	change(say, "LINK_CTRL (PIPE3 forced idle)", LINK_CTRL, LINKCTRL_PIPE3_FORCE_EN | LINKCTRL_PIPE3_FORCE_RX_ELEC_IDLE,
	    LINKCTRL_PIPE3_FORCE_PHY_STATUS);
	change(say, "COMBO_PMA_CTRL (low power)", COMBO_PMA_CTRL, PMA_LOW_PWR, 0);
	change(say, "LINK_PORT (OCD selects)", LINK_PORT, LINKPORT_HUB_PORT_SEL_OCD_U3 | LINKPORT_HUB_PORT_SEL_OCD_U2, 0);
	say_usbcon(say, "2: done");
	return 0;
}

static void
e990_phy_state(char *out, unsigned size)
{
	if (!mapped) {
		snprintf(out, size, "(not mapped)");
		return;
	}
	uint32_t test = rd(usbcon, HSP_TEST), utmi = rd(usbcon, UTMI);

	snprintf(out, size, "line %s, UTMI %08x (VBUSVALID forced %u), SIDDQ %u", linestates[HSP_TEST_LINESTATE(test)], utmi,
	    (utmi & UTMI_FORCE_VBUSVALID) ? 1 : 0, (test & HSP_TEST_SIDDQ) ? 1 : 0);
}

/* as UsbfnPlatformPhyExit: VBUS no longer forced, the PHY suspended, powered down, in reset, isolated */
static void
e990_phy_off(void)
{
	if (!mapped) {
		return;
	}
	wr(usbcon, UTMI, (rd(usbcon, UTMI) & ~(UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID)) | UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP);
	wr(usbcon, HSP, rd(usbcon, HSP) & ~(HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT));
	wr(usbcon, HSP_TEST, rd(usbcon, HSP_TEST) | HSP_TEST_SIDDQ);
	wr(usbcon, CLKRST, rd(usbcon, CLKRST) | CLKRST_PHY20_SW_RST | CLKRST_PHY20_RST_SEL | CLKRST_PHY30_SW_RST |
	    CLKRST_PHY30_RST_SEL);
	wr(pmu, PMU_USBDRD_PHY_CTRL, rd(pmu, PMU_USBDRD_PHY_CTRL) & ~PMU_PHY_ENABLE);
}

static const struct redwood_usb_glue exynos990_glue = {
	.name = "exynos990",
	.dwc3_pa = DWC3_PA,
	.dwc3_size = DWC3_SIZE,
	.look = e990_look,
	.phy_on = e990_phy_on,
	.phy_state = e990_phy_state,
	.phy_off = e990_phy_off,
	.gusb2_set = GUSB2_PHYIF_16 | GUSB2_USBTRDTIM(5),
	.gusb2_clear = GUSB2_PHYIF_16 | GUSB2_USBTRDTIM_MASK | GUSB2_ULPI_UTMI,
	.gfladj_mask = GFLADJ_30MHZ_MASK | GFLADJ_REFCLK_240MHZ_DECR_MASK | GFLADJ_REFCLK_FLADJ_MASK | GFLADJ_REFCLK_LPM_SEL |
	    GFLADJ_30MHZ_SDBND_SEL,
	.gfladj_value = 0x20u | GFLADJ_REFCLK_240MHZ_DECR(0xC) | GFLADJ_REFCLK_LPM_SEL | GFLADJ_30MHZ_SDBND_SEL,
	.gusb3_set = GUSB3_SUSPHY,
	.keep_guctl = true,
};

const struct redwood_usb_glue *
redwood_usb_glue_exynos990(void)
{
	return &exynos990_glue;
}

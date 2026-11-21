/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). A "swap" copies the pixels of the surfaces of its layers into that memory, and
 * "vsync" is a timer. On the y2s (Exynos 990) two things of the display hardware are driven:
 * DECON's TE trigger, held while a frame is copied in (fb_te), and the panel's brightness, sent
 * as DCS commands through DSIM (fb_bl).
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSerialize.h>
#include "redwood_graphics_policy.h"
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
/* Shared pinctrl/SPI interfaces live outside the exported IOKit headers. */
#include "../../bsd/sys/redwood_exynos_spi.h"
int redwood_dsay(const char *, ...);
volatile uint8_t *redwood_gpio_bank(uint32_t block_phys, uint32_t bank_offset);
void redwood_gpio_func(volatile uint8_t *bank, uint32_t pin, uint32_t func);
uint32_t redwood_gpio_get(volatile uint8_t *bank, uint32_t pin);
void redwood_gpio_set(volatile uint8_t *bank, uint32_t pin, uint32_t value);
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}
extern "C" {
typedef void (*redwood_dbgport_fn)(const uint64_t *v, unsigned n, char *out, size_t cap);  /* (bsd/sys/redwood_dbgport.h) */
int redwood_dbgport_add(const char *name, const char *help, redwood_dbgport_fn fn);
void dbg_fb_mode(const uint64_t * v, unsigned n, char * out, size_t cap);
void dbg_fb_fade(const uint64_t * v, unsigned n, char * out, size_t cap);
void dbg_fb_cal(const uint64_t * v, unsigned n, char * out, size_t cap);
void dbg_fb_factory(const uint64_t *v, unsigned n, char *out, size_t cap);
void dbg_fb_warm(const uint64_t *v, unsigned n, char *out, size_t cap);
void dbg_fb_aod(const uint64_t *v, unsigned n, char *out, size_t cap);
}
class IOMobileFramebuffer;
static IOMobileFramebuffer * gFbLive;           /* the one framebuffer, for fb.mode */

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

/*
 * kern.redwood_fb_shown_sid: the IOSurface id of the frame on the panel (A, 2026-11-19), so a root tool
 * (tools/hotspot/rwshot.c) can look the surface up and save what the screen shows: a screenshot without hands.
 */
#include <sys/sysctl.h>
static unsigned int gFbShownSid;
SYSCTL_UINT(_kern, OID_AUTO, redwood_fb_shown_sid, CTLFLAG_RD | CTLFLAG_LOCKED, &gFbShownSid, 0,
    "the IOSurface id of the frame on the panel");

extern "C" void redwood_tl(const char * format, ...) __printflike(1, 2);
extern "C" uint64_t redwood_fb_swap_at;        /* the clock governor's frame floor (redwood_exynos_acpm.c, dvfs_frame=) */
extern "C" int redwood_gov_wake;
extern "C" void redwood_gov_boost(uint32_t ms);
extern "C" uint32_t redwood_wake_boost_ms;          /* kern.redwood.wake_boost_ms (redwood_exynos_acpm.c) */  /* the wake boost (redwood_exynos_acpm.c) */
extern "C" void redwood_dvfs_display(int on);     /* the screen-off power profile (redwood_exynos_acpm.c) */
extern "C" void loader_log_line(const char * line);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/*
 * The Exynos 990's display controller (y2s; Samsung's dpu30/cal_9830). The panel is a MIPI command-mode
 * one: on every TE pulse DECON_F reads the whole frame out of memory through one DMA channel, so pixels
 * written into the memory being read tear. Boot-arg fb_flip=1: two panel buffers, the frame drawn into
 * the one not being read, then that channel's base address switched to it (latched by DECON at the start
 * of the next frame) -- no frame is ever read half drawn. fb_dpu=1: only read the controller's state and
 * say it. Both are for this SoC only: the addresses are its own.
 */
static TUNABLE(uint32_t, gFlip, "fb_flip", 0);
static TUNABLE(uint32_t, gDPU, "fb_dpu", 0);
#define DPU_DECON_F_PHYS        0x19050000ULL
/*
 * DPU_DMA: the common block at + 0, the IDMA of channel c at + 0x1000 * (c + 1) (dpp_drv.c: a dpp node's
 * first reg is its DMA; 0x19041000.. is the DPP colour pipe). Both read 0 to the non-secure side until
 * the DPU's D-TZPC is restored: fb_dpu=2 asks the secure monitor for that, as Linux does when it powers
 * pd-dpu on (pmucal_local.c, need_smc 0x19010204) and as redwood_mali.c does for the GPU's domain.
 */
#define DPU_DMA_PHYS            0x19090000ULL
#define DPU_DPP_PHYS            0x19040000ULL           /* DPP of channel c at + 0x1000 * (c + 1) */
#define DPU_DMA_VERSION         0x0000
#define DPU_DMA_QCH_EN          0x000C
#define DPU_DMA_GLB_CGEN_CH0    0x0040
#define SMC_PREPARE_PD_ONOFF    0x82000410UL
#define PD_DPU_NEED_SMC         0x19010204UL
#define DECON_GLOBAL_CONTROL    0x0000
#define DECON_SHADOW_UPDATE     0x0060
#define  DECON_SHADOW_GLOBAL    (1u << 31)
#define DECON_TRIG_CONTROL      0x0070
#define DECON_TE_COUNT          0x0078
#define DECON_DATA_PATH_0       0x0214                  /* window w enabled: bit 4w */
#define DECON_DATA_PATH_1       0x0218                  /* window w's channel: bits 4w..4w+2 */
#define IDMA_ENABLE_REG         0x0000
#define IDMA_IN_CON_REG         0x0008
#define IDMA_SRC_SIZE_REG       0x0010
#define IDMA_SRC_OFFSET_REG     0x0014
#define IDMA_IMG_SIZE_REG       0x0018
#define IDMA_BASE_Y8_REG        0x0040
#define IDMA_STRIDE_REG         0x0050
#define IDMA_SHADOW             0x0800                  /* what the DMA is using now, read back */
#define FLIP_LATCH_MS           50

/*
 * fb_scaler=1 (PLANS/07-display.md, D4): no copy at all. CoreAnimation's frame (1125x2436 BGRA, one
 * piece of physical memory: MiniIOSurface.cpp, fb_scaler) is read by the display controller itself
 * through channel 5 (VGRFS, one of the two with a scaler) and scaled there to the panel's 1440x3200.
 * The window S-Boot set up stays as it is -- full screen, opaque -- and only the channel it reads
 * changes: DATA_PATH_CONTROL_1's field for it, from S-Boot's channel to 5, latched with the next
 * frame by a shadow update like fb_flip's base. A frame that is anything else (more layers, another
 * size, memory not in one piece) is drawn by the processor as before, and the window goes back to
 * S-Boot's channel. Channel 5 is set up like S-Boot's (its format, bus and QoS settings copied) plus
 * the scaler: sizes, ratios, and a bicubic filter of our own. Its registers are Samsung's cal_9830
 * (regs-dpp.h, dpp_reg.c): IDMA5 at 0x19096000, DPP5 at 0x19046000.
 */
static TUNABLE(uint32_t, gScaler, "fb_scaler", 0);
/*
 * fb_mode (x1slte, SM-G980F, 2026-11-06): 1 = the panel switched to 1080x2400 at 120 Hz (HS), Samsung's "mres" at start;
 * 0 = as UEFI leaves it (1440x3200 at 60 Hz). Needs fb_scaler=1 (the frame is scaled to the panel's size by DPP 5).
 * See modeSetFhd120().
 *
 *   0  1440x3200 @  60   iOS draws 1125x2436 at 3x   touch scan normal 60
 *   1  1080x2400 @ 120   iOS draws 1125x2436 at 3x   touch scan HS120      <- the phone's preset (x1slte.cmdline)
 *   2  1080x2400 @  60   iOS draws 1125x2436 at 3x   touch scan normal 60
 *   3   720x1600 @ 120   iOS draws  750x1624 at 2x   touch scan HS120
 *   4   720x1600 @  60   iOS draws  750x1624 at 2x   touch scan normal 60
 *
 * THE PANEL AT 120 Hz IS NOT iOS AT 120 Hz (A, 2026-11-14; the user: scrolling "a lot better" once all of these held).
 * The panel is a fixed-rate one (no LTPO); a 120 Hz mode needs every one of these, each following fb_mode by itself:
 *   - TimingElements at the mode's refresh (this driver: SyncRate 120 in 16.16, one fixed mode, no range);
 *   - CoreAnimation's clock at 120: backboardd started with CA_HEARTBEAT_RATE=120 (userland/src/inject.c). Without it
 *     iOS ran its frames for 60 (CADisplay.refreshRate 0.016667) while the panel scanned 120 -- the "60 Hz feel";
 *   - apps not held to 60: QuartzCore's phone cap lifted with CADisableMinimumFrameDurationOnPhone set in each app's
 *     info dictionary (inject.c app_fps_unlock; app_fps=0 off);
 *   - the touch controller scanning at 120: sec_ts 0x94 {1, 2} (bsd/dev/redwood_sec_ts.c ts_set_scan_rate; it reported
 *     ~60 a second otherwise; touch_scan=N);
 *   - SpringBoard's own 20-30 fps animation caps raised (userland/src/inject_sb.c sb_fps_hooks; sb_fps=0 off).
 * At 2x (fb_mode 3/4) also: MobileGestalt's cached scale and pitch (init.c mg_scale_sync) and the status bar's notch
 * layout (statusbar_shift.h) -- see the drawing size below. Check on the phone: /var/root/hztest must print 120.0 Hz.
 * Everything here: docs/devices/x1slte/display.md, "Display presets" and "Smoothness findings".
 */
static TUNABLE(uint32_t, gFbMode, "fb_mode", 0);
/*
 * fb_fade=1 (default; B, 2026-11-06): a brightness change with CoreBrightness's FadePeriod (16.16 seconds: the
 * lock's 12124 = 185 ms) is ramped as Apple's backlight does, one step a frame, even to the eye (in the square root
 * of nits). Before, every change was one jump: the screen snapped off and on with the power button.
 */
static TUNABLE(uint32_t, gFade, "fb_fade", 1);
/*
 * fb_dim=1 (default; B, 2026-11-06): the panel's own smooth dimming (WRCTRLD 0x53 bit 3, DD) with every brightness
 * write, so the DDIC blends between the fade's steps over its frames: ten 20 % steps of a 185 ms fade had shown as
 * stairs. fb_dim=0: 0x53 = 0 as Samsung's table has it (dimming off).
 */
static TUNABLE(uint32_t, gDim, "fb_dim", 1);
/* fb_offhold=1: DCS 28 held 300 ms after the last brightness write. Tried: the user found it worse (dark but on). Off. */
static TUNABLE(uint32_t, gOffHold, "fb_offhold", 0);
/*
 * Fade tuning, live with fb.fade (B, 2026-11-07: the power-off fade read as a stutter at low brightness -- a 185 ms fade
 * from ~4 nits has only ~20 DBV levels): gFadeDD 1 keeps the panel's own dimming (DD) on during software fades, so it
 * blends between the steps; gOffFadeMs > 0 stretches a fade to zero (the power button) to at least that long.
 */
static uint32_t gFadeDD = 1, gOffFadeMs = 0;
/*
 * gOffAlpha 1 (default; B, 2026-11-07): the power-off fade is done on the PICTURE, not the backlight: DECON's window
 * plane alpha (WIN_CONTROL_0 [23:16], blend FG x alpha0 + BG x 0 against the black background) goes 255 -> 0, one step
 * a frame, while DBV/AOR/the factory gamma rows stay as they are; then DCS 28. Stepping the backlight switched the
 * panel's gamma/AOR rows on every step and read as a stutter (the user, 21:58). fb.fade dd offms alpha.
 */
static uint32_t gOffAlpha = 1;
static uint32_t gOnFadeMs = 0;          /* power on: the picture fades in over this many ms (fb.fade 4th number; 0 off: off by default, the user saw no difference) */
static TUNABLE(uint32_t, gOffMask, "fb_offmask", 1);    /* display off: DECON's trigger masked (fb_offmask=0 keeps frames going) */
/*
 * fb_mode_lpref=1 (B, 2026-11-06): the gate lets DSIM_UNDERRUN_CTRL (the command-mode underrun reference, dtbo
 * cmd_lp_ref) through whatever it holds. The first fb_mode=1 run found UEFI's 0x138b there, not the dtbo's 0x1e6e,
 * every other DSIM and DECON register exactly the native mode's. Samsung's driver writes cmd_lp_ref from the dtbo at
 * every mode set (cal_9830/dsim_reg.c dsim_reg_set_mres, dsim_reg_init), whatever was there: it depends on nothing the
 * mode change leaves as it is. The FHD value (0x284d) is written as Samsung's is.
 */
static TUNABLE(uint32_t, gFbModeLpRef, "fb_mode_lpref", 0);
/*
 * fb_swapwait=1: with the scaler on, SwapEnd returns only once its frame is on the panel (run 17's cure for
 * tearing: CoreAnimation draws its next frame into the surface the panel showed).
 * By default SwapEnd returns once the swap is queued and the wait is where CoreAnimation asks for it: before
 * each swap it calls IOMobileFramebufferSwapWait for the swaps before (`tl: w N-2`, `w N-1`, then `s N`:
 * run 27), and only then draws into the surface that swap N-2 had. kFBMethodSwapWait answers once the swap
 * asked about is on the panel (fShownSwapID), so the drawing of a frame overlaps the panel's wait for the one
 * before, as with Apple's driver. present() counts a swap whose surface is still shown or queued (would tear).
 * Measured (runs 27-28, PLANS/10-mali2metal.md): no gain -- 45 % of animation frames at 60 Hz against 48 %
 * with the swap waiting -- because what spaces the frames is the window server's own time, so the swap
 * waits again by default (fb_swapwait=1) and fb_swapwait=0 keeps the protocol for when that changes.
 */
static TUNABLE(uint32_t, gSwapWait, "fb_swapwait", 0);   /* 0 since 2026-11-18: what the S20 always ran with (its boot line said fb_swapwait=0) */
#define SCALER_CH               5
#define IDMA_IRQ_REG            0x0004
#define  IDMA_IRQ_ERRORS        ((1u << 26) | (1u << 25) | (1u << 24) | (1u << 23) | (1u << 22) | (1u << 21) | (1u << 19) | (1u << 17))
#define  IDMA_IRQ_PENDING       (0x07FBu << 16)
#define  IDMA_SFR_UPDATE_FORCE  (1u << 4)
#define IDMA_STRIDE_1_REG       0x0054
#define IDMA_IN_REQ_DEST_REG    0x0308
#define DPP_ENABLE_REG          0x0000
#define  DPP_SFR_UPDATE_FORCE   (1u << 4)
#define  DPP_ENABLE_COPIED      ((7u << 8) | (1u << 5) | (1u << 3))     /* clock gates, PSLVERR, Q-channel */
#define DPP_IRQ_REG             0x0004
#define  DPP_IRQ_CONFIG_ERROR   (1u << 21)
#define  DPP_IRQ_PENDING        (0x21u << 16)
#define DPP_IN_CON_REG          0x0008
#define DPP_IMG_SIZE_REG        0x0018
#define DPP_SCALED_IMG_SIZE_REG 0x002C
#define DPP_MAIN_H_RATIO_REG    0x0044
#define DPP_MAIN_V_RATIO_REG    0x0048
#define DPP_V_COEF(n, s, x)     (0x200 + (n) * 0x4 + (s) * 0x24 + (x) * 0x200)
#define DPP_H_COEF(n, s, x)     (0x290 + (n) * 0x4 + (s) * 0x24 + (x) * 0x200)
#define DPP_POSITION_REG(i)     (0x05B0 + 4 * (i))                     /* Y h/v, C h/v: the first phase */
#define DPP_DYNAMIC_GATING_REG  0x0A54
#define DPP_CFG_ERR_STATE_REG   0x0D08
#define DECON_SHADOW_REGS       0x7000                  /* DECON's registers in use, read back */
/*
 * Keys' bicubic (a = -0.5) at phases 0..8 of 16 (the rest are these mirrored), each summing to 512,
 * the scale Samsung's tables use (dpp_coef.h); 11-bit two's complement in the registers. Four taps,
 * the centre one second (as v_coef_4t); horizontally the same four in the middle of eight, the
 * centre fourth (as h_coef_8t).
 */
static const int16_t scalerBicubic[9][4] = {
	{    0,  512,    0,    0 },
	{  -14,  507,   20,   -1 },
	{  -24,  494,   46,   -4 },
	{  -32,  472,   79,   -7 },
	{  -36,  444,  116,  -12 },
	{  -38,  410,  157,  -17 },
	{  -38,  372,  200,  -22 },
	{  -35,  331,  244,  -28 },
	{  -32,  288,  288,  -32 },
};

/*
 * fb_te=1: no tearing without the DMA's registers (they read 0 here: gated). Frames are drawn into
 * a back buffer of ours; then DECON's hardware trigger is masked (the panel keeps showing its own
 * memory), DECON is waited for until it is not sending a frame, the changed rows are copied into the
 * buffer it reads, and the trigger is let go: the next TE sends the whole new frame.
 */
static TUNABLE(uint32_t, gTE, "fb_te", 0);

/*
 * Boot-arg fb_irq (default 1): the waits for DECON to take a new frame (its shadow registers latch at the
 * start of a frame) sleep until its frame-start interrupt (Samsung's dt: decon_f interrupts, the first: SPI
 * 0xfe, level; decon_irq_handler clears INTERRUPT_PENDING bit 12) instead of looking every 100 us -- each
 * look a timer, some 2800 a second on the big cores (the cpuidle wake record, 27 October). Unmasked only
 * while somebody waits: the panel's frames start on every TE whether anything changed or not.
 * fb_irq=0: the polling, as before.
 */
static TUNABLE(uint32_t, gFrameIrq, "fb_irq", 1);
#define DECON_INTID_FRAME_START (32 + 0xfe)
#define DECON_INT_EN_ALL        (1u << 0)               /* INTERRUPT_ENABLE: DPU_INT_EN */
#define DECON_INT_FRAME_DONE    (1u << 13)
#define DECON_INT_EXTRA         (1u << 4)
/* a wait sleeps at most this long between looks: a 60 Hz refresh (27 October: no frame-start interrupt was
 * missed, 0 waits gave up; at 4 ms, 35 of 59 waits looked again for nothing) */
#define LATCH_CAP_MS            17
extern "C" kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid),
    void *context);
extern "C" void gic_v3_spi_mask(uint32_t intid);
extern "C" void gic_v3_spi_unmask(uint32_t intid);
#define DECON_GLOBAL_IDLE       (1u << 5)               /* no frame being sent */
#define DECON_GLOBAL_RUN        (1u << 4)
#define DECON_GLOBAL_CMD_MODE   (1u << 8)
#define DECON_INT_ENABLE        0x0040
#define DECON_INT_PENDING       0x004C
#define DECON_SECURE_CONTROL    0x0064
#define DECON_TRIG_TIMER        0x0074
#define  DECON_TRIG_HW_EN       (1u << 0)
#define  DECON_TRIG_MASK        (1u << 4)               /* HW_TRIG_MASK_DECON */
#define DECON_TRIG_SECURE       0x007C
#define DECON_FRAME_COUNT       0x02A0
#define DECON_OUTFIFO_COUNTER0  0x0474                  /* bits 16..29: the line being sent */
#define DECON_INT_FRAME_START   (1u << 12)              /* INTERRUPT_PENDING: a frame has started (write 1: cleared) */
#define TE_IDLE_WAIT_MS         40
#define TE_SETTLE_US            30

/*
 * fb_bl=1: the brightness CoreBrightness asks for ("IOMFBBrightnessLevel", nits in 16.16) sent to
 * the panel as DCS commands through DSIM0's command FIFO. The panel is Samsung's s6e3hab "hubble3":
 * brightness is its register 6A (DBV, 11 bits), set between the level-2 key F0 5A 5A / F0 A5 A5 and
 * latched by F7 0F (Samsung's exynos990 kernel, panel/s6e3hab/s6e3hab_hubble3_a3_s0_panel*.h).
 */
static TUNABLE(uint32_t, gBL, "fb_bl", 0);
#define DSIM0_PHYS              0x190C0000ULL
#define DSIM_LINK_STATUS1       0x000C
#define DSIM_CONFIG             0x004C
#define DSIM_INTSRC             0x0050
#define DSIM_PKTHDR             0x0058
#define DSIM_PAYLOAD            0x005C
#define DSIM_FIFOCTRL           0x0068
#define  DSIM_FIFO_PL_FULL      (1u << 9)
#define  DSIM_FIFO_PH_EMPTY     (1u << 10)
#define  DSIM_FIFO_PH_FULL      (1u << 11)
#define DSIM_CMD_CONFIG         0x0080
#define DSIM_RXFIFO             0x0060
#define  DSIM_FIFO_RX_EMPTY     (1u << 12)
#define  DSIM_INT_RX_DONE       (1u << 18)
#define  DSIM_PKTHDR_BTA        (1u << 24)
#define DSI_DCS_SHORT_WRITE     0x05
#define DSI_DCS_SHORT_WRITE_1   0x15
#define DSI_DCS_LONG_WRITE      0x39
#define DSI_DCS_READ            0x06
#define DSI_MAX_RETURN_SIZE     0x37
#define DSI_RX_ERROR_REPORT     0x02
#define DSI_RX_SHORT_1          0x21
#define DSI_RX_SHORT_2          0x22
#define DSI_RX_LONG             0x1C
#define  DSIM_CMD_PKT_GO_EN     (1u << 16)
/*
 * hubble2's brightness is its gamma (CA) and AOR (B1: of every frame, the lines the OLEDs are held
 * off), Samsung's tables of both come from the panel's own flash (s6e3hab.c init_aor_table, DIM_FLASH),
 * and 6A (DBV) alone does nothing. Without the gamma: the AOR as a dimmer, from what the panel was
 * found with (its brightness then) to Samsung's darkest (0x0C77 of some 3200 lines held off at 2 nits).
 */
#define PANEL_AOR_DARKEST       0x0C27                  /* (Samsung's 0x0C77 leaves 9 of 3200 lines lit: the colours go red) */
#define PANEL_AOR_BRIGHTEST     0x01E4                  /* Samsung's AOR from 265 nits up (AOR 0 makes the panel white) */
#define kBacklightMax           65536
#define kPanelMaxNits           420

/* hubble3: nits of each brightness step, and the DBV that gives it */
static const uint16_t gPanelNits[] = {
	2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20, 21, 22, 24, 25, 27, 29, 30, 32, 34, 37, 39, 41,
	44, 47, 50, 53, 56, 60, 64, 68, 72, 77, 82, 87, 93, 98, 105, 111, 119, 126, 134, 143, 152, 162, 172, 183, 195, 207,
	220, 234, 249, 265, 282, 300, 316, 333, 350, 357, 365, 372, 380, 387, 395, 403, 412, 420,
};
static const uint16_t gPanelDBV[] = {
	0x00A, 0x00F, 0x014, 0x018, 0x01D, 0x022, 0x027, 0x02B, 0x030, 0x035, 0x03A, 0x03E, 0x043, 0x048, 0x04D, 0x052, 0x05B,
	0x060, 0x065, 0x06A, 0x073, 0x078, 0x082, 0x08C, 0x090, 0x09A, 0x0A4, 0x0B2, 0x0BC, 0x0C6, 0x0D4, 0x0E3, 0x0F1, 0x100,
	0x10E, 0x121, 0x135, 0x148, 0x15C, 0x176, 0x18F, 0x1A8, 0x1C7, 0x1E0, 0x202, 0x21E, 0x245, 0x266, 0x28D, 0x2B8, 0x2E3,
	0x313, 0x343, 0x378, 0x3B3, 0x3ED, 0x42C, 0x471, 0x4BA, 0x508, 0x55A, 0x5B1, 0x5FE, 0x650, 0x6A4, 0x6C7, 0x6EF, 0x711,
	0x739, 0x75C, 0x783, 0x7AB, 0x7D7, 0x7FF,
};
static_assert(sizeof(gPanelNits) / sizeof(gPanelNits[0]) == sizeof(gPanelDBV) / sizeof(gPanelDBV[0]), "one DBV per step");

#include "MiniIOMobileFramebufferCalibration.h"
static TUNABLE(uint32_t, gFactoryAuto, "fb_cal", 1);      /* (on by default: user-verified 2026-11-07; checksum-gated) */
static uint8_t gFactoryData[2][7392], gFactorySavedGamma[34];
static bool gWarmOn;
static void warmApply(const uint64_t *v, unsigned n, char *out, size_t cap);
static bool gFactoryReady, gFactoryEnabled, gFactoryRestore;
static uint16_t gFactoryChecksums[2];
static bool factoryLoad(IOMobileFramebuffer *fb); /* caller holds fDsimLock */
static void factoryParameters(uint32_t level, uint32_t *aor, uint8_t *gamma);

/* A line for the loader's log (IOLog's buffer reaches it only when the kernel returns in order) and for IOLog. */
static void fbSay(const char * format, ...) __printflike(1, 2);
static void
fbSay(const char * format, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	IOLog("%s", line);
	loader_log_line(line);
}

static inline uint32_t
dpuRead(volatile uint8_t * base, uint32_t offset)
{
	return *(volatile uint32_t *) (base + offset);
}

static inline void
dpuWrite(volatile uint8_t * base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *) (base + offset) = value;
}

static uint64_t
dpuSmc(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 0);       /* 0 since 2026-11-18: what the S20 always ran with (its boot line said fb_beat=0) */
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
/*
 * Boot-arg fb_damage=0: every swap scales the whole frame onto the panel, as before. By default,
 * when the bottom layer is the only one, a copy of the picture last shown is kept and only the
 * panel rows over rows of the picture that changed are drawn again (a clock that ticks: a few
 * dozen rows of 3200). Comparing reads cached memory only; the panel's is slow to read and is never read.
 */
static TUNABLE(uint32_t, gDamage, "fb_damage", 1);
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/redwood_input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);
extern "C" uint32_t redwood_touch_lat_live;     /* kern.redwood.touch_lat, live (bsd/dev/redwood_input.c) */

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

/* x1slte: the refresh the panel was switched to (fb_mode=1: 120), which wins over fb_hz; 0 while none was */
static uint32_t gModeHz;

static inline uint32_t
refreshHz(void)
{
	if (gModeHz != 0) {
		return gModeHz;
	}
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	uint32_t        fSwapsQueued, fSwapsDone;       /* tickets: present() hands them out, the presenter retires them */
	uint64_t        fLatchTime;                     /* when the scaler's last frame was seen taken: the panel's phase */
	bool            fSwapQueuedUse[SWAP_QUEUE];     /* the slot's surface was counted in use when queued */
	uint32_t        fStatSwapWaitTimeouts;
	uint32_t        fShownSwapID, fQueuedSwapID;    /* the last swap on the panel, and the last queued (SwapWait) */
	uint32_t        fShownSid, fNextSid;            /* the surfaces the panel reads and has been asked for (ids) */
	uint32_t        fStatIntoShown;                 /* swaps whose surface was still shown or queued: would tear */
	/* what the swaps were (fb_swap line): which bottom-layer surfaces, how many asked for what the one before did */
	uint8_t         fPrevSwapLayers[SWAP_LAYERS_CLEARED + 4 - SWAP_SURFACE(0)];
	uint32_t        fStatSwapSame;                  /* swaps with the same surfaces, rectangles and layers as the one before */
	uint32_t        fStatSwapSids[8], fStatSwapSidsMore;    /* the bottom layer's different surfaces; beyond 8 */
	int             fStatSwapPid;                   /* who swapped last */
	uint32_t        fStatSwapWaits, fStatSwapWaitsBlocked, fStatSwapWaitTimeouts2;
	uint64_t        fStatSwapWaitTime;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	/* fb_damage: the picture of the bottom layer as last put on the panel, and where it went */
	uint8_t *       fShadow;
	size_t          fShadowSize;
	bool            fShadowValid;
	int32_t         fShadowKey[10];         /* sx, sy, sw, sh, px0, py0, px1, py1, filter, panel width */
	uint64_t        fStatRows, fStatRowsFull;       /* panel rows drawn, and rows a full frame each would have been */
	uint32_t        fStatSkipped;           /* frames in which nothing had changed */
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */
	/* fb_flip: the display controller, the two panel buffers, and which rows each still lacks */
	volatile uint8_t * fDecon;
	volatile uint8_t * fDma;                /* the IDMA of the channel that reads the panel's window */
	int             fWin;                   /* that window, -1: none found */
	bool            fFlipWanted, fFlip, fFlipPending, fFrameDrew, fShadowReadable;
	uint8_t *       fBuf[2];
	uint64_t        fBufPhys[2];
	unsigned        fFront;                 /* the buffer being read */
	int32_t         fPrevRy0, fPrevRy1;     /* rows the last frame changed: the buffer drawn next lacks them */
	int32_t         fOwnRy0, fOwnRy1;       /* rows this frame changes */
	uint64_t        fFlipStart;
	uint32_t        fStatFlips, fStatFlipLate;
	uint64_t        fStatFlipWait;
	char            fDpuWhy[160];           /* fb_flip: why it is not on, said again with every report */
	/* fb_scaler */
	volatile uint8_t * fDma5;               /* channel 5's IDMA and DPP */
	volatile uint8_t * fDpp5;
	volatile uint8_t * fDppT;               /* the DPP of S-Boot's channel: what channel 5 copies */
	unsigned        fScanCh;                /* S-Boot's channel, which the window goes back to */
	bool            fScalerReady, fScalerOn, fScanPending;
	MiniIOSurface * fScanShown;             /* the surface the panel reads (held, and in use for its maker) */
	MiniIOSurface * fScanNext;              /* the one asked for, taken at the next frame */
	uint32_t        fScanAsked;             /* its address */
	uint64_t        fScanStart;
	uint32_t        fStatScan, fStatScanCpu, fStatScanNotTaken;
	uint64_t        fStatScanWait;
	char            fScalerWhy[200];
	void scalerSetup(void);
	bool scalerFits(const uint8_t * swap, MiniIOSurface ** surfaces);
	bool scalerWait(void);
	void scalerShow(MiniIOSurface * s);
	void loaderQuiesce(void);
	void scalerOff(const char * why, bool forever);
	char            fDpuDump[512];          /* fb_dpu/fb_flip: what the controller was found doing, likewise */
	/* fb_te: frames drawn into fBack, then copied under a masked trigger into fPanel (the buffer DECON reads) */
	bool            fTe;
	uint8_t *       fPanel;                 /* the boot framebuffer itself */
	uint8_t *       fBack;
	size_t          fBackSize;
	uint32_t        fTeTrig;                /* HW_SW_TRIG_CONTROL as found (the trigger not masked) */
	uint32_t        fTeFrames, fTeRaced, fTeTimeouts, fTeNeverIdle, fTeLate;
	bool            fTeStartFlag;           /* INTERRUPT_PENDING's frame-start bit can be cleared and comes back with a frame */
	uint64_t        fTeIdleWait, fTeIdleWaitMax, fTeCopy, fTeRows;
	uint32_t        fTeCount0, fTeFrameCount0;      /* TE and frame counters at the last report */
	uint64_t        fTeSince;
	/* fb_bl: the panel's brightness through DSIM0 */
	volatile uint8_t * fDsim;
	IOLock *        fBLLock;
	thread_call_t   fBLCall;
	uint32_t        fBLTarget;              /* nits in 16.16, as CoreBrightness gives it */
	uint32_t        fBLSent;                /* AOR of the last successful write */
	uint32_t        fBLDBVSent, fBLFailStreak, fBLFactoryLevel;
	bool            fBLValid;              /* both DBV and AOR known to have been sent */
	uint32_t        fBLCurrent;             /* nits in 16.16 last sent (where a fade starts from) */
	uint32_t        fFadeFrom;              /* the fade: from, when it began, how long (absolute time; 0: none) */
	uint64_t        fFadeStart, fFadeLen;
	uint32_t        fBLRequests, fBLWrites, fBLFailures;
	bool            fBLOff;

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void swapWait(uint32_t id);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst, bool damage);
	bool damageRows(const struct RowJob * job, int32_t * y0, int32_t * y1);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
	void dpuProbe(void);
	void flipEnable(void);
	void flipWait(void);
	void flipNow(void);
	void runJob(struct RowJob * job);
	void teSetup(void);
	bool            fFrameIrq;              /* fb_irq: the frame-start interrupt is attached */
	volatile uint32_t fFrameStarts;         /* frame starts it has seen */
	uint32_t        fStatLatchIrq, fStatLatchCap, fStatLatchLimit;
	void frameIrqSetup(void);
	static void frameIrq(void * context, uint32_t intid);
	bool latchWait(uint32_t mask, uint64_t start);
	uint32_t frameStartMark(void);
	bool frameStartSince(uint32_t mark);
	void teMeasure(void);
	void tePresent(int32_t y0, int32_t y1);
	void teReport(uint64_t nanos);
	void blSetup(void);
	bool dsimWrite(const uint8_t * bytes, uint32_t length);
	bool dsimPacket(uint8_t type, const uint8_t * bytes, uint32_t length);
	bool panelWriteAt(const uint8_t * bytes, uint32_t length, uint8_t offset);
	bool modeSetFhd120(void);
	/* live display mode (B, 2026-11-07): fb.mode N, run on the presenter thread between two frames */
	uint32_t        fModeAsk;               /* 1:1080/120, 2:1080/60, 3:720/120, 4:720/60 (native at boot) */
	volatile int    fModeWant;              /* -1: nothing asked */
	bool            fModeLpRef;             /* fb.mode N 1: as fb_mode_lpref=1 */
	uint32_t        fModeLpVal;             /* fb.mode N 1 L: the underrun reference: 0 Samsung's, 1 as found, else L */
	volatile bool   fModeDone;
	int             fWarmWant;
	bool            fWarmDone;
	char            fWarmReport[256];
	char            fModeReport[1536];
	void modeLive(int mode);
	/* the power-off fade on the picture (gOffAlpha) */
	bool            fAlphaOn;               /* the window's blend registers are ours (saved below) */
	uint32_t        fWinCtl0Saved, fWinCtl1Saved;
	uint64_t        fAlphaStart, fAlphaLen; /* mach time; len 0: no fade running */
	thread_call_t   fAlphaCall;
	void alphaSet(uint32_t alpha);
	bool            fAlphaUp;               /* the fade runs up (power on), not down */
	void alphaFadeIn(void);
	void alphaRestore(void);
	bool            fFramesMasked;          /* DECON's trigger masked while the display is off (no frames into a dark panel) */
	void offFrames(bool off);
	static void alphaWork(thread_call_param_t self, thread_call_param_t unused);
	void modeCounters(const char * when, char * out, size_t cap);
	int dsimRead(uint8_t command, uint8_t * out, uint32_t max);
	void panelReport(const char * when);
	bool blSend(uint32_t dbv, uint32_t aor, const uint8_t *gamma = NULL, bool smooth = true);
	uint32_t aorRead(void);
	uint32_t        fAor0;                  /* the AOR (B1) the panel was found with: its full brightness */
	bool            fPacketGo;              /* DSIM writes with PKT_GO, as Samsung's dsim_write_data() does */
	void brightnessRequest(uint32_t level, uint32_t fade16 = 0);
	void displayPower(bool on);
	IOLock *        fDsimLock;              /* one DSIM command sequence at a time */
	bool            fDsimReads;             /* DCS reads work (a read timed out: none tried again) */
	bool            fDisplayOff;
	/* the display off held until the panel's dimming has settled (fb_dim): its thread call, when brightness was last sent */
	thread_call_t   fOffCall;
	uint64_t        fBLSentAt;
	bool            fOffPending;
	static void     offWork(thread_call_param_t self, thread_call_param_t unused);
	static void blWork(thread_call_param_t self, thread_call_param_t unused);
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	/* where a frame's time in the driver goes (5 s sums) */
	uint64_t        fStatCompare, fStatScale;
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);
static IOMobileFramebuffer * gFramebuffer;      /* the one there is (redwood_fb_quiesce) */
static volatile uint32_t gBLThermalCap;
/*
 * Always-On Display (A, 2026-11-15): kern.redwood.display_aod, set by SpringBoard's AOD service on the backlight's
 * InactiveOn transition. iOS's own AOD brightness curve never reaches this driver, so the level is made here, in
 * SLIDER terms: AOD sits at fb_aod_pct (default 30; live: kern.redwood.poke "fb.aod PCT") % of the slider position the screen had when AOD began, on the
 * same slider -> nits curve as legacyToNits16 (2 + 418 s^2.2 nits). 100 % -> 30 %, 50 % -> 15 %, 25 % -> 8 %: about
 * 7 % of the light, clearly dimmer than the awake screen by the same step at every level (30 % of the NITS looked
 * ~2/3 as bright, 35 % of the slider still too close: the user chose 3/10). Floor: the panel's 2 nits.
 * Then HELD: auto-brightness keeps sending small changes during AOD (a dark room's sensor wobble showed at the
 * panel's 1-nit bottom steps); those are kept for the wake but do not move the AOD picture. Only a big change of
 * light (CoreBrightness asking 2x or 1/2 of the level AOD started from) takes the level again. Faded in and out;
 * CoreBrightness's exact level comes back on wake. 0 off; 1 the default; 2..60 a fixed ceiling in nits.
 */
static volatile uint32_t gAODDim;
static uint32_t gAODBase, gAODLevel;    /* (fBLLock) nits 16.16 AOD started from, and the level held */
static TUNABLE(uint32_t, gAODPct, "fb_aod_pct", 30);
static volatile uint32_t gAODPctLive;    /* fb.aod PCT (0: the boot value) */
static TUNABLE(uint32_t, gAODMaxNits, "fb_aod_max", 60);
/* (p / 100)^2.2 in 1/65536 for p = 10, 15 .. 60: a slider fraction p of s is (p/100)^2.2 of the nits above 2 */
static const uint32_t gAODSliderFactor[11] = { 414, 1009, 1900, 3104, 4636, 6508, 8730, 11312, 14263, 17590, 21302 };

static uint32_t
aodFrom(uint32_t base)
{
	uint32_t lo = 2u << 16, hi = (gAODMaxNits ? gAODMaxNits : 60) << 16, pct = gAODPctLive ? gAODPctLive : gAODPct, level;

	pct = pct < 10 ? 10 : pct > 60 ? 60 : pct;
	if (base <= lo) {
		return base;
	}
	level = lo + (uint32_t) (((uint64_t) (base - lo) * gAODSliderFactor[(pct - 10) / 5]) >> 16);
	return level > hi ? hi : level;
}

/* (fBLLock held) */
static uint32_t
aodLevel(uint32_t target)
{
	uint32_t mode = gAODDim;

	if (mode == 0 || target == 0) {
		return target;
	}
	if (mode >= 2) {
		uint32_t hi = mode << 16;

		return target < hi ? target : hi;
	}
	if (gAODBase == 0 || (uint64_t) target >= 2ull * gAODBase || 2ull * target <= gAODBase) {
		gAODBase = target;              /* (the first request, or a real change of light) */
		gAODLevel = aodFrom(target);
	}
	return gAODLevel < target ? gAODLevel : target;
}
/* the window server's refresh notifications (kFBNotifyVSync): asked for, sent, not sent (no port, or closed) */
static volatile UInt32 gVSyncAsked, gVSyncSent, gVSyncStopped;         /* nits in 16.16 the panel is held under while the chip is hot; 0: none */

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	gFramebuffer = this;
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	/*
	 * The drawing follows the panel's mode (B, 2026-11-14): at 720 x 1600 (fb_mode=3/4) the iPhone's
	 * 1125 x 2436 was drawn and then shrunk to 0.42 of its pixels on every frame -- 2.4 times the GPU's
	 * work for nothing shown. There iOS draws the same 375 x 812 points at two pixels a point
	 * (750 x 1624, its @2x artwork), about what the panel shows. 1080 x 2400 and the native mode keep
	 * three. fbscale= given on the command line wins (fbscale=3: as before).
	 */
	uint32_t autoScale = gFBScale != 0 ? gFBScale : (gScaler && (gFbMode == 3 || gFbMode == 4)) ? 2 : 0;
	if (autoScale != 0 && autoScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = autoScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);
	fWin = -1;
	fPanel = fBase;
	if (gFlip || gDPU || gScaler) {
		dpuProbe();
	}
	if (gTE && !gFlip) {
		teSetup();
	}
	if (gFrameIrq) {
		frameIrqSetup();        /* (after teSetup: its measuring reads the frame-start flag itself) */
	}
	blSetup();

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	fModeWant = -1;
	fWarmWant = -1;
	gFbLive = this;
	redwood_dbgport_add("fb.fade", "[dd [offms [alpha]]]: panel dimming during fades, the power-off fade at least offms (0: iOS's), on the picture (alpha 1) or the backlight (0)",
	    dbg_fb_fade);
	redwood_dbgport_add("fb.mode", "[1..4 [lpref]]: live mode (1=1080/120, 2=1080/60, 3=720/120, 4=720/60), "
	    "with DECON's TE and frame counters over 100 ms before and after; no number: the counters now", dbg_fb_mode);
	redwood_dbgport_add("fb.cal", "[address length]: read panel calibration SPI flash (JEDEC ID if no args; factory range 0xf0000..0xf8fff, max 128 bytes)", dbg_fb_cal);
	redwood_dbgport_add("fb.aod", "[PCT]: Always-On Display level, PCT (10..60) % of the slider position, live", dbg_fb_aod);
	redwood_dbgport_add("fb.warm", "[0|1]: optional mild software True Tone approximation, restore exact prior DQE state on disable", dbg_fb_warm);
	redwood_dbgport_add("fb.factory", "[0|1]: factory gamma/AOR status, disable/restore, or load/enable verified normal and 120 Hz calibration", dbg_fb_factory);
	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("RedwoodWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	int32_t         ry0, ry1;               /* the panel rows to draw: all of py0..py1, or those fb_damage found changed */
	bool            filter, unscaled;
	bool            copy;                   /* fb_te: not drawing but rows ry0..ry1 of the back buffer to the panel */
	bool            compare;                /* fb_damage: not drawing but picture rows ry0..ry1 compared with fShadow */
	int32_t *       changed;                /* ... the first and last that differed, gathered from every share */
};

static bool rowsDiffer(const uint8_t * a, const uint8_t * b, size_t bytes);

static void
atomicMin(int32_t * at, int32_t value)
{
	int32_t seen = __atomic_load_n(at, __ATOMIC_RELAXED);

	while (value < seen && !__atomic_compare_exchange_n(at, &seen, value, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

static void
atomicMax(int32_t * at, int32_t value)
{
	int32_t seen = __atomic_load_n(at, __ATOMIC_RELAXED);

	while (value > seen && !__atomic_compare_exchange_n(at, &seen, value, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	if (job->compare) {
		size_t rowBytes = (size_t) sw * 4;
		int32_t first = -1, last = -1;

		for (int32_t r = y0; r < y1; r++) {
			const uint8_t * now = pixels + (size_t) (sy + r) * stride + (size_t) sx * 4;
			uint8_t * then = fShadow + (size_t) r * rowBytes;

			if (rowsDiffer(now, then, rowBytes)) {
				memcpy(then, now, rowBytes);
				first = first < 0 ? r : first;
				last = r;
			}
		}
		if (first >= 0) {
			atomicMin(&job->changed[0], first);
			atomicMax(&job->changed[1], last);
		}
		return;
	}
	if (job->copy) {
		for (int32_t y = y0 < 0 ? 0 : y0; y < y1 && (uint32_t) y < fPanelHeight; y++) {
			memcpy(fPanel + (size_t) y * fRowBytes, fBack + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
		}
		return;
	}
	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
}

/*
 * fb_dpu / fb_flip: what the bootloader left the display controller doing. The window that is on
 * (DATA_PATH_CONTROL_0), the DMA channel it reads (DATA_PATH_CONTROL_1) and where that channel reads
 * from; fb_flip goes on only if that is the boot framebuffer itself, i.e. the DMA sees physical
 * addresses (no SYSMMU in the way) and the window is the one the panel shows.
 */
void
IOMobileFramebuffer::dpuProbe(void)
{
	uint32_t paths0, paths1, ch, base, shadow;
	int on = 0;

	fDecon = (volatile uint8_t *) ml_io_map((vm_offset_t) DPU_DECON_F_PHYS, 0x10000);
	if (fDecon == NULL) {
		IOLog("MiniIOMobileFramebuffer: dpu: DECON_F not mapped\n");
		strlcpy(fDpuWhy, "DECON_F not mapped", sizeof(fDpuWhy));
		return;
	}
	paths0 = dpuRead(fDecon, DECON_DATA_PATH_0);
	paths1 = dpuRead(fDecon, DECON_DATA_PATH_1);
	{
		volatile uint8_t * com = (volatile uint8_t *) ml_io_map((vm_offset_t) DPU_DMA_PHYS, 0x1000);
		volatile uint8_t * dma0 = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DMA_PHYS + 0x1000), 0x1000);
		volatile uint8_t * dpp0 = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DPP_PHYS + 0x1000), 0x1000);

		for (int round = 0; com != NULL && dma0 != NULL && dpp0 != NULL && round < 2; round++) {
			fbSay("fb: dpu: %s: DMA common version %#x qch %#x cgen %#x; IDMA0 enable %#x size %#x base %#x (in use %#x); "
			    "DPP0 enable %#x; DECON global %#x trig %#x\n", round ? "after the D-TZPC restore" : "as found",
			    dpuRead(com, DPU_DMA_VERSION), dpuRead(com, DPU_DMA_QCH_EN), dpuRead(com, DPU_DMA_GLB_CGEN_CH0),
			    dpuRead(dma0, IDMA_ENABLE_REG), dpuRead(dma0, IDMA_SRC_SIZE_REG), dpuRead(dma0, IDMA_BASE_Y8_REG),
			    dpuRead(dma0, IDMA_BASE_Y8_REG + IDMA_SHADOW), dpuRead(dpp0, 0), dpuRead(fDecon, DECON_GLOBAL_CONTROL),
			    dpuRead(fDecon, DECON_TRIG_CONTROL));
			if (round == 0) {
				if (gDPU < 2) {
					break;
				}
				fbSay("fb: dpu: smc(%#lx, 1, %#lx, 2) = %#llx\n", SMC_PREPARE_PD_ONOFF, PD_DPU_NEED_SMC,
				    dpuSmc(SMC_PREPARE_PD_ONOFF, 1, PD_DPU_NEED_SMC, 2));
			}
		}
	}
	{
		/* every input DMA channel's enable, size and base (and the base in use), and DECON's own state */
		size_t at = (size_t) snprintf(fDpuDump, sizeof(fDpuDump), "DECON global %#x trig %#x shadow %#x te %#x paths %#x/%#x;",
		    dpuRead(fDecon, DECON_GLOBAL_CONTROL), dpuRead(fDecon, DECON_TRIG_CONTROL), dpuRead(fDecon, DECON_SHADOW_UPDATE),
		    dpuRead(fDecon, DECON_TE_COUNT), paths0, paths1);
		for (unsigned c = 0; c < 6 && at < sizeof(fDpuDump); c++) {
			volatile uint8_t * dma = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DMA_PHYS + 0x1000ULL * (c + 1)), 0x1000);

			if (dma == NULL) {
				continue;
			}
			at += (size_t) snprintf(fDpuDump + at, sizeof(fDpuDump) - at, " ch%u en %#x size %#x base %#x/%#x;", c,
			    dpuRead(dma, IDMA_ENABLE_REG), dpuRead(dma, IDMA_SRC_SIZE_REG), dpuRead(dma, IDMA_BASE_Y8_REG),
			    dpuRead(dma, IDMA_BASE_Y8_REG + IDMA_SHADOW));
		}
		loader_log_line("MiniIOMobileFramebuffer: dpu: ");
		loader_log_line(fDpuDump);
		loader_log_line("\n");
	}
	IOLog("MiniIOMobileFramebuffer: dpu: DECON_F global %#x trigger %#x shadow %#x TE count %#x, windows on %#x, channels %#x\n",
	    dpuRead(fDecon, DECON_GLOBAL_CONTROL), dpuRead(fDecon, DECON_TRIG_CONTROL), dpuRead(fDecon, DECON_SHADOW_UPDATE),
	    dpuRead(fDecon, DECON_TE_COUNT), paths0, paths1);
	for (int w = 0; w < 6; w++) {
		if (paths0 & (1u << (4 * w))) {
			fWin = w;
			on++;
		}
	}
	if (on != 1) {
		IOLog("MiniIOMobileFramebuffer: dpu: %d windows on, not one: left alone\n", on);
		snprintf(fDpuWhy, sizeof(fDpuWhy), "%d windows on (paths %#x %#x)", on, paths0, paths1);
		fWin = -1;
		return;
	}
	ch = (paths1 >> (4 * fWin)) & 7;
	if (ch > 5) {
		IOLog("MiniIOMobileFramebuffer: dpu: window %d reads channel %u, which has no IDMA: left alone\n", fWin, ch);
		snprintf(fDpuWhy, sizeof(fDpuWhy), "window %d on channel %u", fWin, ch);
		fWin = -1;
		return;
	}
	fDma = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DMA_PHYS + 0x1000ULL * (ch + 1)), 0x1000);
	if (fDma == NULL) {
		fWin = -1;
		return;
	}
	if (gScaler && !gFlip) {
		fScanCh = ch;
		if (gFbMode >= 1 && gFbMode <= 4) {
			fModeAsk = gFbMode;
			(void) modeSetFhd120();         /* the scaler's target follows it (fPanelWidth/Height) */
		}
		scalerSetup();
	}
	base = dpuRead(fDma, IDMA_BASE_Y8_REG);
	shadow = dpuRead(fDma, IDMA_BASE_Y8_REG + IDMA_SHADOW);
	IOLog("MiniIOMobileFramebuffer: dpu: window %d, channel %u: IDMA enable %#x in %#x size %#x offset %#x image %#x "
	    "stride %#x, base %#x (in use %#x); the boot framebuffer is at %#llx\n", fWin, ch, dpuRead(fDma, IDMA_ENABLE_REG),
	    dpuRead(fDma, IDMA_IN_CON_REG), dpuRead(fDma, IDMA_SRC_SIZE_REG), dpuRead(fDma, IDMA_SRC_OFFSET_REG),
	    dpuRead(fDma, IDMA_IMG_SIZE_REG), dpuRead(fDma, IDMA_STRIDE_REG), base, shadow, vinfo.v_physaddr);
	if (!gFlip) {
		return;
	}
	if ((uint64_t) shadow != vinfo.v_physaddr && (uint64_t) base != vinfo.v_physaddr) {
		IOLog("MiniIOMobileFramebuffer: dpu: the channel does not read the boot framebuffer (an SMMU address?): no fb_flip\n");
		snprintf(fDpuWhy, sizeof(fDpuWhy), "window %d channel %u reads %#x / %#x, not the boot framebuffer %#llx", fWin, ch, base, shadow, vinfo.v_physaddr);
		return;
	}
	fShadowReadable = (uint64_t) shadow == vinfo.v_physaddr;
	/* the second buffer: one piece, below 4 GB (the register holds 32 bits), written through the write buffer */
	IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache,
	    (mach_vm_size_t) fRowBytes * fPanelHeight, 0x00000000FFFFF000ULL);
	if (memory == NULL) {
		IOLog("MiniIOMobileFramebuffer: dpu: no memory for a second panel buffer: no fb_flip\n");
		strlcpy(fDpuWhy, "no memory for a second buffer", sizeof(fDpuWhy));
		return;
	}
	fBuf[0] = (uint8_t *) vinfo.v_baseaddr;
	fBufPhys[0] = vinfo.v_physaddr;
	fBuf[1] = (uint8_t *) memory->getBytesNoCopy();
	fBufPhys[1] = memory->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
	fFlipWanted = true;
	IOLog("MiniIOMobileFramebuffer: dpu: fb_flip: frames go to %#llx and %#llx in turn\n", fBufPhys[0], fBufPhys[1]);
}

/* The first picture: from here on the frame is drawn where the panel is not reading. */
void
IOMobileFramebuffer::flipEnable(void)
{
	for (uint32_t y = 0; y < fPanelHeight; y++) {
		memcpy(fBuf[1] + (size_t) y * fRowBytes, fBuf[0] + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
	}
	fFront = 0;
	fBase = fBuf[1];
	fPrevRy0 = fPrevRy1 = 0;
	fFlip = true;
}

/* Before drawing: the switch to the frame before has been taken, so the buffer drawn into is nobody's. */
void
IOMobileFramebuffer::flipWait(void)
{
	uint32_t mask = DECON_SHADOW_GLOBAL | (1u << fWin);
	uint64_t began = mach_absolute_time(), limit, waited;

	if (!fFlipPending) {
		return;
	}
	(void) limit;
	latchWait(mask, fFlipStart);
	waited = mach_absolute_time() - began;
	fStatFlipWait += waited;
	fFlipPending = false;
	uint32_t inUse = dpuRead(fDma, IDMA_BASE_Y8_REG + IDMA_SHADOW);
	bool taken = fShadowReadable ? inUse == (uint32_t) fBufPhys[fFront] :
	    (dpuRead(fDecon, DECON_SHADOW_UPDATE) & mask) == 0;
	if (fStatFlips <= 4 || !taken) {
		uint64_t since;

		absolutetime_to_nanoseconds(mach_absolute_time() - fFlipStart, &since);
		IOLog("MiniIOMobileFramebuffer: dpu: flip %u: the channel reads %#x (asked %#llx) %llu us after the switch; shadow %#x\n",
		    fStatFlips, inUse, fBufPhys[fFront], since / 1000, dpuRead(fDecon, DECON_SHADOW_UPDATE));
	}
	if (!taken && ++fStatFlipLate >= 3) {
		/* the controller does not take the switch: back to one buffer, the boot framebuffer */
		IOLog("MiniIOMobileFramebuffer: dpu: the switch is not taken (%u times): fb_flip off\n", fStatFlipLate);
		snprintf(fDpuWhy, sizeof(fDpuWhy), "the switch was not taken (in use %#x, shadow %#x)", inUse, dpuRead(fDecon, DECON_SHADOW_UPDATE));
		dpuWrite(fDma, IDMA_BASE_Y8_REG, (uint32_t) fBufPhys[0]);
		dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | mask);
		fFlip = fFlipWanted = false;
		fBase = fBuf[0];
		fShadowValid = false;
	}
}

/* After drawing: the panel reads the buffer just drawn from its next frame on. */
void
IOMobileFramebuffer::flipNow(void)
{
	unsigned drawn = fFront ^ 1;

	__asm__ volatile ("dsb sy" ::: "memory");
	dpuWrite(fDma, IDMA_BASE_Y8_REG, (uint32_t) fBufPhys[drawn]);
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | DECON_SHADOW_GLOBAL | (1u << fWin));
	fFlipStart = mach_absolute_time();
	fFlipPending = true;
	fFront = drawn;
	fBase = fBuf[drawn ^ 1];
	fPrevRy0 = fOwnRy0;
	fPrevRy1 = fOwnRy1;
	fStatFlips++;
}

/*
 * fb_te: how DECON sends frames, watched for 100 ms before the first one: the TE pulses, the frames
 * sent, how long each takes (the IDLE bit of GLOBAL_CONTROL is clear while one is being sent) and how
 * long after its TE it starts; then 60 ms with the trigger masked, in which no frame should go out.
 */
/*
 * fb_scaler: channel 5 made ready once -- S-Boot's channel's settings, this picture's sizes, the
 * scaler -- while it is in nobody's path. The window keeps reading S-Boot's channel until the first
 * frame that fits (scalerShow()).
 */
void
IOMobileFramebuffer::scalerSetup(void)
{
	static const uint32_t idmaCopied[] = {
		IDMA_IN_CON_REG, IDMA_STRIDE_REG, IDMA_STRIDE_1_REG, 0x0064 /* SBWC */, 0x0070 /* recovery */,
		0x0100 /* deadlock */, 0x0110 /* bus */, 0x0114 /* cache */, 0x0120 /* performance */, 0x0130, 0x0134 /* QoS */,
		0x0140 /* dynamic gating */,
	};
	uint32_t paths0 = dpuRead(fDecon, DECON_DATA_PATH_0), paths1 = dpuRead(fDecon, DECON_DATA_PATH_1);
	uint32_t w = fWidth, h = fHeight, pw = fPanelWidth, ph = fPanelHeight, a, b;

	if (fScanCh == SCALER_CH) {
		snprintf(fScalerWhy, sizeof(fScalerWhy), "S-Boot's window reads channel %u already", SCALER_CH);
		goto refused;
	}
	for (int win = 0; win < 6; win++) {
		if ((paths0 & (1u << (4 * win))) && ((paths1 >> (4 * win)) & 7) == SCALER_CH) {
			snprintf(fScalerWhy, sizeof(fScalerWhy), "window %d reads channel %u", win, SCALER_CH);
			goto refused;
		}
	}
	fDma5 = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DMA_PHYS + 0x1000ULL * (SCALER_CH + 1)), 0x1000);
	fDpp5 = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DPP_PHYS + 0x1000ULL * (SCALER_CH + 1)), 0x1000);
	fDppT = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DPP_PHYS + 0x1000ULL * (fScanCh + 1)), 0x1000);
	if (fDma5 == NULL || fDpp5 == NULL || fDppT == NULL) {
		strlcpy(fScalerWhy, "channel 5 not mapped", sizeof(fScalerWhy));
		goto refused;
	}
	/* are its registers there at all (clock and D-TZPC): two that must hold what is written */
	dpuWrite(fDma5, IDMA_SRC_OFFSET_REG, 0x00020004);
	dpuWrite(fDpp5, DPP_IMG_SIZE_REG, 0x00100020);
	a = dpuRead(fDma5, IDMA_SRC_OFFSET_REG);
	b = dpuRead(fDpp5, DPP_IMG_SIZE_REG);
	if (a != 0x00020004 || b != 0x00100020) {
		snprintf(fScalerWhy, sizeof(fScalerWhy), "channel 5's registers do not hold what is written (IDMA %#x, DPP %#x)", a, b);
		goto refused;
	}
	/*
	 * A row's length is SRC_SIZE's width, in pixels: the whole buffer is f_w x f_h, the picture IMG_SIZE
	 * of it at SRC_OFFSET (idma_reg_set_coordinates, cal_9830/dpp_reg.c:77-85). The stride registers
	 * count only with IDMA_PLANE_n_STRIDE_SEL set, which S-Boot leaves off (it has 0 in both, and
	 * SRC_SIZE 1440 x 3200 for its 5760 bytes a row). A run that looked for the row in them refused.
	 */
	if ((dpuRead(fDma, IDMA_STRIDE_REG) & (0xFu << 20)) != 0 || (dpuRead(fDma, IDMA_SRC_SIZE_REG) & 0xffff) * 4 != fRowBytes) {
		snprintf(fScalerWhy, sizeof(fScalerWhy), "S-Boot's channel is not what was expected: stride select %#x, size %#x for %u "
		    "bytes a row", dpuRead(fDma, IDMA_STRIDE_REG), dpuRead(fDma, IDMA_SRC_SIZE_REG), fRowBytes);
		goto refused;
	}

	/* IDMA 5: S-Boot's channel's settings (its format is CoreAnimation's byte order: the copy path copies as it is) */
	dpuWrite(fDma5, IDMA_ENABLE_REG, dpuRead(fDma, IDMA_ENABLE_REG) & 0xff000000u);        /* outstanding reads */
	dpuWrite(fDma5, IDMA_IRQ_REG, IDMA_IRQ_PENDING | (dpuRead(fDma, IDMA_IRQ_REG) & 0xffffu));
	for (unsigned i = 0; i < sizeof(idmaCopied) / sizeof(idmaCopied[0]); i++) {
		dpuWrite(fDma5, idmaCopied[i], dpuRead(fDma, idmaCopied[i]));
	}
	dpuWrite(fDma5, IDMA_SRC_OFFSET_REG, 0);
	dpuWrite(fDma5, IDMA_SRC_SIZE_REG, (h << 16) | w);       /* (the width a row: set again with every frame's surface) */
	dpuWrite(fDma5, IDMA_IMG_SIZE_REG, (h << 16) | w);
	dpuWrite(fDma5, IDMA_BASE_Y8_REG, (uint32_t) vinfo.v_physaddr);        /* (memory that is there, until a frame) */

	/* DPP 5: S-Boot's DPP's settings, then the scaler */
	dpuWrite(fDpp5, DPP_ENABLE_REG, dpuRead(fDppT, DPP_ENABLE_REG) & DPP_ENABLE_COPIED);
	dpuWrite(fDpp5, DPP_IRQ_REG, DPP_IRQ_PENDING | (dpuRead(fDppT, DPP_IRQ_REG) & 0xffffu));
	dpuWrite(fDpp5, DPP_IN_CON_REG, dpuRead(fDppT, DPP_IN_CON_REG));
	for (uint32_t r = 0x30; r <= 0x40; r += 4) {
		dpuWrite(fDpp5, r, dpuRead(fDppT, r));                  /* CSC */
	}
	dpuWrite(fDpp5, DPP_DYNAMIC_GATING_REG, dpuRead(fDppT, DPP_DYNAMIC_GATING_REG));
	dpuWrite(fDpp5, DPP_IMG_SIZE_REG, (h << 16) | w);
	dpuWrite(fDpp5, DPP_SCALED_IMG_SIZE_REG, (ph << 16) | pw);
	dpuWrite(fDpp5, DPP_MAIN_H_RATIO_REG, (uint32_t) (((uint64_t) w << 20) / pw));
	dpuWrite(fDpp5, DPP_MAIN_V_RATIO_REG, (uint32_t) (((uint64_t) h << 20) / ph));
	for (unsigned n = 0; n < 9; n++) {
		for (unsigned x = 0; x < 2; x++) {
			for (unsigned t = 0; t < 4; t++) {
				dpuWrite(fDpp5, DPP_V_COEF(n, t, x), (uint32_t) scalerBicubic[n][t] & 0x7ff);
			}
			for (unsigned t = 0; t < 8; t++) {
				dpuWrite(fDpp5, DPP_H_COEF(n, t, x), (uint32_t) (t >= 2 && t <= 5 ? scalerBicubic[n][t - 2] : 0) & 0x7ff);
			}
		}
	}
	for (unsigned i = 0; i < 4; i++) {
		dpuWrite(fDpp5, DPP_POSITION_REG(i), 0);
	}
	/* taken at once: the channel is in nobody's path */
	dpuWrite(fDma5, IDMA_ENABLE_REG, dpuRead(fDma5, IDMA_ENABLE_REG) | IDMA_SFR_UPDATE_FORCE);
	dpuWrite(fDpp5, DPP_ENABLE_REG, dpuRead(fDpp5, DPP_ENABLE_REG) | DPP_SFR_UPDATE_FORCE);
	fScalerReady = true;
	fbSay("fb_scaler: channel %u ready for window %d (S-Boot's channel %u): %ux%u -> %ux%u, ratios %#x %#x; "
	    "S-Boot's IDMA enable %#x irq %#x in %#x req %#x, DPP enable %#x in %#x\n", SCALER_CH, fWin, fScanCh, w, h, pw, ph,
	    dpuRead(fDpp5, DPP_MAIN_H_RATIO_REG), dpuRead(fDpp5, DPP_MAIN_V_RATIO_REG),
	    dpuRead(fDma, IDMA_ENABLE_REG), dpuRead(fDma, IDMA_IRQ_REG), dpuRead(fDma, IDMA_IN_CON_REG),
	    dpuRead(fDma, IDMA_IN_REQ_DEST_REG), dpuRead(fDppT, DPP_ENABLE_REG), dpuRead(fDppT, DPP_IN_CON_REG));
	fbSay("fb_scaler: channel %u now: IDMA enable %#x irq %#x in %#x size %#x req %#x, DPP enable %#x irq %#x config errors %#x\n",
	    SCALER_CH, dpuRead(fDma5, IDMA_ENABLE_REG), dpuRead(fDma5, IDMA_IRQ_REG), dpuRead(fDma5, IDMA_IN_CON_REG),
	    dpuRead(fDma5, IDMA_SRC_SIZE_REG), dpuRead(fDma5, IDMA_IN_REQ_DEST_REG), dpuRead(fDpp5, DPP_ENABLE_REG),
	    dpuRead(fDpp5, DPP_IRQ_REG), dpuRead(fDpp5, DPP_CFG_ERR_STATE_REG));
	return;
refused:
	fbSay("fb_scaler: not used: %s\n", fScalerWhy);
}

/* A swap the controller can show as it is: one full-screen layer, BGRA, in one piece of memory below 4 GB. */
bool
IOMobileFramebuffer::scalerFits(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	MiniIOSurface * s = surfaces[0];
	int32_t src[4], dst[4];

	if (!fScalerReady || fDisplayOff || s == NULL || swapWord(swap, SWAP_SURFACE(1)) != 0 ||
	    swapWord(swap, SWAP_SURFACE(2)) != 0 || swapWord(swap, SWAP_SURFACE(3)) != 0) {
		return false;
	}
	memcpy(src, swap + SWAP_SOURCE(0), sizeof(src));
	memcpy(dst, swap + SWAP_DEST(0), sizeof(dst));

	const char *why = s->phys == 0 ? "its memory is not in one piece" :
	    s->info.planeCount != 0 || s->info.bytesPerElement != 4 || s->info.pixelFormat != 0x42475241 ? "not BGRA" :
	    s->info.width != fWidth || s->info.height != fHeight ? "not the display's size" :
	    s->info.bytesPerRow % 16 != 0 || s->info.bytesPerRow / 4 < fWidth || s->info.bytesPerRow / 4 > 0x3fff ? "its rows" :
	    s->phys + s->info.offset + (uint64_t) s->info.bytesPerRow * fHeight > (1ULL << 32) ? "above 4 GB" :
	    src[0] != 0 || src[1] != 0 || src[2] != (int32_t) fWidth || src[3] != (int32_t) fHeight ? "a part of it" :
	    dst[0] != 0 || dst[1] != 0 || dst[2] != (int32_t) fWidth || dst[3] != (int32_t) fHeight ? "not the whole screen" : NULL;

	if (why != NULL) {
		static const char *said[8];
		unsigned i = 0;

		/* each reason once, with the surface it was found on */
		while (i < 8 && said[i] != NULL && said[i] != why) {
			i++;
		}
		if (i < 8 && said[i] == NULL) {
			said[i] = why;
			fbSay("fb_scaler: surface %u (%ux%u '%.4s', %u bytes a row, at %#llx) by the processor: %s\n", s->sid,
			    s->info.width, s->info.height, (const char *) &s->info.pixelFormat, s->info.bytesPerRow, s->phys, why);
		}
		return false;
	}
	return true;
}

/*
 * The frame asked for last time has been taken (DECON latches at the start of a frame): the surface
 * before it is the panel's no longer and goes back to its maker. false: the scaler was turned off.
 */
bool
IOMobileFramebuffer::scalerWait(void)
{
	uint32_t mask = DECON_SHADOW_GLOBAL | (1u << fWin), irq, dppIrq, cfg, inUse, pathInUse;
	uint64_t began = mach_absolute_time(), limit;
	bool taken;

	if (!fScanPending) {
		return true;
	}
	(void) limit;
	latchWait(mask, fScanStart);
	fStatScanWait += mach_absolute_time() - began;
	fScanPending = false;
	taken = (dpuRead(fDecon, DECON_SHADOW_UPDATE) & mask) == 0;
	if (taken) {
		fLatchTime = mach_absolute_time();
	}
	irq = dpuRead(fDma5, IDMA_IRQ_REG);
	dppIrq = dpuRead(fDpp5, DPP_IRQ_REG);
	cfg = dpuRead(fDpp5, DPP_CFG_ERR_STATE_REG);
	inUse = dpuRead(fDma5, IDMA_BASE_Y8_REG + IDMA_SHADOW);
	pathInUse = dpuRead(fDecon, DECON_SHADOW_REGS + DECON_DATA_PATH_1);
	if (fStatScan <= 3 || !taken || (irq & IDMA_IRQ_ERRORS) || (dppIrq & DPP_IRQ_CONFIG_ERROR) || (cfg & 0x1f)) {
		uint64_t since;

		absolutetime_to_nanoseconds(mach_absolute_time() - fScanStart, &since);
		fbSay("fb_scaler: frame %u %s after %llu us: IDMA irq %#x, base in use %#x (asked %#x); DPP irq %#x, config errors %#x; "
		    "channels in use %#x; DECON global %#x\n", fStatScan, taken ? "taken" : "NOT taken", since / 1000, irq, inUse,
		    fScanAsked, dppIrq, cfg, pathInUse, dpuRead(fDecon, DECON_GLOBAL_CONTROL));
	}
	if ((irq & IDMA_IRQ_ERRORS) || (dppIrq & DPP_IRQ_CONFIG_ERROR) || (cfg & 0x1f)) {
		char why[120];

		snprintf(why, sizeof(why), "channel %u reports an error (IDMA irq %#x, DPP irq %#x, config errors %#x)", SCALER_CH, irq,
		    dppIrq, cfg);
		scalerOff(why, true);
		return false;
	}
	if (!taken) {
		if (++fStatScanNotTaken >= 3) {
			scalerOff("the switch is not taken", true);
			return false;
		}
		return true;
	}
	if (fScanShown != NULL) {
		MiniIOSurfaceDisplayUse(fScanShown, -1);
		MiniIOSurfaceDrop(fScanShown);
	}
	fScanShown = fScanNext;
	fScanNext = NULL;
	fShownSid = fScanShown != NULL ? fScanShown->sid : 0;
	gFbShownSid = fShownSid;
	fNextSid = 0;
	return true;
}

/* A frame to the panel through the scaler: the channel's base set to the surface, latched with the next frame. */
void
IOMobileFramebuffer::scalerShow(MiniIOSurface * s)
{
	uint32_t base = (uint32_t) (s->phys + s->info.offset);

	if (!scalerWait()) {
		return;
	}
	dpuWrite(fDma5, IDMA_BASE_Y8_REG, base);
	dpuWrite(fDma5, IDMA_SRC_SIZE_REG, (fHeight << 16) | (s->info.bytesPerRow / 4));       /* a row: in pixels */
	if (!fScalerOn) {
		uint32_t paths1 = dpuRead(fDecon, DECON_DATA_PATH_1);

		/* in nobody's path yet: its own registers taken at once; the window's channel with the frame */
		dpuWrite(fDma5, IDMA_ENABLE_REG, dpuRead(fDma5, IDMA_ENABLE_REG) | IDMA_SFR_UPDATE_FORCE);
		dpuWrite(fDecon, DECON_DATA_PATH_1, (paths1 & ~(7u << (4 * fWin))) | ((uint32_t) SCALER_CH << (4 * fWin)));
		fbSay("fb_scaler: on: window %d reads channel %u from the next frame (surface %u at %#x, %u bytes a row, size %#x)\n",
		    fWin, SCALER_CH, s->sid, base, s->info.bytesPerRow, dpuRead(fDma5, IDMA_SRC_SIZE_REG));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | DECON_SHADOW_GLOBAL | (1u << fWin));
	if (fScanNext != NULL) {
		MiniIOSurfaceDisplayUse(fScanNext, -1);
		MiniIOSurfaceDrop(fScanNext);
	}
	/* held, and in use for its maker, until the frame after it is taken */
	fScanNext = MiniIOSurfaceCopy(s->sid);
	MiniIOSurfaceDisplayUse(fScanNext, 1);
	fNextSid = s->sid;
	fScanAsked = base;
	fScanStart = mach_absolute_time();
	fScanPending = true;
	fScalerOn = true;
	fStatScan++;
	/*
	 * The swap is over only once the panel reads the new frame: until the shadow update latches,
	 * the controller still reads the frame before it, and CoreAnimation -- told the swap is done --
	 * draws its next frame into exactly that surface (run 16: the wallpaper tore while swiping).
	 * Waiting here (at most a frame) is what makes the swap's completion mean "on the panel".
	 */
	(void) scalerWait();
}

/* Back to S-Boot's channel (its buffer already holds this frame); forever: not again this boot. */
void
IOMobileFramebuffer::scalerOff(const char * why, bool forever)
{
	if (forever && fScalerReady) {
		fScalerReady = false;
		strlcpy(fScalerWhy, why, sizeof(fScalerWhy));
		fbSay("fb_scaler: off for the rest of this boot: %s\n", why);
	}
	if (!fScalerOn) {
		return;
	}
	uint32_t paths1 = dpuRead(fDecon, DECON_DATA_PATH_1), mask = DECON_SHADOW_GLOBAL | (1u << fWin);
	uint64_t began = mach_absolute_time(), limit;

	dpuWrite(fDecon, DECON_DATA_PATH_1, (paths1 & ~(7u << (4 * fWin))) | (fScanCh << (4 * fWin)));
	__asm__ volatile ("dsb sy" ::: "memory");
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | mask);
	(void) limit;
	latchWait(mask, began);
	if (fScanShown != NULL) {
		MiniIOSurfaceDisplayUse(fScanShown, -1);
		MiniIOSurfaceDrop(fScanShown);
		fScanShown = NULL;
	}
	if (fScanNext != NULL) {
		MiniIOSurfaceDisplayUse(fScanNext, -1);
		MiniIOSurfaceDrop(fScanNext);
		fScanNext = NULL;
	}
	fScanPending = false;
	fScalerOn = false;
	if (fStatScanCpu < 4 || forever) {
		fbSay("fb_scaler: window %d back on channel %u (%s)\n", fWin, fScanCh, why);
	}
}

void
IOMobileFramebuffer::teMeasure(void)
{
	uint64_t start, now, limit, busySince = 0, lastTe = 0, busyTotal = 0, busyMax = 0, teToBusy = 0;
	uint32_t te0, fc0, te, samples = 0, idleSamples = 0, busyRuns = 0, teToBusyN = 0, lineMax = 0, lineChanges = 0, lastLine = 0;
	uint32_t maskedFrames, maskedBusy = 0, maskedTe;
	bool idle = true;

	te0 = te = dpuRead(fDecon, DECON_TE_COUNT);
	fc0 = dpuRead(fDecon, DECON_FRAME_COUNT);
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &limit);
	start = mach_absolute_time();
	do {
		uint32_t global = dpuRead(fDecon, DECON_GLOBAL_CONTROL), count = dpuRead(fDecon, DECON_TE_COUNT);
		uint32_t line = (dpuRead(fDecon, DECON_OUTFIFO_COUNTER0) >> 16) & 0x3fff;
		bool nowIdle = (global & DECON_GLOBAL_IDLE) != 0;

		now = mach_absolute_time();
		samples++;
		if (count != te) {
			te = count;
			lastTe = now;
		}
		if (line != lastLine) {
			lineChanges++;
			lastLine = line;
		}
		lineMax = line > lineMax ? line : lineMax;
		idleSamples += nowIdle;
		if (idle && !nowIdle) {
			busyRuns++;
			busySince = now;
			if (lastTe != 0) {
				teToBusy += now - lastTe;
				teToBusyN++;
			}
		} else if (!idle && nowIdle && busySince != 0) {
			busyTotal += now - busySince;
			busyMax = now - busySince > busyMax ? now - busySince : busyMax;
		}
		idle = nowIdle;
	} while (now - start < limit);
	fbSay("fb: te: in 100 ms %u TE pulses, %u frames counted; %u samples, idle in %u; %u frames seen sent, %llu us each "
	    "(longest %llu), starting %llu us after their TE; output line counter changed %u times (highest %u)\n",
	    te - te0, dpuRead(fDecon, DECON_FRAME_COUNT) - fc0, samples, idleSamples, busyRuns,
	    busyRuns ? micros(busyTotal) / busyRuns : 0, micros(busyMax), teToBusyN ? micros(teToBusy) / teToBusyN : 0,
	    lineChanges, lineMax);
	fTeNeverIdle = busyRuns == 0;

	/* the trigger masked: does that stop the frames? */
	dpuWrite(fDecon, DECON_TRIG_CONTROL, fTeTrig | DECON_TRIG_MASK);
	(void) dpuRead(fDecon, DECON_TRIG_CONTROL);
	delay(20000);           /* (a frame that had begun is over) */
	maskedTe = dpuRead(fDecon, DECON_TE_COUNT);
	maskedFrames = dpuRead(fDecon, DECON_FRAME_COUNT);
	nanoseconds_to_absolutetime(40 * NSEC_PER_MSEC, &limit);
	start = mach_absolute_time();
	idle = true;
	do {
		bool nowIdle = (dpuRead(fDecon, DECON_GLOBAL_CONTROL) & DECON_GLOBAL_IDLE) != 0;

		maskedBusy += idle && !nowIdle;
		idle = nowIdle;
	} while (mach_absolute_time() - start < limit);
	maskedTe = dpuRead(fDecon, DECON_TE_COUNT) - maskedTe;
	maskedFrames = dpuRead(fDecon, DECON_FRAME_COUNT) - maskedFrames;
	dpuWrite(fDecon, DECON_TRIG_CONTROL, fTeTrig);
	fbSay("fb: te: with the trigger masked for 40 ms: %u TE pulses, %u frames counted, %u seen sent\n", maskedTe, maskedFrames,
	    maskedBusy);
	{
		/* the frame-start flag: cleared, is it clear, and set again once a frame has gone? */
		uint32_t cleared, later;

		dpuWrite(fDecon, DECON_INT_PENDING, DECON_INT_FRAME_START);
		cleared = dpuRead(fDecon, DECON_INT_PENDING);
		delay(40000);
		later = dpuRead(fDecon, DECON_INT_PENDING);
		fTeStartFlag = (cleared & DECON_INT_FRAME_START) == 0 && (later & DECON_INT_FRAME_START) != 0;
		fbSay("fb: te: frame-start flag: %#x after clearing, %#x 40 ms later: %s\n", cleared, later,
		    fTeStartFlag ? "used to catch late frames" : "not usable");
	}
	if (maskedBusy != 0 || (busyRuns == 0 && maskedFrames != 0 && maskedFrames == maskedTe)) {
		fTeNeverIdle = true;    /* (masking does not hold the frames back: copying under it does not help) */
		fbSay("fb: te: masking the trigger does not stop the frames\n");
	}
}

/* The frame-start interrupt: fb_irq. */
void
IOMobileFramebuffer::frameIrqSetup(void)
{
	uint32_t enable;

	if (fDecon == NULL) {
		fDecon = (volatile uint8_t *) ml_io_map((vm_offset_t) DPU_DECON_F_PHYS, 0x10000);
	}
	if (fDecon == NULL) {
		fbSay("fb_irq: DECON_F not mapped: off\n");
		return;
	}
	enable = dpuRead(fDecon, DECON_INT_ENABLE);
	/* only frame start: frame done and the extra ones would want clearing of their own (decon_reg_set_int) */
	dpuWrite(fDecon, DECON_INT_ENABLE, (enable & ~(DECON_INT_FRAME_DONE | DECON_INT_EXTRA)) | DECON_INT_FRAME_START |
	    DECON_INT_EN_ALL);
	dpuWrite(fDecon, DECON_INT_PENDING, DECON_INT_FRAME_START | DECON_INT_FRAME_DONE | DECON_INT_EXTRA);
	if (gic_v3_spi_attach(DECON_INTID_FRAME_START, false, frameIrq, this) != KERN_SUCCESS) {
		dpuWrite(fDecon, DECON_INT_ENABLE, enable);
		fbSay("fb_irq: interrupt %u could not be attached: off\n", DECON_INTID_FRAME_START);
		return;
	}
	gic_v3_spi_mask(DECON_INTID_FRAME_START);
	fFrameIrq = true;
	fbSay("fb_irq: DECON frame start on interrupt %u (INTERRUPT_ENABLE %#x -> %#x): the waits for a frame to be taken sleep "
	    "until it\n", DECON_INTID_FRAME_START, enable, dpuRead(fDecon, DECON_INT_ENABLE));
}

/* (interrupt context) a frame has started: its flag cleared, the waiter woken, quiet until the next wait */
void
IOMobileFramebuffer::frameIrq(void * context, uint32_t intid)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) context;
	uint32_t pending = dpuRead(fb->fDecon, DECON_INT_PENDING);

	dpuWrite(fb->fDecon, DECON_INT_PENDING, pending & (DECON_INT_FRAME_START | DECON_INT_FRAME_DONE | DECON_INT_EXTRA));
	gic_v3_spi_mask(intid);
	if (pending & DECON_INT_FRAME_START) {
		OSIncrementAtomic((volatile SInt32 *) &fb->fFrameStarts);
	}
	thread_wakeup((event_t) (uintptr_t) &fb->fFrameStarts);
}

/*
 * Until the shadow bits of `mask` are clear (DECON has taken what was asked at `start`) or FLIP_LATCH_MS have
 * gone by. false: they did not clear.
 */
bool
IOMobileFramebuffer::latchWait(uint32_t mask, uint64_t start)
{
	uint64_t limit, cap;

	nanoseconds_to_absolutetime((uint64_t) FLIP_LATCH_MS * NSEC_PER_MSEC, &limit);
	nanoseconds_to_absolutetime((uint64_t) LATCH_CAP_MS * NSEC_PER_MSEC, &cap);
	while ((dpuRead(fDecon, DECON_SHADOW_UPDATE) & mask) != 0) {
		uint64_t now = mach_absolute_time();

		if (now - start >= limit) {
			fStatLatchLimit++;
			return false;
		}
		if (!fFrameIrq) {
			delay(100);
			continue;
		}
		uint64_t deadline = now + cap < start + limit ? now + cap : start + limit;
		uint32_t seen = fFrameStarts;

		dpuWrite(fDecon, DECON_INT_PENDING, DECON_INT_FRAME_START);   /* (no stale one) */
		assert_wait_deadline((event_t) (uintptr_t) &fFrameStarts, THREAD_UNINT, deadline);
		gic_v3_spi_unmask(DECON_INTID_FRAME_START);
		if ((dpuRead(fDecon, DECON_SHADOW_UPDATE) & mask) == 0) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			gic_v3_spi_mask(DECON_INTID_FRAME_START);
			break;
		}
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT) {
			gic_v3_spi_mask(DECON_INTID_FRAME_START);
			fStatLatchCap++;
		} else if (fFrameStarts != seen) {
			fStatLatchIrq++;
		}
	}
	return true;
}

/* The frame-start flag for tePresent: a mark now, then whether a frame has started since. */
uint32_t
IOMobileFramebuffer::frameStartMark(void)
{
	/* (the presenter's thread, as latchWait: the interrupt is masked now, the flag is ours) */
	dpuWrite(fDecon, DECON_INT_PENDING, DECON_INT_FRAME_START);
	return fFrameStarts;
}

bool
IOMobileFramebuffer::frameStartSince(uint32_t mark)
{
	return fFrameStarts != mark ||
	       (dpuRead(fDecon, DECON_INT_PENDING) & DECON_INT_FRAME_START) != 0;
}

void
IOMobileFramebuffer::teSetup(void)
{
	uint32_t global, trig;

	if (fDecon == NULL) {
		fDecon = (volatile uint8_t *) ml_io_map((vm_offset_t) DPU_DECON_F_PHYS, 0x10000);
	}
	if (fDecon == NULL) {
		fbSay("fb: te: DECON_F not mapped: fb_te off\n");
		return;
	}
	global = dpuRead(fDecon, DECON_GLOBAL_CONTROL);
	trig = dpuRead(fDecon, DECON_TRIG_CONTROL);
	fbSay("fb: te: DECON global %#x trigger %#x (timer %#x, secure %#x), TE count %u, frames %u, output %#x, windows secure %#x, "
	    "interrupts %#x pending %#x\n", global, trig, dpuRead(fDecon, DECON_TRIG_TIMER), dpuRead(fDecon, DECON_TRIG_SECURE),
	    dpuRead(fDecon, DECON_TE_COUNT), dpuRead(fDecon, DECON_FRAME_COUNT), dpuRead(fDecon, DECON_OUTFIFO_COUNTER0),
	    dpuRead(fDecon, DECON_SECURE_CONTROL), dpuRead(fDecon, DECON_INT_ENABLE), dpuRead(fDecon, DECON_INT_PENDING));
	if (!(global & DECON_GLOBAL_CMD_MODE) || !(trig & DECON_TRIG_HW_EN) || (trig & DECON_TRIG_MASK)) {
		fbSay("fb: te: not a command-mode panel on an unmasked hardware trigger: fb_te off\n");
		return;
	}
	fTeTrig = trig;
	teMeasure();
	fBackSize = (size_t) fRowBytes * fPanelHeight;
	fBack = (uint8_t *) IOMallocData(fBackSize);
	if (fBack == NULL) {
		fbSay("fb: te: no memory for a back buffer of %zu bytes: fb_te off\n", fBackSize);
		return;
	}
	memcpy(fBack, fPanel, fBackSize);
	fBase = fBack;
	fTeCount0 = dpuRead(fDecon, DECON_TE_COUNT);
	fTeFrameCount0 = dpuRead(fDecon, DECON_FRAME_COUNT);
	fTe = true;
	fbSay("fb: te: on: frames drawn into a back buffer, copied to the panel's with the trigger masked%s\n",
	    fTeNeverIdle ? " (without waiting: IDLE never seen clear)" : ", once DECON is idle");
}

/* The rows a frame changed, from the back buffer to the panel's, while DECON is held from sending a frame. */
void
IOMobileFramebuffer::tePresent(int32_t y0, int32_t y1)
{
	uint64_t began = mach_absolute_time(), idleAt, limit, waited;
	uint32_t frames;
	RowJob job;

	y0 = y0 < 0 ? 0 : y0;
	y1 = y1 > (int32_t) fPanelHeight ? (int32_t) fPanelHeight : y1;
	if (y1 <= y0) {
		return;
	}
	dpuWrite(fDecon, DECON_TRIG_CONTROL, fTeTrig | DECON_TRIG_MASK);
	(void) dpuRead(fDecon, DECON_TRIG_CONTROL);
	nanoseconds_to_absolutetime(TE_IDLE_WAIT_MS * NSEC_PER_MSEC, &limit);
	for (unsigned attempt = 0;; attempt++) {
		if (!fTeNeverIdle) {
			while ((dpuRead(fDecon, DECON_GLOBAL_CONTROL) & DECON_GLOBAL_IDLE) == 0) {
				if (mach_absolute_time() - began > limit) {
					fTeTimeouts++;
					break;
				}
				delay(20);
			}
		}
		if (!fTeStartFlag) {
			delay(3);
			break;
		}
		/* a TE just before the mask may start a frame a moment later: idle, and no frame started since */
		uint32_t mark = frameStartMark();

		delay(TE_SETTLE_US);
		if (!frameStartSince(mark) || attempt == 2) {
			break;
		}
		fTeLate++;
	}
	idleAt = mach_absolute_time();
	frames = dpuRead(fDecon, DECON_FRAME_COUNT);
	uint32_t startsAtIdle = fFrameStarts;
	bzero(&job, sizeof(job));
	job.copy = true;
	job.ry0 = y0;
	job.ry1 = y1;
	runJob(&job);
	__asm__ volatile ("dsb sy" ::: "memory");
	fTeRaced += dpuRead(fDecon, DECON_FRAME_COUNT) != frames ||
	    (fTeStartFlag && frameStartSince(startsAtIdle));
	dpuWrite(fDecon, DECON_TRIG_CONTROL, fFramesMasked ? (fTeTrig | DECON_TRIG_MASK) : fTeTrig);   /* (display off: stays masked) */
	waited = idleAt - began;
	fTeFrames++;
	fTeRows += (uint64_t) (y1 - y0);
	fTeIdleWait += waited;
	fTeIdleWaitMax = waited > fTeIdleWaitMax ? waited : fTeIdleWaitMax;
	fTeCopy += mach_absolute_time() - idleAt;
}

/* fb_te, every five seconds with the other numbers (fLock held) */
void
IOMobileFramebuffer::teReport(uint64_t nanos)
{
	uint32_t te = dpuRead(fDecon, DECON_TE_COUNT), frames = dpuRead(fDecon, DECON_FRAME_COUNT);
	uint64_t ms = nanos / NSEC_PER_MSEC ? nanos / NSEC_PER_MSEC : 1;

	fbSay("fb: te: %u copies in %llu ms, %llu rows each; waited %llu us for DECON (longest %llu), copied in %llu us; %u raced a frame, "
	    "%u late frames waited out, %u waits timed out; frames sent %llu a second\n", fTeFrames, ms,
	    fTeFrames ? fTeRows / fTeFrames : 0, fTeFrames ? micros(fTeIdleWait) / fTeFrames : 0, micros(fTeIdleWaitMax),
	    fTeFrames ? micros(fTeCopy) / fTeFrames : 0, fTeRaced, fTeLate, fTeTimeouts, (uint64_t) (frames - fTeFrameCount0) * 1000 / ms);
	fTeCount0 = te;
	fTeFrameCount0 = frames;
	fTeFrames = fTeRaced = fTeTimeouts = fTeLate = 0;
	fTeRows = fTeIdleWait = fTeIdleWaitMax = fTeCopy = 0;
}

/* fb_bl: DSIM0 and what it is doing; the brightness is sent from a thread call (the requests come in bursts). */
void
IOMobileFramebuffer::blSetup(void)
{
	fDsim = (volatile uint8_t *) ml_io_map((vm_offset_t) DSIM0_PHYS, 0x1000);
	if (fDsim == NULL) {
		fbSay("fb: bl: DSIM0 not mapped\n");
		return;
	}
	fbSay("fb: bl: DSIM0 config %#x, command config %#x, FIFO %#x, link %#x, interrupts %#x%s\n", dpuRead(fDsim, DSIM_CONFIG),
	    dpuRead(fDsim, DSIM_CMD_CONFIG), dpuRead(fDsim, DSIM_FIFOCTRL), dpuRead(fDsim, DSIM_LINK_STATUS1),
	    dpuRead(fDsim, DSIM_INTSRC), gBL ? "" : "; brightness requests only logged (fb_bl=1 to send them)");
	fBLLock = IOLockAlloc();
	fDsimLock = IOLockAlloc();
	fDsimReads = true;
	if (gBL) {
		fBLCall = thread_call_allocate_with_options(&IOMobileFramebuffer::blWork, this, THREAD_CALL_PRIORITY_KERNEL,
		    THREAD_CALL_OPTIONS_ONCE);
		IOLockLock(fDsimLock);
		panelReport("as found");
		fAor0 = PANEL_AOR_BRIGHTEST;
		if (gFactoryAuto && factoryLoad(this)) gFactoryEnabled = true;
		IOLockUnlock(fDsimLock);
		fbSay("fb: bl: brightness by AOR from %#x (full) to %#x (darkest)\n", fAor0, PANEL_AOR_DARKEST);
	}
}

/* the panel's AOR (B1, two bytes), or the brightest of Samsung's table if it cannot be read. With fDsimLock held. */
uint32_t
IOMobileFramebuffer::aorRead(void)
{
	static const uint8_t keyOn[] = { 0xF0, 0x5A, 0x5A }, keyOff[] = { 0xF0, 0xA5, 0xA5 };
	uint8_t value[2];
	int got;

	(void) dsimWrite(keyOn, sizeof(keyOn));
	got = dsimRead(0xB1, value, 2);
	(void) dsimWrite(keyOff, sizeof(keyOff));
	return got == 2 ? (uint32_t) value[0] << 8 | value[1] : PANEL_AOR_BRIGHTEST;
}

/*
 * One DCS read: the largest packet the panel may answer with, then the read with the bus turned
 * around (BTA); the answer comes back in DSIM's RX FIFO (dsim_drv.c dsim_read_data). Bytes read, or
 * -1. With fDsimLock held.
 */
int
IOMobileFramebuffer::dsimRead(uint8_t command, uint8_t * out, uint32_t max)
{
	uint64_t began, limit;
	uint32_t word, got = 0;

	if (!fDsimReads) {
		return -1;
	}
	nanoseconds_to_absolutetime(30 * NSEC_PER_MSEC, &limit);
	while (!(dpuRead(fDsim, DSIM_FIFOCTRL) & DSIM_FIFO_RX_EMPTY)) {
		(void) dpuRead(fDsim, DSIM_RXFIFO);     /* (anything left from before) */
		if (++got > 64) {
			break;
		}
	}
	got = 0;
	dpuWrite(fDsim, DSIM_INTSRC, DSIM_INT_RX_DONE);
	dpuWrite(fDsim, DSIM_PKTHDR, DSI_MAX_RETURN_SIZE | (max & 0xff) << 8);
	dpuWrite(fDsim, DSIM_PKTHDR, DSIM_PKTHDR_BTA | DSI_DCS_READ | (uint32_t) command << 8);
	began = mach_absolute_time();
	while ((dpuRead(fDsim, DSIM_FIFOCTRL) & DSIM_FIFO_RX_EMPTY) && !(dpuRead(fDsim, DSIM_INTSRC) & DSIM_INT_RX_DONE)) {
		if (mach_absolute_time() - began > limit) {
			fDsimReads = false;
			fbSay("fb: bl: DCS read %#x: no answer in 30 ms (FIFO %#x, interrupts %#x, link %#x): no more reads\n", command,
			    dpuRead(fDsim, DSIM_FIFOCTRL), dpuRead(fDsim, DSIM_INTSRC), dpuRead(fDsim, DSIM_LINK_STATUS1));
			return -1;
		}
		delay(10);
	}
	delay(20);
	for (unsigned words = 0; words < 8 && !(dpuRead(fDsim, DSIM_FIFOCTRL) & DSIM_FIFO_RX_EMPTY); words++) {
		word = dpuRead(fDsim, DSIM_RXFIFO);
		switch (word & 0xff) {
		case DSI_RX_SHORT_1:
			out[got++] = (uint8_t) (word >> 8);
			break;
		case DSI_RX_SHORT_2:
			out[got++] = (uint8_t) (word >> 8);
			if (got < max) {
				out[got++] = (uint8_t) (word >> 16);
			}
			break;
		case DSI_RX_LONG: {
			uint32_t length = (word >> 8) & 0xffff;

			for (uint32_t i = 0; i < length; i += 4) {
				uint32_t data = dpuRead(fDsim, DSIM_RXFIFO);

				for (uint32_t b = 0; b < 4 && i + b < length; b++) {
					if (got < max) {
						out[got++] = (uint8_t) (data >> (8 * b));
					}
				}
			}
			break;
		}
		case DSI_RX_ERROR_REPORT:
			fbSay("fb: bl: DCS read %#x: the panel reports error %#x\n", command, (word >> 8) & 0xffff);
			break;
		default:
			break;
		}
	}
	return (int) got;
}

/* What the panel says of itself: power mode (0A), CTRL display (54: what 53 was set to, C0 = HBM), brightness (6A), ID (04). */
void
IOMobileFramebuffer::panelReport(const char * when)
{
	static const struct { uint8_t command, bytes; const char * name; } reads[] = {
		{ 0x0A, 1, "power mode" }, { 0x54, 1, "CTRL display" }, { 0x6A, 3, "DBV" }, { 0xB1, 2, "AOR" }, { 0x04, 3, "ID" },
		{ 0xCA, 34, "gamma" },
	};
	char line[512];
	size_t at = (size_t) snprintf(line, sizeof(line), "fb: bl: panel %s:", when);
	static const uint8_t keyOn[] = { 0xF0, 0x5A, 0x5A }, keyOff[] = { 0xF0, 0xA5, 0xA5 };

	(void) dsimWrite(keyOn, sizeof(keyOn));         /* (Samsung's own registers read 0 while locked) */
	for (unsigned i = 0; i < sizeof(reads) / sizeof(reads[0]) && at < sizeof(line); i++) {
		uint8_t value[40] = { 0 };
		int got = dsimRead(reads[i].command, value, reads[i].bytes);

		at += (size_t) snprintf(line + at, sizeof(line) - at, " %s %02x =", reads[i].name, reads[i].command);
		for (int b = 0; b < got && at < sizeof(line); b++) {
			at += (size_t) snprintf(line + at, sizeof(line) - at, " %02x", value[b]);
		}
		if (got <= 0 && at < sizeof(line)) {
			at += (size_t) snprintf(line + at, sizeof(line) - at, " (%s)", got < 0 ? "no answer" : "nothing");
		}
		if (got < 0) {
			break;
		}
	}
	(void) dsimWrite(keyOff, sizeof(keyOff));
	fbSay("%s\n", line);
}

/* One DCS write: up to two bytes a short packet, more a long one through the payload FIFO. */
bool
IOMobileFramebuffer::dsimWrite(const uint8_t * bytes, uint32_t length)
{
	uint64_t began = mach_absolute_time(), limit;
	uint32_t header;

	nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &limit);
	while (dpuRead(fDsim, DSIM_FIFOCTRL) & (DSIM_FIFO_PH_FULL | DSIM_FIFO_PL_FULL)) {
		if (mach_absolute_time() - began > limit) {
			return false;
		}
		delay(10);
	}
	if (fPacketGo) {
		dpuWrite(fDsim, DSIM_CMD_CONFIG, dpuRead(fDsim, DSIM_CMD_CONFIG) | DSIM_CMD_PKT_GO_EN);
	}
	if (length > 2) {
		for (uint32_t i = 0; i < length; i += 4) {
			uint32_t word = 0;

			for (uint32_t b = 0; b < 4 && i + b < length; b++) {
				word |= (uint32_t) bytes[i + b] << (8 * b);
			}
			dpuWrite(fDsim, DSIM_PAYLOAD, word);
		}
		header = DSI_DCS_LONG_WRITE | (length & 0xff) << 8 | (length >> 8 & 0xff) << 16;
	} else {
		header = (length == 2 ? DSI_DCS_SHORT_WRITE_1 : DSI_DCS_SHORT_WRITE) | (uint32_t) bytes[0] << 8 |
		    (uint32_t) (length == 2 ? bytes[1] : 0) << 16;
	}
	dpuWrite(fDsim, DSIM_PKTHDR, header);
	if (fPacketGo) {
		dpuWrite(fDsim, DSIM_CMD_CONFIG, dpuRead(fDsim, DSIM_CMD_CONFIG) & ~DSIM_CMD_PKT_GO_EN);
	}
	while (!(dpuRead(fDsim, DSIM_FIFOCTRL) & DSIM_FIFO_PH_EMPTY)) {
		if (mach_absolute_time() - began > limit) {
			return false;
		}
		delay(10);
	}
	return true;
}

bool
IOMobileFramebuffer::blSend(uint32_t dbv, uint32_t aor, const uint8_t *gamma, bool smooth)
{
	static const uint8_t keyOn[] = { 0xF0, 0x5A, 0x5A }, keyOff[] = { 0xF0, 0xA5, 0xA5 }, update[] = { 0xF7, 0x0F };
	const uint8_t hbmOff[] = { 0x53, (uint8_t) (gDim && smooth ? 0x08 : 0x00) };     /* HBM off; DD (smooth dimming) with fb_dim */
	const uint8_t level[] = { 0x6A, (uint8_t) (dbv >> 8 & 0x07), (uint8_t) (dbv & 0xff), 0x01 };
	const uint8_t offRatio[] = { 0xB1, (uint8_t) (aor >> 8), (uint8_t) aor };
	bool ok;

	ok = dsimWrite(keyOn, sizeof(keyOn));
	if (gamma != NULL) {
		uint8_t command[35] = { 0xCA };
		memcpy(command + 1, gamma, 34);
		ok = ok && dsimWrite(command, sizeof(command));
	}
	ok = ok && dsimWrite(offRatio, sizeof(offRatio));
	ok = ok && dsimWrite(level, sizeof(level));
	ok = ok && dsimWrite(hbmOff, sizeof(hbmOff));
	ok = ok && dsimWrite(update, sizeof(update));
	return dsimWrite(keyOff, sizeof(keyOff)) && ok;
}

/* Software fades complete before DCS 28; wake cancels pending off under the same lock. */
void
IOMobileFramebuffer::offWork(thread_call_param_t self, thread_call_param_t unused __unused)
{
	IOMobileFramebuffer *fb = (IOMobileFramebuffer *)self;
	const uint8_t command[] = { 0x28 };
	IOLockLock(fb->fDsimLock);
	if (!fb->fOffPending) { IOLockUnlock(fb->fDsimLock); return; }
	IOLockLock(fb->fBLLock);
	uint64_t end = fb->fFadeStart + fb->fFadeLen, grace;
	bool unfinished = fb->fFadeLen && fb->fBLTarget == 0 && fb->fBLCurrent != 0;
	IOLockUnlock(fb->fBLLock);
	if (fb->fAlphaLen != 0) {
		end = fb->fAlphaStart + fb->fAlphaLen;
		unfinished = true;
	}
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &grace);
	if (unfinished && mach_absolute_time() < end + grace) {
		uint64_t next;
		clock_interval_to_deadline(1000000u / refreshHz(), kMicrosecondScale, &next);
		IOLockUnlock(fb->fDsimLock);
		thread_call_enter_delayed(fb->fOffCall, next);
		return;
	}
	fb->fOffPending = false;
	bool ok = fb->dsimWrite(command, sizeof(command));
	if (ok) { fb->fDisplayOff = true; fb->offFrames(true); redwood_dvfs_display(0); }
	IOLockUnlock(fb->fDsimLock);
	fbSay("fb: bl: display off after fade (DCS 28) %s\n", ok ? "sent" : "NOT SENT");
}

void
IOMobileFramebuffer::displayPower(bool on)
{
	const uint8_t command[] = { (uint8_t)(on ? 0x29 : 0x28) };
	if (fDsim == NULL || !gBL) return;
	IOLockLock(fDsimLock);
	if (on && fOffPending) {
		fOffPending = false;
		if (fOffCall != NULL) thread_call_cancel(fOffCall);
		alphaFadeIn();                  /* (the picture had been fading out) */
		redwood_gov_boost(redwood_wake_boost_ms);
		IOLockUnlock(fDsimLock);
		fbSay("fb: bl: display off cancelled by wake\n");
		return;
	}
	if (fDisplayOff == !on || (!on && fOffPending)) { IOLockUnlock(fDsimLock); return; }
	if (on) { offFrames(false); alphaFadeIn(); redwood_dvfs_display(1); redwood_gov_boost(redwood_wake_boost_ms); }
	if (!on) {
		uint64_t at = 0, settle;
		IOLockLock(fBLLock);
		bool software = gFade && fFadeLen != 0 && fBLTarget == 0;
		if (software) at = fFadeStart + fFadeLen;
		IOLockUnlock(fBLLock);
		if (fAlphaLen != 0) {
			software = true;
			at = fAlphaStart + fAlphaLen;   /* the picture's fade: off once it is black */
		}
		if (software) {
			nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &settle);
			at += settle; /* one frame for the final zero write */
		} else if (gDim && gOffHold) {
			nanoseconds_to_absolutetime(300 * NSEC_PER_MSEC, &settle);
			at = fBLSentAt + settle;
		}
		if (at > mach_absolute_time()) {
			if (fOffCall == NULL) fOffCall = thread_call_allocate_with_options(&IOMobileFramebuffer::offWork,
			    this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
			if (fOffCall != NULL) {
				fOffPending = true;
				thread_call_enter_delayed(fOffCall, at);
				IOLockUnlock(fDsimLock);
				return;
			}
		}
	}
	bool ok = dsimWrite(command, sizeof(command));
	if (ok) { fDisplayOff = !on; if (!on) offFrames(true); redwood_dvfs_display(on); }
	IOLockUnlock(fDsimLock);
	fbSay("fb: bl: display %s (DCS %02x) %s\n", on ? "on" : "off", command[0], ok ? "sent" : "NOT SENT");
}

/* nits in 16.16 to the panel's DBV, between the steps of Samsung's table */
static uint32_t
nitsToDBV(uint32_t level)
{
	const unsigned steps = sizeof(gPanelNits) / sizeof(gPanelNits[0]);
	uint64_t milli = ((uint64_t) level * 1000) >> 16;

	if (level == 0) {
		return 0;
	}
	if (milli <= (uint64_t) gPanelNits[0] * 1000) {
		return gPanelDBV[0];
	}
	for (unsigned i = 0; i + 1 < steps; i++) {
		uint64_t lo = (uint64_t) gPanelNits[i] * 1000, hi = (uint64_t) gPanelNits[i + 1] * 1000;

		if (milli < hi) {
			return gPanelDBV[i] + (uint32_t) ((uint64_t) (gPanelDBV[i + 1] - gPanelDBV[i]) * (milli - lo) / (hi - lo));
		}
	}
	return gPanelDBV[steps - 1];
}

/* the integer cube root (floor) */
static uint64_t
icbrt64(uint64_t v)
{
	uint64_t lo = 0, hi = 2097152;          /* 2^21: its cube is 2^63 */

	while (lo < hi) {
		uint64_t mid = (lo + hi + 1) / 2;

		if (mid * mid * mid <= v) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	return lo;
}

void
IOMobileFramebuffer::blWork(thread_call_param_t self, thread_call_param_t unused __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;

	for (;;) {
		uint32_t target, dbv, aor, back;
		uint8_t gamma[34];
		const uint8_t *gammaToSend = NULL;
		uint64_t began, nits;
		bool ok;

		uint64_t fstart, flen, now = mach_absolute_time();
		uint32_t from;
		bool more = false;

		IOLockLock(fb->fBLLock);
		target = aodLevel(fb->fBLTarget);
		from = fb->fFadeFrom;
		fstart = fb->fFadeStart;
		flen = fb->fFadeLen;
		IOLockUnlock(fb->fBLLock);
		if (flen != 0 && now - fstart < flen) {
			/*
			 * the fade: even to the eye -- linear in the cube root of nits (CIE lightness L*), so the dim end gets its
			 * share of the steps. In the square root the last steps were 11 -> 4.6 -> 0.16 nits -> off: a snap to black,
			 * plainest over the lock screen's bright wallpaper.
			 */
			uint64_t sf = icbrt64((uint64_t) from << 24), st = icbrt64((uint64_t) target << 24), sv;
			uint64_t done = now - fstart;

			sv = st >= sf ? sf + (st - sf) * done / flen : sf - (sf - st) * done / flen;
			target = (uint32_t) ((sv * sv * sv) >> 24);
			more = true;
		}
		if (gBLThermalCap != 0 && target > gBLThermalCap) {
			target = gBLThermalCap;
		}
		IOLockLock(fb->fDsimLock);
		dbv = nitsToDBV(target);
		nits = (uint64_t) target < ((uint64_t) kPanelMaxNits << 16) ? target : (uint64_t) kPanelMaxNits << 16;
		aor = fb->fAor0 < PANEL_AOR_DARKEST ?
		    PANEL_AOR_DARKEST - (uint32_t) ((uint64_t) (PANEL_AOR_DARKEST - fb->fAor0) * nits / ((uint64_t) kPanelMaxNits << 16)) :
		    fb->fAor0;
		if (gFactoryEnabled) {
			factoryParameters(target, &aor, gamma);
			gammaToSend = gamma;
		} else if (gFactoryRestore) {
			gammaToSend = gFactorySavedGamma;
		}
		if (fb->fBLValid && aor == fb->fBLSent && dbv == fb->fBLDBVSent &&
		    (!gFactoryEnabled || fb->fBLFactoryLevel == target)) {
			IOLockUnlock(fb->fDsimLock);
			if (more) {
				uint64_t next;

				clock_interval_to_deadline(1000000u / refreshHz(), kMicrosecondScale, &next);
				thread_call_enter_delayed(fb->fBLCall, next);
			}
			return;
		}
		began = mach_absolute_time();
		ok = fb->blSend(dbv, aor, gammaToSend, flen == 0 || gFadeDD);
		fb->fBLValid = ok;
		if (ok) {
			fb->fBLSent = aor;
			fb->fBLDBVSent = dbv;
			fb->fBLFactoryLevel = target;
			gFactoryRestore = false;
			IOLockLock(fb->fBLLock);
			fb->fBLCurrent = target;
			IOLockUnlock(fb->fBLLock);
			fb->fBLSentAt = mach_absolute_time();
			fb->fBLFailStreak = 0;
		} else {
			fb->fBLFailStreak++;
		}
		fb->fBLWrites++;
		fb->fBLFailures += !ok;
		if (fb->fBLWrites <= 2) {
			char when[32];

			snprintf(when, sizeof(when), "after write %u", fb->fBLWrites);
			fb->panelReport(when);
		}
		back = fb->fBLWrites <= 2 ? fb->aorRead() : aor;
		IOLockUnlock(fb->fDsimLock);
		if (fb->fBLWrites <= 40 || !ok || fb->fBLWrites % 100 == 0) {
			fbSay("fb: bl: %u.%03u nits -> AOR %#x (read back %#x%s), DBV %#x %s in %llu us (write %u, %u failed)\n", target >> 16,
			    (uint32_t) (((uint64_t) (target & 0xffff) * 1000) >> 16), aor, back, fb->fPacketGo ? ", PKT_GO" : "", dbv,
			    ok ? "sent" : "NOT SENT", micros(mach_absolute_time() - began), fb->fBLWrites, fb->fBLFailures);
		}
		if ((ok && more) || (!ok && fb->fBLFailStreak <= 3)) {
			/* A fade advances on a frame; failed packets get at most three delayed retries. */
			uint64_t next;

			clock_interval_to_deadline(ok ? 1000000u / refreshHz() : 50000u, kMicrosecondScale, &next);
			thread_call_enter_delayed(fb->fBLCall, next);
			return;
		}
		if (!ok) {
			return; /* next explicit request may retry; do not spin on a broken link */
		}
	}
}

/* CoreBrightness: IORegistryEntrySetCFProperty(the framebuffer, "IOMFBBrightnessLevel", nits in 16.16) */
IOReturn
IOMobileFramebuffer::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSNumber * level;

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	level = OSDynamicCast(OSNumber, dict->getObject("IOMFBBrightnessLevel"));
	fBLRequests++;
	if (fBLRequests <= 12 || (level == NULL && fBLRequests <= 64)) {
		OSCollectionIterator * keys = OSCollectionIterator::withCollection(dict);
		char names[256] = "";

		while (keys != NULL) {
			OSSymbol * key = OSDynamicCast(OSSymbol, keys->getNextObject());

			if (key == NULL) {
				break;
			}
			strlcat(names, " ", sizeof(names));
			strlcat(names, key->getCStringNoCopy(), sizeof(names));
		}
		OSSafeReleaseNULL(keys);
		fbSay("fb: bl: setProperties %u:%s%s%u\n", fBLRequests, names, level ? " = " : "", level ? level->unsigned32BitValue() : 0);
	}
	if (level == NULL) {
		return kIOReturnUnsupported;
	}
	uint32_t native = level->unsigned32BitValue();
	brightnessRequest(native);
	return kIOReturnSuccess;
}

/* a brightness, in nits 16.16, for the panel: sent from the thread call (requests come in bursts: the last one wins) */
void
IOMobileFramebuffer::brightnessRequest(uint32_t level, uint32_t fade16)
{
	uint64_t len = 0;

	if (fBLCall == NULL || fDsim == NULL) {
		return;
	}
	/* Same order as offWork/blWork: DSIM, then brightness. Alpha state
	 * and register restoration must not race a wake or an old fade callback. */
	IOLockLock(fDsimLock);
	if (gFade && fade16 != 0 && fBLWrites != 0) {
		uint64_t ns = (uint64_t) fade16 * NSEC_PER_SEC / 65536;

		if (level == 0 && gOffFadeMs != 0 && ns < (uint64_t) gOffFadeMs * NSEC_PER_MSEC) {
			ns = (uint64_t) gOffFadeMs * NSEC_PER_MSEC;     /* (fb.fade: the power-off fade stretched) */
		}

		nanoseconds_to_absolutetime(ns > NSEC_PER_SEC ? NSEC_PER_SEC : ns, &len);
	}
	if (gOffAlpha && level == 0 && len != 0 && fDecon != NULL && fWin >= 0 && fWin <= 5 && !fDisplayOff) {
		if (fAlphaCall == NULL) {
			fAlphaCall = thread_call_allocate_with_options(&IOMobileFramebuffer::alphaWork, this,
			    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
		}
		if (fAlphaCall != NULL) {
			fAlphaUp = false;
			fAlphaStart = mach_absolute_time();
			fAlphaLen = len;
			thread_call_enter(fAlphaCall);
			IOLockUnlock(fDsimLock);
			return;                         /* the backlight stays where it is */
		}
	}
	if (level != 0 && fAlphaOn && !fAlphaUp) {
		fAlphaLen = 0;
		alphaRestore();
	}
	IOLockLock(fBLLock);
	fFadeFrom = fBLCurrent;
	fFadeStart = mach_absolute_time();
	fFadeLen = len;
	fBLTarget = level;
	IOLockUnlock(fBLLock);
	thread_call_enter(fBLCall);
	IOLockUnlock(fDsimLock);
}

/* the window's plane alpha (0 black .. 255 the picture as it is), taken at the next frame */
void
IOMobileFramebuffer::alphaSet(uint32_t alpha)
{
	uint32_t c0 = 0x1000 + 0x30 * (uint32_t) fWin, c1 = 0x1004 + 0x30 * (uint32_t) fWin;

	if (!fAlphaOn) {
		fWinCtl0Saved = dpuRead(fDecon, c0);
		fWinCtl1Saved = dpuRead(fDecon, c1);
		fAlphaOn = true;
		/* FG colour x plane alpha0, BG x 0; alpha likewise; user-defined function (0xd), alpha-mult from alpha0 */
		dpuWrite(fDecon, c1, (6u << 24) | (0u << 16) | (6u << 8) | 0u);
		dpuWrite(fDecon, c0, (fWinCtl0Saved & ~((0xFu << 8) | 0x3u)) | (0xDu << 8) | 0u);
	}
	dpuWrite(fDecon, c0, (dpuRead(fDecon, c0) & ~(0xFFu << 16)) | ((alpha & 0xFFu) << 16));
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | (1u << fWin));
}

/* (B, 2026-11-07) display off: DECON's trigger masked, so no frame goes out at 60/120 Hz into a dark panel (it kept
 * 12 frames per 100 ms going at 120 Hz with the display off, and ACPM then refused the screen-off memory clock) */
void
IOMobileFramebuffer::offFrames(bool off)
{
	if (fDecon == NULL || !gOffMask || off == fFramesMasked) {
		return;
	}
	uint32_t trig = dpuRead(fDecon, DECON_TRIG_CONTROL);

	dpuWrite(fDecon, DECON_TRIG_CONTROL, off ? (trig | DECON_TRIG_MASK) : (trig & ~DECON_TRIG_MASK));
	fFramesMasked = off;
}

/* power on: the window starts black and comes up over gOnFadeMs (fDsimLock held) */
void
IOMobileFramebuffer::alphaFadeIn(void)
{
	if (!gOffAlpha || gOnFadeMs == 0 || fDecon == NULL || fWin < 0 || fWin > 5) {
		if (fAlphaOn) {
			fAlphaLen = 0;
			alphaRestore();
		}
		return;
	}
	if (fAlphaCall == NULL) {
		fAlphaCall = thread_call_allocate_with_options(&IOMobileFramebuffer::alphaWork, this,
		    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	}
	if (fAlphaCall == NULL) {
		alphaRestore();
		return;
	}
	alphaSet(0);
	fAlphaUp = true;
	fAlphaStart = mach_absolute_time();
	nanoseconds_to_absolutetime((uint64_t) gOnFadeMs * NSEC_PER_MSEC, &fAlphaLen);
	thread_call_enter(fAlphaCall);
}

void
IOMobileFramebuffer::alphaRestore(void)
{
	if (!fAlphaOn || fDecon == NULL) {
		return;
	}
	dpuWrite(fDecon, 0x1004 + 0x30 * (uint32_t) fWin, fWinCtl1Saved);
	dpuWrite(fDecon, 0x1000 + 0x30 * (uint32_t) fWin, fWinCtl0Saved);
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | (1u << fWin));
	fAlphaOn = false;
}

/* one step of the picture's fade a frame: linear in the encoded value (about even to the eye), then held at 0 */
void
IOMobileFramebuffer::alphaWork(thread_call_param_t self, thread_call_param_t unused __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	/* A pending callback belongs to no particular fade. Read direction, time
	 * and length under the same lock as wake/brightness, then apply only that
	 * current fade. An old fade-out must never paint black after a new wake. */
	IOLockLock(fb->fDsimLock);
	uint64_t len = fb->fAlphaLen;
	if (len == 0) { IOLockUnlock(fb->fDsimLock); return; }
	uint64_t now = mach_absolute_time();
	uint64_t done = now >= fb->fAlphaStart ? now - fb->fAlphaStart : 0;
	uint32_t alpha;
	bool more = done < len;
	if (fb->fAlphaUp) {
		alpha = more ? (uint32_t)(255 * done / len) : 255;
		if (!more) {
			fb->fAlphaLen = 0;
			fb->fAlphaUp = false;
			fb->alphaRestore();
		} else fb->alphaSet(alpha);
	} else {
		alpha = more ? (uint32_t)(255 - 255 * done / len) : 0;
		fb->alphaSet(alpha);
		/* Retain the finished fade-out until offWork sends DCS28 or wake
		 * restores it. No further alpha callback is needed at zero. */
	}
	if (more) {
		uint64_t next;
		clock_interval_to_deadline(1000000u / refreshHz(), kMicrosecondScale, &next);
		thread_call_enter_delayed(fb->fAlphaCall, next);
	}
	IOLockUnlock(fb->fDsimLock);
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->ry1 - job->ry0;
		y0 = job->ry0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->ry0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst, bool damage)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.copy = false;
	job.compare = false;
	job.changed = NULL;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	job.ry0 = py0;
	job.ry1 = py1;
	if (damage && gDamage && !fDiff) {
		uint64_t compared = mach_absolute_time();
		bool changed = damageRows(&job, &job.ry0, &job.ry1);

		fStatCompare += mach_absolute_time() - compared;
		if (!changed) {
			fStatSkipped++;
			fStatRowsFull += (uint64_t) (py1 - py0);
			return;
		}
	} else {
		fShadowValid = false;
	}
	if (fFlip) {
		/* this buffer was last drawn two frames ago: what the frame in between changed is missing here too */
		int32_t own0 = job.ry0, own1 = job.ry1;
		int32_t p0 = fPrevRy0 > py0 ? fPrevRy0 : py0, p1 = fPrevRy1 < py1 ? fPrevRy1 : py1;

		if (p1 > p0) {
			job.ry0 = p0 < job.ry0 ? p0 : job.ry0;
			job.ry1 = p1 > job.ry1 ? p1 : job.ry1;
		}
		fOwnRy0 = own0 < fOwnRy0 ? own0 : fOwnRy0;
		fOwnRy1 = own1 > fOwnRy1 ? own1 : fOwnRy1;
		fFrameDrew = true;
	} else if (fTe) {
		fOwnRy0 = job.ry0 < fOwnRy0 ? job.ry0 : fOwnRy0;
		fOwnRy1 = job.ry1 > fOwnRy1 ? job.ry1 : fOwnRy1;
		fFrameDrew = true;
	}
	fStatRows += (uint64_t) (job.ry1 - job.ry0);
	fStatRowsFull += (uint64_t) (py1 - py0);
	{
		uint64_t scaled = mach_absolute_time();

		runJob(&job);
		fStatScale += mach_absolute_time() - scaled;
	}
}

/* A job's rows drawn: by this thread alone, or shared out (fb_threads) with this thread taking the first share. */
void
IOMobileFramebuffer::runJob(RowJob * job)
{
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads || job->ry1 - job->ry0 < 64) {
		drawRows(job, job->ry0, job->ry1, fBlendRow);
		return;
	}
	IOLockLock(fHelperLock);
	fHelperJob = job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawRows(job, job->ry0, job->ry0 + (int32_t) ((int64_t) (job->ry1 - job->ry0) / fThreads), fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * fb_damage: which panel rows of this layer need drawing. Compares each row of the picture with
 * the copy of what was last shown (fShadow), keeps the rows that changed, and works out the panel
 * rows that take anything from them: with filtering a panel row mixes two picture rows, the one
 * its sample falls in and the next. False: nothing changed, nothing to draw. Anything else about
 * the layer that changed (its rectangles, the filter) makes it a whole frame, as before.
 */
typedef uint64_t __attribute__((aligned(4), may_alias)) word64_t;
typedef uint32_t __attribute__((aligned(4), may_alias)) word32_t;

/*
 * Whether two rows of pixels differ: eight bytes at a time, four of those to a test. (The kernel's
 * memcmp() goes byte by byte -- 11 million steps for a frame, more than all the drawing it saves.)
 */
static bool
rowsDiffer(const uint8_t * a, const uint8_t * b, size_t bytes)
{
	const word64_t * x = (const word64_t *) a;
	const word64_t * y = (const word64_t *) b;
	size_t words = bytes / 8, i = 0;

	for (; i + 4 <= words; i += 4) {
		if (((x[i] ^ y[i]) | (x[i + 1] ^ y[i + 1]) | (x[i + 2] ^ y[i + 2]) | (x[i + 3] ^ y[i + 3])) != 0) {
			return true;
		}
	}
	for (; i < words; i++) {
		if (x[i] != y[i]) {
			return true;
		}
	}
	if (bytes & 4) {
		return *(const word32_t *) (a + bytes - 4) != *(const word32_t *) (b + bytes - 4);
	}
	return false;
}

bool
IOMobileFramebuffer::damageRows(const RowJob * job, int32_t * ry0, int32_t * ry1)
{
	int32_t key[10] = { job->sx, job->sy, job->sw, job->sh, job->px0, job->py0, job->px1, job->py1, job->filter, (int32_t) fPanelWidth };
	size_t rowBytes = (size_t) job->sw * 4, need = rowBytes * (size_t) job->sh;
	int32_t changedFirst = -1, changedLast = -1, first = -1, last = -1;

	if (need > fShadowSize) {
		if (fShadow != NULL) {
			IOFree(fShadow, fShadowSize);
		}
		fShadow = (uint8_t *) IOMalloc(need);
		fShadowSize = fShadow != NULL ? need : 0;
		fShadowValid = false;
	}
	if (fShadow == NULL) {
		return true;            /* no room for the copy: draw it all */
	}
	if (!fShadowValid || memcmp(key, fShadowKey, sizeof(key)) != 0) {
		for (int32_t r = 0; r < job->sh; r++) {
			memcpy(fShadow + (size_t) r * rowBytes, job->pixels + (size_t) (job->sy + r) * job->stride + (size_t) job->sx * 4, rowBytes);
		}
		memcpy(fShadowKey, key, sizeof(key));
		fShadowValid = true;
		return true;            /* the whole layer: ry0..ry1 as given */
	}
	{
		/* the rows compared by every one who draws, each a share of them (fb_threads) */
		int32_t changed[2] = { INT32_MAX, -1 };
		RowJob compare = *job;

		compare.compare = true;
		compare.copy = false;
		compare.ry0 = 0;
		compare.ry1 = job->sh;
		compare.changed = changed;
		runJob(&compare);
		if (changed[1] < 0) {
			return false;
		}
		changedFirst = changed[0];
		changedLast = changed[1];
	}
	if (changedFirst < 0) {
		return false;
	}
	/* the panel rows that take anything from picture rows changedFirst..changedLast */
	for (int32_t y = job->py0; y < job->py1; y++) {
		int32_t lo, hi;

		if (job->unscaled) {
			lo = hi = y - job->py0;
		} else if (job->filter) {
			int64_t at = sample_at(y - job->py0, job->sh, job->py1 - job->py0);

			lo = (int32_t) (at >> 8);
			hi = (at & 255) != 0 && lo + 1 < job->sh ? lo + 1 : lo;
		} else {
			lo = hi = (int32_t) ((int64_t) (y - job->py0) * job->sh / (job->py1 - job->py0));
		}
		if (hi >= changedFirst && lo <= changedLast) {
			first = first < 0 ? y : first;
			last = y;
		}
	}
	if (first < 0) {
		return false;
	}
	*ry0 = first;
	*ry1 = last + 1;
	return true;
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		fbSay("fb: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat || redwood_touch_lat_live) {
		redwood_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	{
		/* frames are coming: the governor's frame floor (dvfs_frame=1); woken at once when they start after a pause */
		uint64_t last = redwood_fb_swap_at, gap;

		nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &gap);
		redwood_fb_swap_at = now;
		if (last == 0 || now - last > gap) {
			thread_wakeup((event_t) &redwood_gov_wake);
		}
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;
	uint32_t sid0 = swapWord(swap, SWAP_SURFACE(0));

	if (gScaler && fScalerReady && sid0 != 0) {
		bool busy = sid0 == fShownSid || sid0 == fNextSid;

		for (unsigned k = 0; k < fSwapsWaiting; k++) {
			busy |= swapWord(fSwap[(fSwapFirst + k) % SWAP_QUEUE], SWAP_SURFACE(0)) == sid0;
		}
		fStatIntoShown += busy;
	}
	fQueuedSwapID = swapWord(swap, SWAP_ID);
	fStatSwapSame += memcmp(fPrevSwapLayers, swap + SWAP_SURFACE(0), sizeof(fPrevSwapLayers)) == 0;
	memcpy(fPrevSwapLayers, swap + SWAP_SURFACE(0), sizeof(fPrevSwapLayers));
	{
		unsigned i;

		for (i = 0; i < 8 && fStatSwapSids[i] != 0 && fStatSwapSids[i] != sid0; i++) {
		}
		if (i == 8) {
			fStatSwapSidsMore++;
		} else {
			fStatSwapSids[i] = sid0;
		}
	}
	fStatSwapPid = proc_selfpid();
	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	fSwapsWaiting++;
	/* in use from the moment it is queued, as Apple's driver counts it: a surface asked for is not free */
	if (gScaler && fScalerReady && fSwapSurfaces[slot][0] != NULL) {
		MiniIOSurfaceDisplayUse(fSwapSurfaces[slot][0], 1);
		fSwapQueuedUse[slot] = true;
	}
	uint32_t ticket = ++fSwapsQueued;
	IOLockWakeup(fSwapLock, fSwap, true);
	if (gScaler && gSwapWait && fScalerReady) {
		uint64_t until;

		clock_interval_to_deadline(100, kMillisecondScale, &until);
		while ((int32_t) (fSwapsDone - ticket) < 0) {
			if (IOLockSleepDeadline(fSwapLock, &fSwapsDone, until, THREAD_UNINT) == THREAD_TIMED_OUT) {
				fStatSwapWaitTimeouts++;
				break;
			}
		}
	}
	IOLockUnlock(fSwapLock);
}

/*
 * IOMobileFramebufferSwapWait: until swap `id` is on the panel (see gSwapWait), or it was never queued, or
 * nothing is left on its way to the panel (a swap the presenter gave up on), at most 100 ms.
 */
void
IOMobileFramebuffer::swapWait(uint32_t id)
{
	uint64_t began = mach_absolute_time(), until;
	bool blocked = false;

	clock_interval_to_deadline(100, kMillisecondScale, &until);
	IOLockLock(fSwapLock);
	fStatSwapWaits++;
	while ((int32_t) (fShownSwapID - id) < 0 && (int32_t) (fQueuedSwapID - id) >= 0 &&
	    (fSwapsWaiting != 0 || fSwapsDone != fSwapsQueued)) {
		blocked = true;
		if (IOLockSleepDeadline(fSwapLock, &fShownSwapID, until, THREAD_UNINT) == THREAD_TIMED_OUT) {
			fStatSwapWaitTimeouts2++;
			break;
		}
	}
	if (blocked) {
		fStatSwapWaitsBlocked++;
		fStatSwapWaitTime += mach_absolute_time() - began;
	}
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0 && fModeWant < 0 && fWarmWant < 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		if (fModeWant >= 0) {
			int mode = fModeWant;

			IOLockUnlock(fSwapLock);
			modeLive(mode);                 /* (between two frames: nothing of a frame is in flight) */
			IOLockLock(fSwapLock);
			fModeWant = -1;
			fModeDone = true;
			IOLockWakeup(fSwapLock, (void *) (uintptr_t) &fModeDone, false);
			IOLockUnlock(fSwapLock);
			continue;
		}
		if (fWarmWant >= 0) {
			uint64_t wanted = (uint64_t)fWarmWant;
			IOLockUnlock(fSwapLock);
			warmApply(&wanted, 1, fWarmReport, sizeof(fWarmReport));
			IOLockLock(fSwapLock);
			fWarmWant = -1;
			fWarmDone = true;
			IOLockWakeup(fSwapLock, &fWarmDone, false);
			IOLockUnlock(fSwapLock);
			continue;
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		bool queuedUse = fSwapQueuedUse[fSwapFirst];

		fSwapQueuedUse[fSwapFirst] = false;
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		if (queuedUse && surfaces[0] != NULL) {
			MiniIOSurfaceDisplayUse(surfaces[0], -1);       /* (the scaler holds its own while it shows it) */
		}
		IOLockLock(fSwapLock);
		fSwapsDone++;
		fShownSwapID = swapWord(swap, SWAP_ID);
		IOLockWakeup(fSwapLock, &fSwapsDone, false);
		IOLockWakeup(fSwapLock, &fShownSwapID, false);
		IOLockUnlock(fSwapLock);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	if (gOnTheBeat) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	if (fFlip) {
		flipWait();
	} else if (fFlipWanted && fGraphics) {
		flipEnable();
	}
	fOwnRy0 = INT32_MAX;
	fOwnRy1 = -1;
	fFrameDrew = false;
	/* its turn has come: the swap after it may be handed over */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	bool zeroCopy = false;

	if (gScaler && scalerFits(swap, surfaces)) {
		scalerShow(surfaces[0]);
		zeroCopy = fScalerOn;
	}
	if (zeroCopy) {
		if (!fGraphics) {
			fGraphics = true;
			PE_initialize_console(NULL, kPEGraphicsMode);
		}
		layers = 1;
		recorded = surfaces[0];
	} else if (fScalerOn) {
		fShadowValid = false;           /* the panel's own buffer is frames behind: all of it again, then back to it */
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS && !zeroCopy; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				fShadowValid = false;
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
				fOwnRy0 = 0;
				fOwnRy1 = (int32_t) fPanelHeight;
				fFrameDrew = true;
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			/* damage only where nothing is drawn over the bottom layer: above it would be drawn over */
			/* ... in this swap and in the one before (what was drawn over it then is still on the panel) */
			drawLayer(s, src, dst, layer == 0 && (fLayersShown & ~1u) == 0 && swapWord(swap, SWAP_SURFACE(1)) == 0 &&
			    swapWord(swap, SWAP_SURFACE(2)) == 0 && swapWord(swap, SWAP_SURFACE(3)) == 0);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	drawn = mach_absolute_time();
	if (fFlip && fFrameDrew) {
		flipNow();
	} else if (fTe && fFrameDrew) {
		tePresent(fOwnRy0, fOwnRy1);
		drawn = mach_absolute_time();
	}
	if (gScaler && !zeroCopy) {
		fStatScanCpu++;
		if (fScalerOn) {
			scalerOff("a frame the scaler cannot show", false);
		}
	}
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat || redwood_touch_lat_live) {
		redwood_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		fbSay("fb: %u frames in %llu ms (%llu a second), %llu us each in the driver (comparing %llu, scaling %llu); %llu%% of the rows drawn\n",
		    fStatFrames, nanos / NSEC_PER_MSEC, fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    micros(fStatCompare) / fStatFrames, micros(fStatScale) / fStatFrames, fStatRowsFull ? fStatRows * 100 / fStatRowsFull : 0);
		fStatCompare = fStatScale = 0;
		if (gScaler) {
			fbSay("fb_scaler: %u frames read by the controller itself, %u drawn by the processor; %llu us waited for the switch a "
			    "frame; %u not taken%s%s\n", fStatScan, fStatScanCpu, fStatScan ? micros(fStatScanWait) / fStatScan : 0,
			    fStatScanNotTaken, fScalerReady ? "" : "; off: ", fScalerReady ? "" : fScalerWhy);
			fbSay("fb_swap: SwapEnd %s; %u SwapWaits, %u of them blocked (%llu us each), %u timed out; %u swaps found their "
			    "surface still on the panel or queued (would tear)\n", gSwapWait ? "waits for the panel (fb_swapwait=1)" :
			    "returns once queued", fStatSwapWaits, fStatSwapWaitsBlocked,
			    fStatSwapWaitsBlocked ? micros(fStatSwapWaitTime) / fStatSwapWaitsBlocked : 0, fStatSwapWaitTimeouts2,
			    fStatIntoShown);
			fbSay("fb_vsync: refresh notifications asked for %u times so far; %u sent, %u stopped (no port or callback)\n",
			    gVSyncAsked, gVSyncSent, gVSyncStopped);
			fbSay("fb_swap: by pid %d; %u swaps the same as the one before (surfaces, rectangles, layers); the bottom "
			    "layer's surfaces %u %u %u %u %u %u %u %u (+%u more)\n", fStatSwapPid, fStatSwapSame, fStatSwapSids[0],
			    fStatSwapSids[1], fStatSwapSids[2], fStatSwapSids[3], fStatSwapSids[4], fStatSwapSids[5],
			    fStatSwapSids[6], fStatSwapSids[7], fStatSwapSidsMore);
			fStatSwapSame = fStatSwapSidsMore = 0;
			bzero(fStatSwapSids, sizeof(fStatSwapSids));
			fbSay("fb_irq: waits for the controller to take a frame: %u ended by its frame-start interrupt, %u looked again "
			    "after %u ms, %u gave up after %u ms%s\n", fStatLatchIrq, fStatLatchCap, LATCH_CAP_MS, fStatLatchLimit,
			    FLIP_LATCH_MS, fFrameIrq ? "" : " (fb_irq off: looked every 100 us)");
			fStatLatchIrq = fStatLatchCap = fStatLatchLimit = 0;
			fStatScan = fStatScanCpu = 0;
			fStatScanWait = 0;
			fStatSwapWaits = fStatSwapWaitsBlocked = fStatSwapWaitTimeouts2 = fStatIntoShown = 0;
			fStatSwapWaitTime = 0;
		}
		if (fTe) {
			teReport(nanos);
		}
		fStatSwapsLate = 0;
		if (gDamage && fStatRowsFull != 0) {
			IOLog("MiniIOMobileFramebuffer: fb_damage: %llu%% of the panel's rows drawn, %u frames had no change\n",
			    fStatRows * 100 / fStatRowsFull, fStatSkipped);
		}
		fStatRows = fStatRowsFull = 0;
		fStatSkipped = 0;
		if (gFlip && !fFlip) {
			IOLog("MiniIOMobileFramebuffer: fb_flip asked for and off: %s\n", fDpuWhy[0] ? fDpuWhy : "not started yet");
		}
		if (fDpuDump[0] && fStatSince != 0 && fSwapsShown < 2000) {
			IOLog("MiniIOMobileFramebuffer: dpu: %s\n", fDpuDump);
		}
		if (fFlip) {
			uint64_t flipWaited;

			absolutetime_to_nanoseconds(fStatFlipWait, &flipWaited);
			IOLog("MiniIOMobileFramebuffer: fb_flip: %u flips so far, %llu us waited for the switch before drawing (%u not taken)\n",
			    fStatFlips, fStatFlips ? flipWaited / NSEC_PER_USEC / (fStatFrames ? fStatFrames : 1) : 0, fStatFlipLate);
			fStatFlipWait = 0;
		}
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	/*
	 * In step with the panel when the controller reads the frames itself: a vsync just after a frame was
	 * taken. A swap waits for its frame to be taken (fb_swapwait); a vsync out of phase with that makes
	 * the window server miss every other one (run 18: 33-35 frames a second, 6-7.5 ms waited a frame).
	 */
	uint64_t latch = fOwner != NULL ? fOwner->fLatchTime : 0, slack;

	nanoseconds_to_absolutetime(300 * NSEC_PER_USEC, &slack);
	if (latch != 0 && now > latch && now - latch < 120 * period) {
		uint64_t next = latch + slack + ((now - latch) / period + 1) * period;

		if (next > now && next <= now + period + slack) {
			fVSyncTime = next;
		}
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
		OSIncrementAtomic(&gVSyncStopped);
	} else {
		OSIncrementAtomic(&gVSyncSent);
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
		if (gTouchLat || redwood_touch_lat_live) {
			/* the refresh it is for, and when it was sent: tools/frame_split.py's refresh -> swap */
			redwood_tl("tl: vs %llu due %llu sent %llu\n", fVSyncCount, micros(now), micros(mach_absolute_time()));
		}
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		/* into the loader's log (fbSay): the message buffer wraps before anybody reads the first calls */
		fbSay("fb: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* in: the swap; returns once it is on the panel */
		if (gTouchLat || redwood_touch_lat_live) {
			redwood_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		if (!gSwapWait && args->scalarInputCount > 0) {
			fOwner->swapWait((uint32_t) in[0]);
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
		/* Swap signalling/cancellation is unrelated to panel power. A swap
		 * identifier (including zero) must never become an on/off request. */
		return kIOReturnSuccess;
	case kFBMethodRequestPowerChange:               /* in: 0 off, 1 on */
		if (args->scalarInputCount >= 1) {
			fOwner->displayPower(in[0] != 0);
		}
		return kIOReturnSuccess;

	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync) {
			OSIncrementAtomic(&gVSyncAsked);
		}
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 *
 * With boot-arg metal=1 a user process may put those two properties here, which is the whole
 * of what keeps Metal from finding our driver; see setProperties below, and docs/metal.md 10.31
 * (10.14 for the measurement that made it a boot-arg).
 */

/* Metal's step 2 reads exactly these two, in this order, off this service (docs/metal.md 1). */
#define kAcceleratorPluginNameKey      "MetalPluginName"
#define kAcceleratorPluginClassNameKey "MetalPluginClassName"

static bool gAcceleratorTakesProperties;        /* boot-arg metal=1 */

/*
 * OpenGL ES for apps (with metal=1; gles=0 turns it off). An app's EAGLContext goes through
 * OpenGLES.framework/libGFXShared.dylib: gfxInitializeLibrary() (0x1d31762f0) reads, off each
 * IOAcceleratorES, "IOGLESBundleName", then "IOGLESMetalBundleName" (taken when the preference
 * com.apple.opengl UseMetal, or else the boolean "IOGLESDefaultUseMetal", says so), dlopens
 * /System/Library/Extensions/<name>.bundle/<name> and calls its gldInitializeLibrary. With none
 * of them here no GL driver loads, -[EAGLContext initWithAPI:] gives nil, and games built on
 * OpenGL ES die at their first gl call (Leo's Fortune: strstr(glGetString(GL_EXTENSIONS)) on
 * NULL). Apple's own GL ES on top of Metal, AppleMetalGLRenderer (in the shared cache, exports
 * gldInitializeLibrary/gldCreateDevice), is what an iPhone of this generation names here; it
 * draws through MTLCreateSystemDefaultDevice(), i.e. our RedwoodMetal.
 * Published together with MetalPluginName (and removed with it), so GL ES is never offered
 * before Metal is.
 */
#define kGLESBundleNameKey             "IOGLESBundleName"
#define kGLESMetalBundleNameKey        "IOGLESMetalBundleName"
#define kGLESDefaultUseMetalKey        "IOGLESDefaultUseMetal"
#define kGLESRenderer                  "AppleMetalGLRenderer"

static bool gAcceleratorOffersGLES;             /* metal=1, unless gles=0 */

/* The virtual HID service supplies events, but MobileGestalt's ALS capability
 * query uses IOPropertyMatch in the kernel registry (20F66: 0x19c547058).
 * Publish backing metadata only after SSP has detected the light sensor. This
 * is deliberately not an IOHIDEventService: it must not create duplicate events. */
extern "C" void
redwood_als_publish(void)
{
	uint32_t enabled = 1;
	PE_parse_boot_argn("sens_als", &enabled, sizeof(enabled));
	if (!enabled) return;
	IOService *service = new IOService;
	IOService *provider = IOService::getResourceService();
	if (!service) return;
	if (!service->init() || !service->attach(provider)) {
		service->release();
		return;
	}
	service->setName("RedwoodAmbientLightSensor");
	service->setProperty("PrimaryUsagePage", (unsigned long long)0xff00, 32);
	service->setProperty("PrimaryUsage", (unsigned long long)4, 32);
	service->setProperty("Built-In", kOSBooleanTrue);
	service->setProperty("VendorID", (unsigned long long)0x5ac, 32);
	service->setProperty("Product", "Redwood ambient light sensor");
	service->registerService();
	service->release();
	IOLog("sensors: detected ALS published in IORegistry\n");
}

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
#define RW_GRAPHICS_DECL(Type) \
    virtual OSPtr<OSObject> copyProperty(const Type *key) const APPLE_KEXT_OVERRIDE; \
    virtual OSPtr<OSObject> copyProperty(const Type *key, const IORegistryPlane *plane, IOOptionBits options) const APPLE_KEXT_OVERRIDE;
    RW_GRAPHICS_DECL(char)
    RW_GRAPHICS_DECL(OSString)
    RW_GRAPHICS_DECL(OSSymbol)
#undef RW_GRAPHICS_DECL
    virtual OSPtr<OSDictionary> dictionaryWithProperties(void) const APPLE_KEXT_OVERRIDE;
    virtual bool serializeProperties(OSSerialize *serialize) const APPLE_KEXT_OVERRIDE;

};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/* Keep incompatible core renderers on their established path. Filtering is
 * request-local: never remove properties from the shared live registry table,
 * never patch user-space IOKit entry points or recursively call another hook. */
static bool rw_graphics_legacy_client(void)
{
    char name[32] = {0};
    proc_selfname(name, sizeof(name));
    return rw_graphics_legacy_process(name);
}
static const char *rw_graphics_key(const char *key) { return key; }
static const char *rw_graphics_key(const OSString *key) { return key ? key->getCStringNoCopy() : nullptr; }
#define RW_GRAPHICS_COPY(Type) \
OSPtr<OSObject> IOAcceleratorES::copyProperty(const Type *key) const { \
    if (rw_graphics_legacy_client() && rw_graphics_discovery_key(rw_graphics_key(key))) return nullptr; \
    return IOService::copyProperty(key); \
} \
OSPtr<OSObject> IOAcceleratorES::copyProperty(const Type *key, const IORegistryPlane *plane, IOOptionBits options) const { \
    if (rw_graphics_legacy_client() && rw_graphics_discovery_key(rw_graphics_key(key))) return nullptr; \
    return IOService::copyProperty(key, plane, options); \
}
RW_GRAPHICS_COPY(char)
RW_GRAPHICS_COPY(OSString)
RW_GRAPHICS_COPY(OSSymbol)
#undef RW_GRAPHICS_COPY

OSPtr<OSDictionary> IOAcceleratorES::dictionaryWithProperties(void) const
{
    auto dict = IOService::dictionaryWithProperties();
    if (dict && rw_graphics_legacy_client()) {
        const char *const keys[] = {"MetalPluginName", "MetalPluginClassName",
            "IOGLESBundleName", "IOGLESMetalBundleName", "IOGLESDefaultUseMetal"};
        for (unsigned i=0; i<sizeof keys/sizeof *keys; i++) dict->removeObject(keys[i]);
    }
    return dict;
}
bool IOAcceleratorES::serializeProperties(OSSerialize *serialize) const
{
    if (!rw_graphics_legacy_client()) return IOService::serializeProperties(serialize);
    auto dict = dictionaryWithProperties();
    return dict && dict->serialize(serialize);
}


/*
 * Switch Metal's own device discovery on, from user space: IORegistryEntrySetCFProperties() with
 * MetalPluginName and MetalPluginClassName. Metal reads the first (getMetalPluginClassForService,
 * 0x181a39ba4), builds "/System/Library/Extensions/" + it + ".bundle", opens that with NSBundle
 * and takes the class the second names; with the first missing it returns nil before it even
 * reads the second, and that single `cbz` is where the chain breaks on this port today.
 *
 * This method is the only thing that was in the way. IORegistryEntry::setProperties is
 * "return kIOReturnUnsupported" (IORegistryEntry.cpp) and IOService does not override it; the
 * MAC check in front of the call has no policy to ask, because this kernel registers no iokit
 * hook (bsd/kern/kern_minimac.c); and this service carries no property whitelist.
 *
 * Why it is gated on a boot-arg, which is a measured risk and not caution in general
 * (docs/metal.md 10.14): the conditions Metal puts on a device object are asserts, not soft
 * refusals (10.2-10.3). A process that asks Metal and is handed a half-made device does not
 * degrade to another renderer -- it aborts ("Assertion failed: ([self
 * conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion _limits.maxColorAttachments
 * > 0"), and an aborting daemon is restarted by launchd, which is a loop. Published instead at
 * registerService() time, the properties would exist before launchd, backboardd and SpringBoard
 * are born, so each of the three would take the Metal road on its first call, with nobody left
 * to read the log. Behind a boot-arg, a machine that has them is a machine booted on purpose to
 * have them, and the first such boot is meant to be RC=rc.ios, where launchd is never born.
 *
 * An empty string removes a property rather than setting it. That is the way out of the loop
 * above without a reboot: the properties live only in the registry, so processes started after
 * they are gone go back to the software road.
 *
 * Everything else is refused, and refused whole, before anything is set: every process on the
 * machine can reach this method and the only thing it exists for is this one switch.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (!gAcceleratorTakesProperties) {
		static unsigned said;

		if (said++ < 4) {
			proc_selfname(who, sizeof(who));
			IOLog("IOAcceleratorES: %s asks to set properties on it; boot-arg metal=1 was not given\n", who);
		}
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);

		if (key == NULL || (!key->isEqualTo(kAcceleratorPluginNameKey) &&
		    !key->isEqualTo(kAcceleratorPluginClassNameKey))) {
			IOLog("IOAcceleratorES: refusing what %s sent: only " kAcceleratorPluginNameKey " and "
			    kAcceleratorPluginClassNameKey " are taken here\n", who);
			keys->release();
			return kIOReturnUnsupported;
		}
		if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			IOLog("IOAcceleratorES: refusing \"%s\" from %s: Metal reads it as a CFString\n",
			    key->getCStringNoCopy(), who);
			keys->release();
			return kIOReturnBadArgument;
		}
	}
	keys->reset();
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSString * value = OSDynamicCast(OSString, dict->getObject(key));

		if (value->getLength() == 0) {
			removeProperty(key);
			IOLog("IOAcceleratorES: %s removed by %s\n", key->getCStringNoCopy(), who);
			if (gAcceleratorOffersGLES && key->isEqualTo(kAcceleratorPluginNameKey)) {
				removeProperty(kGLESBundleNameKey);
				removeProperty(kGLESMetalBundleNameKey);
				removeProperty(kGLESDefaultUseMetalKey);
				IOLog("IOAcceleratorES: gles: OpenGL ES withdrawn with Metal\n");
			}
		} else if (setProperty(key, value)) {
			IOLog("IOAcceleratorES: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
			if (gAcceleratorOffersGLES && key->isEqualTo(kAcceleratorPluginNameKey)) {
				setProperty(kGLESBundleNameKey, kGLESRenderer);
				setProperty(kGLESMetalBundleNameKey, kGLESRenderer);
				setProperty(kGLESDefaultUseMetalKey, kOSBooleanTrue);
				IOLog("IOAcceleratorES: gles: " kGLESBundleNameKey " = " kGLESRenderer
				    " (OpenGL ES for apps, on Metal)\n");
			}
		} else {
			IOLog("IOAcceleratorES: %s could not be set\n", key->getCStringNoCopy());
			keys->release();
			return kIOReturnNoMemory;
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

/*
 * The display's backlight, as CoreBrightness (backboardd) looks for it: a service with
 * "backlight-control" = true that conforms to AppleARMBacklight (CBDisplayContaineriOS). Only for
 * such a service does it make its display module, which then sets the brightness -- as
 * "IOMFBBrightnessLevel" (nits, 16.16) on the framebuffer, and by IORegistryEntrySetCFProperties on
 * this service. It reads "IODisplayParameters" = { brightness = { min, max, value } } from here.
 * Everything it sets here is said (fb: bl: lines) and a brightness taken from it goes to the panel
 * like the framebuffer's: value / max of the panel's 420 nits.
 */

class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);

public:
	IOMobileFramebuffer *   fFB;
	uint32_t                fValue;
	uint32_t                fRequests;

	void publishParameters(void);
	bool takeBrightness(OSObject * value, const char * name);
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

void
AppleARMBacklight::publishParameters(void)
{
	OSDictionary * parameters = OSDictionary::withCapacity(1);
	OSDictionary * brightness = OSDictionary::withCapacity(3);

	if (parameters != NULL && brightness != NULL) {
		setNumber(brightness, "min", 0);
		setNumber(brightness, "max", kBacklightMax);
		setNumber(brightness, "value", fValue);
		parameters->setObject("brightness", brightness);
		setProperty("IODisplayParameters", parameters);
	}
	OSSafeReleaseNULL(brightness);
	OSSafeReleaseNULL(parameters);
}

/* a "brightness": a number of 0..max, or { value = N } */
static uint32_t gPendingFade;           /* FadePeriod of the request being taken (16.16 s) */

/*
 * fb_bl_curve=1 (default; A, 2026-11-13, user: "50-100% almost feels the same"): the slider's legacy value to nits on a
 * perceptual curve, not value x 420. backboardd writes the Control Center slider s as 0.025 + 0.95 s (measured: 0.1 ->
 * 0.120, 0.5 -> 0.500, 1.0 -> 0.975), so that range is stretched back to 0..1 first, then nits = 2 + 418 s^2.2 (the
 * eye's response; CoreBrightness's own slider -> nits is ~s^2.05): 25 % 22 nits, 50 % 93, 75 % 224, 100 % 420.
 * Linear gave 50 % = 210 and 100 % = 420, one doubling the eye barely notices. fb_bl_curve=0: the old linear map.
 * The same curve is in userland/src/brightness_policy.h (rb_slider_nits), for auto-brightness's manual anchor.
 */
static TUNABLE(uint32_t, gBLCurve, "fb_bl_curve", 1);
static const uint32_t gSliderNits16[33] = {      /* 2 + 418 s^2.2 nits in 16.16, s = i / 32 */
	131072, 144448, 192532, 281038, 413468, 592454, 820135, 1098329, 1428622, 1812428, 2251025, 2745581, 3297175,
	3906808, 4575419, 5303888, 6093048, 6943688, 7856557, 8832373, 9871819, 10975551, 12144201, 13378376, 14678660,
	16045619, 17479800, 18981731, 20551927, 22190886, 23899093, 25677018, 27525120,
};

static uint32_t
legacyToNits16(uint32_t value)
{
	if (!gBLCurve) {
		return (uint32_t) ((uint64_t) value * kPanelMaxNits);
	}
	if (value == 0) {
		return 0;                               /* the display going off */
	}
	/* s in 1/65536: (value / 65536 - 0.025) / 0.95 */
	int64_t lo = kBacklightMax / 40, span = (int64_t) kBacklightMax * 95 / 100;
	int64_t s = ((int64_t) value - lo) * kBacklightMax / span;
	s = s < 0 ? 0 : s > kBacklightMax ? kBacklightMax : s;
	uint32_t i = (uint32_t) (s >> 11), frac = (uint32_t) (s & 2047);
	if (i >= 32) {
		return gSliderNits16[32];
	}
	return gSliderNits16[i] + (uint32_t) (((uint64_t) (gSliderNits16[i + 1] - gSliderNits16[i]) * frac) >> 11);
}

bool
AppleARMBacklight::takeBrightness(OSObject * value, const char * name)
{
	OSNumber * number = OSDynamicCast(OSNumber, value);
	OSDictionary * dict = OSDynamicCast(OSDictionary, value);

	if (number == NULL && dict != NULL) {
		number = OSDynamicCast(OSNumber, dict->getObject("value"));
	}
	if (number == NULL) {
		return false;
	}
	fValue = number->unsigned32BitValue() > kBacklightMax ? kBacklightMax : number->unsigned32BitValue();
	publishParameters();
	if (fFB != NULL) {
		fFB->brightnessRequest(legacyToNits16(fValue), gPendingFade);
	}
	fbSay("fb: bl: backlight %s = %u of %u\n", name, fValue, kBacklightMax);
	return true;
}

IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSSymbol * key;
	bool taken = false;

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	fRequests++;
	{
		OSNumber * fade = OSDynamicCast(OSNumber, dict->getObject("FadePeriod"));

		gPendingFade = fade != NULL ? fade->unsigned32BitValue() : 0;
	}
	/* iOS also writes native 16.16 luminance here. It must succeed and remain
	 * readable. Legacy brightness writes remain active for Control Center;
	 * neither interface permanently disables the other. */
	OSNumber * nits = OSDynamicCast(OSNumber, dict->getObject("brightness-nits"));
	if (nits != NULL) {
		uint64_t raw = nits->unsigned64BitValue();
		uint32_t native = raw > ((uint64_t) kPanelMaxNits << 16) ? kPanelMaxNits << 16 : (uint32_t) raw;
		setProperty("brightness-nits", native, 32);
		if (fFB != NULL) {
			fFB->brightnessRequest(native, gPendingFade);
		}
		taken = true;
	}
	OSNumber * nitsCap = OSDynamicCast(OSNumber, dict->getObject("brightness-nits-cap"));
	if (nitsCap != NULL) {
		setProperty("brightness-nits-cap", nitsCap);
		taken = true;
	}
	keys = OSCollectionIterator::withCollection(dict);
	while (keys != NULL && (key = OSDynamicCast(OSSymbol, keys->getNextObject())) != NULL) {
		OSObject * value = dict->getObject(key);
		OSNumber * number = OSDynamicCast(OSNumber, value);

		if (fRequests <= 40) {
			fbSay("fb: bl: backlight setProperties %u: %s = %s%llu\n", fRequests, key->getCStringNoCopy(),
			    number ? "" : value && OSDynamicCast(OSDictionary, value) ? "(dictionary) " : "(other) ",
			    number ? number->unsigned64BitValue() : 0);
		}
		if (key->isEqualTo("brightness")) {
			taken |= takeBrightness(value, "brightness");
		} else if (key->isEqualTo("IODisplayParameters") && OSDynamicCast(OSDictionary, value) != NULL) {
			OSObject * inner = ((OSDictionary *) value)->getObject("brightness");

			taken |= inner != NULL && takeBrightness(inner, "IODisplayParameters.brightness");
		}
	}
	OSSafeReleaseNULL(keys);
	return taken ? kIOReturnSuccess : kIOReturnUnsupported;
}

static void
publishBacklight(IOMobileFramebuffer * fb)
{
	AppleARMBacklight * backlight = OSTypeAlloc(AppleARMBacklight);

	if (backlight != NULL && backlight->init() && backlight->attach(fb)) {
		backlight->fFB = fb;
		backlight->fValue = kBacklightMax / 2;
		backlight->setName("backlight");
		backlight->setProperty("backlight-control", kOSBooleanTrue);
		/*
		 * What CoreBrightness reads before it builds its Display (B, 2026-11-06; found by the Pixel 10 port,
		 * git/TheAndersMadsen__ios-on-pixel-10/research/ios26-phaseb-r1/corebrightness/FINDINGS.md; the same names in
		 * iOS 16.5's cache): -[CBBacklightNode getBacklightCalibration:] reads "backlight-calibration-parameters" (16.16
		 * numbers); without it, a fallback interpolates "backlight-marketing-table" before that table is loaded -- the
		 * NULL read at +0x1c8 that crashed backboardd with an ambient light sensor (sens_als=1). Values: a consistent model
		 * of this panel (2..420 nits on Samsung's curve points), not measured.
		 */
		{
			static const uint32_t nits[] = { 2, 5, 10, 20, 40, 70, 100, 140, 180, 230, 280, 340, 420 };
			const unsigned n = sizeof(nits) / sizeof(nits[0]);
			int32_t table[sizeof(nits) / sizeof(nits[0])];
			OSDictionary * calib = OSDictionary::withCapacity(9);

			for (unsigned i = 0; i < n; i++) {
				table[i] = (int32_t) (nits[i] << 16);
			}
			OSData * marketing = OSData::withBytes(table, sizeof(table));
			if (marketing != NULL) {
				backlight->setProperty("backlight-marketing-table", marketing);
				marketing->release();
			}
			if (calib != NULL) {
				int32_t lmax = table[n - 1], lmin = table[0], lmid = table[(n - 1) / 2];
				int32_t imax = 20 << 16, imid = (int32_t) ((int64_t) imax * lmid / lmax);
				const struct { const char * key; int32_t value; } v[] = {
					{ "current-for-max-backlight", imax }, { "current-for-mid-backlight", imid },
					{ "hardware-max-current-limit", imax }, { "lMaxProduct", lmax }, { "lMidProduct", lmid },
					{ "lMinProduct", lmin }, { "lMaxPanel", lmax }, { "lMidPanel", lmid }, { "lMinPanel", lmin },
				};

				for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
					OSNumber * num = OSNumber::withNumber((uint64_t) (uint32_t) v[i].value, 32);

					if (num != NULL) {
						calib->setObject(v[i].key, num);
						num->release();
					}
				}
				backlight->setProperty("backlight-calibration-parameters", calib);
				calib->release();
			}
		}
		backlight->publishParameters();
		backlight->registerService();
		fbSay("fb: bl: AppleARMBacklight published%s\n", gBL ? "" : " (fb_bl=0: requests only logged)");
	}
	OSSafeReleaseNULL(backlight);
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
	uint32_t metal = 0;

	gAcceleratorTakesProperties = PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0;
	{
		/* on with Metal unless gles=0: the phone's boot line is at its 550-character limit */
		uint32_t gles = 1;

		PE_parse_boot_argn("gles", &gles, sizeof(gles));
		gAcceleratorOffersGLES = gAcceleratorTakesProperties && gles != 0;
	}
	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
        accelerator->setProperty("RedwoodGraphicsPolicyVersion", (unsigned long long)1, 32);
		accelerator->registerService();
		/* the first line to look for in the log of such a boot: the gate is open, nothing is set yet */
		if (gAcceleratorTakesProperties) {
			IOLog("IOAcceleratorES: metal=%u: this service takes " kAcceleratorPluginNameKey
			    " and " kAcceleratorPluginClassNameKey " from user space\n", metal);
		}
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
/*
 * On the way back to the loader (osfmk/arm64/loader_return.c, one processor, interrupts off): the panel's
 * window back on S-Boot's channel, so the loader's menu -- drawn into the firmware's framebuffer -- is what
 * shows, not CoreAnimation's last frame on the scaler's channel. Registers only: no lock, no sleep.
 */
extern "C" void loader_set_display_size(uint32_t width, uint32_t height);

void
IOMobileFramebuffer::loaderQuiesce(void)
{
	loader_set_display_size(fPanelWidth, fPanelHeight);
	if (!fScalerOn || fDecon == NULL || fWin < 0) {
		return;
	}
	uint32_t paths1 = dpuRead(fDecon, DECON_DATA_PATH_1), mask = DECON_SHADOW_GLOBAL | (1u << fWin);

	dpuWrite(fDecon, DECON_DATA_PATH_1, (paths1 & ~(7u << (4 * fWin))) | (fScanCh << (4 * fWin)));
	__asm__ volatile ("dsb sy" ::: "memory");
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | mask);
	for (unsigned i = 0; i < 400 && (dpuRead(fDecon, DECON_SHADOW_UPDATE) & mask) != 0; i++) {
		delay(100);
	}
	fScalerOn = false;
}

/*
 * The panel held under `nits` (0: as asked again) while the chip is hot, from the thermal loop
 * (bsd/dev/redwood_exynos_acpm.c): what CoreBrightness asks is kept, and comes back when the cap goes.
 */
extern "C" void redwood_fb_thermal_cap(uint32_t nits);
void
redwood_fb_thermal_cap(uint32_t nits)
{
	uint32_t cap = nits << 16;

	if (cap == gBLThermalCap) {
		return;
	}
	gBLThermalCap = cap;
	if (gFramebuffer != NULL && gFramebuffer->fBLCall != NULL && gFramebuffer->fDsim != NULL) {
		thread_call_enter(gFramebuffer->fBLCall);
	}
}

/* kern.redwood.display_aod (bsd/dev/redwood_dbgport.c): see gAODDim. Entering fades over 600 ms, leaving over 200. */
extern "C" int redwood_fb_aod(int value);
extern "C" int
redwood_fb_aod(int value)
{
	IOMobileFramebuffer * fb = gFramebuffer;

	if (value < 0) {
		return (int) gAODDim;
	}
	if (value > 60) {
		return -1;
	}
	if ((uint32_t) value == gAODDim) {
		return value;
	}
	if (fb == NULL || fb->fBLCall == NULL || fb->fDsim == NULL || fb->fDsimLock == NULL) {
		gAODDim = (uint32_t) value;
		return value;
	}
	IOLockLock(fb->fDsimLock);
	IOLockLock(fb->fBLLock);
	gAODDim = (uint32_t) value;
	gAODBase = value == 1 ? fb->fBLTarget : 0;     /* entering: the brightness right now */
	gAODLevel = gAODBase ? aodFrom(gAODBase) : 0;
	if (fb->fBLTarget != 0 && fb->fBLWrites != 0) {
		uint64_t len;

		nanoseconds_to_absolutetime((value ? 600 : 200) * NSEC_PER_MSEC, &len);
		fb->fFadeFrom = fb->fBLCurrent;
		fb->fFadeStart = mach_absolute_time();
		fb->fFadeLen = len;
	}
	IOLockUnlock(fb->fBLLock);
	thread_call_enter(fb->fBLCall);
	IOLockUnlock(fb->fDsimLock);
	return value;
}

extern "C" void redwood_fb_quiesce(void);
extern "C" void
redwood_fb_quiesce(void)
{
	if (gFramebuffer != NULL) {
		gFramebuffer->loaderQuiesce();
	}
}

extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishBacklight(fb);
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}

/* ---- 1080x2400 at 120 Hz (x1slte, SM-G980F, fb_mode=1) ------------------------------------------------------------ */

/*
 * Samsung's runtime resolution and refresh change ("mres", dpu30), for the S20's S6E3HAB "hubble1" panel, as Android's
 * own driver does it, with every number from Samsung's sources for this panel and this phone's dtbo:
 *   the panel  drivers/video/fbdev/exynos/panel/s6e3hab/s6e3hab_hubble1_a3_s0_panel.h: the display-mode command
 *              set (osc_86_4m / osc_96_5m_display_mode_cmdtbl), its tables (scaler, caset, paset, pps, osc, ltps,
 *              fps), the 120 HS row of S6E3HAB_VRR_DIM (s6e3hab.h: vfp 16), the variant by the panel's ID
 *              (0x8100x1..x3: OSC 96.5 MHz; 0x810041/42: the default one); "B0 offset register" before a write at an
 *              offset (ddi_props.gpara WRITE|POINT, panel.c panel_dsi_write_cmd), DCS long writes for 2+ bytes
 *              (dpu30/panels/common_mipi_lcd.c), DSC "PRA" type 0x07 and PPS type 0x0A
 *   DSIM       cal_9830/dsim_reg.c dsim_reg_set_mres: threshold = enc_sw x slices, RESOL, transfers = lines,
 *              SLICE01 = dec_sw, the command-mode underrun reference (dtbo cmd_lp_ref)
 *   DECON      cal_9830/decon_reg.c decon_reg_set_mres: blender background, scaled size, DSC encoders + OUTFIFO
 *   the modes  dtbo panel_display_mode 1440x3200_60NS (slice 720x40, cmd_lp_ref 0x1e6e) and 1080x2400_120HS (slice
 *              540x40, cmd_lp_ref 0x284d); 2 encoders, 2 slices; enc_sw (exynos_panel_calc_slice_width) 240 / 180
 * It goes only from the state it was measured against: every DSIM register it changes must read UEFI's native-mode
 * value first, or nothing at all is written and the panel stays at 1440x3200 at 60 Hz.
 */
#define DSIM_NUM_OF_TRANSFER_R  0x0030
#define DSIM_UNDERRUN_CTRL_R    0x0034
#define DSIM_THRESHOLD_R        0x0038
#define DSIM_RESOL_R            0x003C
#define DSIM_SLICE01_R          0x0078
#define DSI_DSC_PRA             0x07
#define DSI_DSC_PPS             0x0A

/* the 128-byte PPS of 1080x2400, slice 540x40 (hubble1_a3_s0_pps_table[0][S6E3HAB_SCALER_x1_78]) */
static const uint8_t kPpsFhd[128] = {
	0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x09, 0x60, 0x04, 0x38, 0x00, 0x28, 0x02, 0x1C, 0x02, 0x1C,
	0x02, 0x00, 0x02, 0x0E, 0x00, 0x20, 0x03, 0xDD, 0x00, 0x07, 0x00, 0x0C, 0x02, 0x77, 0x02, 0x8B,
	0x18, 0x00, 0x10, 0xF0, 0x03, 0x0C, 0x20, 0x00, 0x06, 0x0B, 0x0B, 0x33, 0x0E, 0x1C, 0x2A, 0x38,
	0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7B, 0x7D, 0x7E, 0x01, 0x02, 0x01, 0x00, 0x09, 0x40,
	0x09, 0xBE, 0x19, 0xFC, 0x19, 0xFA, 0x19, 0xF8, 0x1A, 0x38, 0x1A, 0x78, 0x1A, 0xB6, 0x2A, 0xF6,
	0x2B, 0x34, 0x2B, 0x74, 0x3B, 0x74, 0x6B, 0xF4,
};

/* the HS rows of the LTPS tables, per oscillator */
static const uint8_t kLtps1Osc96[8] = { 0x17, 0x53, 0x00, 0x00, 0x00, 0x40, 0x17, 0x53 };
static const uint8_t kLtps2Osc96[8] = { 0x18, 0x1C, 0x00, 0x00, 0x00, 0xD8, 0x18, 0x1C };
static const uint8_t kLtps1Osc86[8] = { 0x15, 0x4A, 0x00, 0x00, 0x00, 0x40, 0x15, 0x4A };
static const uint8_t kLtps2Osc86[8] = { 0x16, 0x19, 0x00, 0x00, 0x00, 0xD8, 0x16, 0x19 };

/* the DECON half (blender, scaled size, OUTFIFO, the two DSC encoders, the window): see deconModeRegs below */
struct mode_reg { uint32_t off, native, fhd, mask; };
#include "MiniIOMobileFramebufferMode.h"


/* A DSI packet of the given data type: short (type 0x05 / 0x15 / 0x07: up to two bytes) or long (0x39, 0x0A). */
bool
IOMobileFramebuffer::dsimPacket(uint8_t type, const uint8_t * bytes, uint32_t length)
{
	uint64_t began = mach_absolute_time(), limit;
	uint32_t header;
	bool longp = type == DSI_DCS_LONG_WRITE || type == DSI_DSC_PPS;

	nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &limit);
	while (dpuRead(fDsim, DSIM_FIFOCTRL) & (DSIM_FIFO_PH_FULL | DSIM_FIFO_PL_FULL)) {
		if (mach_absolute_time() - began > limit) {
			return false;
		}
		delay(10);
	}
	if (longp) {
		for (uint32_t i = 0; i < length; i += 4) {
			uint32_t word = 0;

			for (uint32_t b = 0; b < 4 && i + b < length; b++) {
				word |= (uint32_t) bytes[i + b] << (8 * b);
			}
			dpuWrite(fDsim, DSIM_PAYLOAD, word);
		}
		header = type | (length & 0xff) << 8 | (length >> 8 & 0xff) << 16;
	} else {
		header = type | (uint32_t) (length > 0 ? bytes[0] : 0) << 8 | (uint32_t) (length > 1 ? bytes[1] : 0) << 16;
	}
	dpuWrite(fDsim, DSIM_PKTHDR, header);
	while (!(dpuRead(fDsim, DSIM_FIFOCTRL) & DSIM_FIFO_PH_EMPTY)) {
		if (mach_absolute_time() - began > limit) {
			return false;
		}
		delay(10);
	}
	return true;
}

/* A write of the panel's register bytes[0] at an offset: "B0 offset register" first when the offset is not 0. */
bool
IOMobileFramebuffer::panelWriteAt(const uint8_t * bytes, uint32_t length, uint8_t offset)
{
	if (offset != 0) {
		const uint8_t gpara[3] = { 0xB0, offset, bytes[0] };

		if (!dsimPacket(DSI_DCS_LONG_WRITE, gpara, sizeof(gpara))) {
			return false;
		}
	}
	return dsimPacket(length == 1 ? DSI_DCS_SHORT_WRITE : DSI_DCS_LONG_WRITE, bytes, length);
}

/*
 * The boot logo across the panel's switch (A, 2026-11-15). The loader leaves its picture (iOS's boot logo, centered)
 * at the native 1440x3200 in the boot framebuffer; fb_mode then cuts S-Boot's window to 1080x2400 (720x1600) of
 * the same rows, so until iOS's first frame the panel showed that picture's top-left corner: the logo a third
 * bigger and off to the lower right. While the switch has the trigger masked, the picture is scaled down in place
 * (a box filter; each output pixel reads only pixels at or after its own place, so in-place is safe).
 * Only before iOS's first frame. fb_bootpic=0: as before.
 */
static TUNABLE(uint32_t, gBootPic, "fb_bootpic", 1);
static bool gBootPicDone;

static void
bootPictureScale(uint8_t * base, uint32_t rowBytes, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
	for (uint32_t y = 0; y < dh; y++) {
		uint32_t y0 = y * sh / dh, y1 = ((y + 1) * sh + dh - 1) / dh;
		uint32_t * dst = (uint32_t *) (base + (size_t) y * rowBytes);

		for (uint32_t x = 0; x < dw; x++) {
			uint32_t x0 = x * sw / dw, x1 = ((x + 1) * sw + dw - 1) / dw, n = 0;
			uint32_t c[4] = { 0, 0, 0, 0 };

			for (uint32_t sy = y0; sy < y1 && sy < sh; sy++) {
				const uint32_t * src = (const uint32_t *) (base + (size_t) sy * rowBytes);

				for (uint32_t sx = x0; sx < x1 && sx < sw; sx++, n++) {
					uint32_t p = src[sx];

					c[0] += p & 0xff; c[1] += (p >> 8) & 0xff; c[2] += (p >> 16) & 0xff; c[3] += p >> 24;
				}
			}
			n = n ? n : 1;
			dst[x] = (c[0] / n) | ((c[1] / n) << 8) | ((c[2] / n) << 16) | ((c[3] / n) << 24);
		}
	}
}

bool
IOMobileFramebuffer::modeSetFhd120(void)
{
	const bool hd = fModeAsk == 3 || fModeAsk == 4;
	const uint32_t width = hd ? 720 : 1080, height = hd ? 1600 : 2400;
	const struct { uint32_t off, native, fhd, mask; } dsim[] = {
		{ DSIM_THRESHOLD_R,       480,                 width / 3,                 0xffffffffu },
		{ DSIM_RESOL_R,           (3200u << 16) | 480, (height << 16) | (width / 3), (0x1fffu << 16) | 0x1fffu },
		{ DSIM_NUM_OF_TRANSFER_R, 3200,                height,                0xffffffffu },
		{ DSIM_SLICE01_R,         (720u << 16) | 720,  ((width / 2) << 16) | (width / 2),  0xffffffffu },
		{ DSIM_UNDERRUN_CTRL_R,   0x1e6e,              hd ? 0x2c8cU : 0x284dU,              0xffffu },
	};
	unsigned ndecon = 0;                    /* the entries with a mask: an all-zero one only holds the place */

	for (unsigned i = 0; i < sizeof(deconModeRegs) / sizeof(deconModeRegs[0]); i++) {
		ndecon += deconModeRegs[i].mask != 0;
	}
	uint8_t id[3] = { 0 };
	bool osc96, ok = true;
	uint32_t trig, global;
	uint64_t began, limit;

	if (fDsim == NULL) {
		fDsim = (volatile uint8_t *) ml_io_map((vm_offset_t) DSIM0_PHYS, 0x1000);
	}
	if (fDsim == NULL || fDecon == NULL) {
		fbSay("fb_mode: DSIM or DECON not mapped: stays 1440x3200 at 60 Hz\n");
		return false;
	}
	/* the gate: the native mode, exactly as measured, or nothing */
	for (unsigned i = 0; i < sizeof(dsim) / sizeof(dsim[0]); i++) {
		uint32_t v = dpuRead(fDsim, dsim[i].off) & dsim[i].mask;

		if (v != (dsim[i].native & dsim[i].mask)) {
			if ((gFbModeLpRef || fModeLpRef) && dsim[i].off == DSIM_UNDERRUN_CTRL_R) {
				fbSay("fb_mode: DSIM %#x (underrun reference) reads %#x, not the dtbo's %#x: let through "
				    "(fb_mode_lpref=1; Samsung writes it at every mode set), %#x will be written\n", dsim[i].off, v,
				    dsim[i].native, dsim[i].fhd);
				continue;
			}
			fbSay("fb_mode: DSIM %#x reads %#x, not the native %#x: nothing written, stays 1440x3200 at 60 Hz\n",
			    dsim[i].off, v, dsim[i].native);
			ok = false;
		}
	}
	for (unsigned i = 0; i < sizeof(deconModeRegs) / sizeof(deconModeRegs[0]) && deconModeRegs[i].mask; i++) {
		uint32_t v = dpuRead(fDecon, deconModeRegs[i].off) & deconModeRegs[i].mask;

		if (v != (deconModeRegs[i].native & deconModeRegs[i].mask)) {
			fbSay("fb_mode: DECON %#x reads %#x, not the native %#x: nothing written, stays 1440x3200 at 60 Hz\n",
			    deconModeRegs[i].off, v, deconModeRegs[i].native);
			ok = false;
		}
	}
	if (ndecon == 0) {
		fbSay("fb_mode: no DECON register list in this build: nothing written\n");
		ok = false;
	}
	/* S-Boot's window (the scaler's from here on) ends at the panel's corner: (1439, 3199) -> (1079, 2399) */
	if (fWin < 0 || fWin > 5 || dpuRead(fDecon, 0x100C + 0x30 * (uint32_t) fWin) != ((3199u << 16) | 1439u)) {
		fbSay("fb_mode: window %d does not end at 1439,3199 (%#x): nothing written\n", fWin,
		    fWin >= 0 && fWin <= 5 ? dpuRead(fDecon, 0x100C + 0x30 * (uint32_t) fWin) : 0);
		ok = false;
	}
	if (!ok) {
		return false;
	}
	/* the panel's variant: its ID (DCS 0x04) */
	{
		static const uint8_t keyOn[] = { 0xF0, 0x5A, 0x5A }, keyOff[] = { 0xF0, 0xA5, 0xA5 };

		fDsimReads = true;
		(void) dsimWrite(keyOn, sizeof(keyOn));
		if (dsimRead(0x04, id, 3) != 3) {
			(void) dsimWrite(keyOff, sizeof(keyOff));
			fbSay("fb_mode: the panel's ID did not come: nothing written\n");
			return false;
		}
		(void) dsimWrite(keyOff, sizeof(keyOff));
	}
	osc96 = (id[2] & 0x40) == 0;
	fbSay("fb_mode: panel ID %02x %02x %02x (%s): to %ux%u at %u Hz\n", id[0], id[1], id[2],
	    osc96 ? "OSC 96.5 MHz" : "OSC 86.4 MHz", width, height, (fModeAsk == 1 || fModeAsk == 3) ? 120 : 60);

	/* the trigger masked, DECON idle: no frame goes out while the two ends change */
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &limit);
	began = mach_absolute_time();
	while (dpuRead(fDecon, DECON_SHADOW_UPDATE) != 0 && mach_absolute_time() - began < limit) {
		delay(50);                      /* Samsung: no shadow update pending before the trigger is masked */
	}
	trig = dpuRead(fDecon, DECON_TRIG_CONTROL);
	dpuWrite(fDecon, DECON_TRIG_CONTROL, trig | DECON_TRIG_MASK);
	began = mach_absolute_time();
	while (!((global = dpuRead(fDecon, DECON_GLOBAL_CONTROL)) & DECON_GLOBAL_IDLE) && mach_absolute_time() - began < limit) {
		delay(50);
	}
	if (!(global & DECON_GLOBAL_IDLE)) {
		dpuWrite(fDecon, DECON_TRIG_CONTROL, trig);
		fbSay("fb_mode: DECON not idle in 100 ms (global %#x): nothing written\n", global);
		return false;
	}

	/* the panel: Samsung's display-mode command set for 1080x2400_120HS */
	{
		static const uint8_t key1On[] = { 0x9F, 0xA5, 0xA5 }, key2On[] = { 0xF0, 0x5A, 0x5A };
		static const uint8_t key2Off[] = { 0xF0, 0xA5, 0xA5 }, key1Off[] = { 0x9F, 0x5A, 0x5A };
		static const uint8_t aidHs[] = { 0xB1, 0x40 };                   /* AID 2 cycles in HS (offset 0x06) */
		static const uint8_t dsc[] = { 0x01 };
		const uint8_t caset[] = { 0x2A, 0, 0, (uint8_t)((width - 1) >> 8), (uint8_t)(width - 1) };
		const uint8_t paset[] = { 0x2B, 0, 0, (uint8_t)((height - 1) >> 8), (uint8_t)(height - 1) };
		const uint8_t scaler[] = { 0xBA, (uint8_t)(hd ? 0 : 2), 0x26, 0x08, 0x08, 0xF3 };
		static const uint8_t osc[] = { 0xF2, 0xC0 }, vfpNm[] = { 0xF2, 0x00, 0x10 }, vfpHs[] = { 0xF2, 0x00, 0x10 };
		static const uint8_t pwrGen[] = { 0xF3, 0x00 }, srcAmp[] = { 0xF6, 0x01 }, ltps0[] = { 0xCB, 0x85 };
		static const uint8_t fps[] = { 0x60, 0x20 }, gammaUpdate[] = { 0xF7, 0x0F };
		uint8_t ltps1[9] = { 0xCB }, ltps2[9] = { 0xCB };

		memcpy(ltps1 + 1, osc96 ? kLtps1Osc96 : kLtps1Osc86, 8);
		memcpy(ltps2 + 1, osc96 ? kLtps2Osc96 : kLtps2Osc86, 8);
		/*
		 * fb_mode=2 (B, 2026-11-06): the resolution half alone -- PPS, CASET/PASET, scaler -- at the panel's own 60 Hz
		 * (no AID/OSC/VFP/LTPS/FPS: those are the 120 HS half). Every 1080x2400 mode of the dtbo has the same DSIM and
		 * DECON values (cmd_lp_ref 0x284d), so this tells which half garbled the first fb_mode=1 picture.
		 */
		bool hs = fModeAsk == 1 || fModeAsk == 3;

		ok = panelWriteAt(key1On, sizeof(key1On), 0) && panelWriteAt(key2On, sizeof(key2On), 0) &&
		    (!hs || panelWriteAt(aidHs, sizeof(aidHs), 0x06));
		IOSleep(17);                                                    /* Samsung: wait 1 vsync */
		ok = ok && dsimPacket(DSI_DSC_PRA, dsc, 1) && dsimPacket(DSI_DSC_PPS, hd ? kPpsHd : kPpsFhd, sizeof(kPpsFhd)) &&
		    panelWriteAt(caset, sizeof(caset), 0) && panelWriteAt(paset, sizeof(paset), 0) &&
		    panelWriteAt(scaler, sizeof(scaler), 0) && (!hs || (
		    panelWriteAt(osc, sizeof(osc), 0) && panelWriteAt(vfpNm, sizeof(vfpNm), 0x0B) &&
		    panelWriteAt(vfpHs, sizeof(vfpHs), 0x06) && panelWriteAt(pwrGen, sizeof(pwrGen), 0x06) &&
		    panelWriteAt(srcAmp, sizeof(srcAmp), 0x18) && panelWriteAt(ltps0, sizeof(ltps0), 0x03) &&
		    panelWriteAt(ltps1, sizeof(ltps1), 0x54) && panelWriteAt(ltps2, sizeof(ltps2), 0x8E) &&
		    panelWriteAt(fps, sizeof(fps), 0))) && panelWriteAt(gammaUpdate, sizeof(gammaUpdate), 0) &&
		    panelWriteAt(key2Off, sizeof(key2Off), 0) && panelWriteAt(key1Off, sizeof(key1Off), 0);
		if (!ok) {
			fbSay("fb_mode: a panel command did not go out (FIFO %#x): the panel may be half-switched\n",
			    dpuRead(fDsim, DSIM_FIFOCTRL));
		}
	}
	/* DSIM, then DECON (dsim_reg_set_mres, decon_reg_set_mres), then one shadow update */
	for (unsigned i = 0; i < sizeof(dsim) / sizeof(dsim[0]); i++) {
		uint32_t fhd = dsim[i].fhd;

		/* fb.mode N 1 L: the underrun reference left as found (L=1) or written as L (Samsung's 0x284d assumes Samsung's
		 * DSIM clock; S-Boot's 0x138b for the dtbo's 0x1e6e says its clock is ~0.64 of that) */
		if (dsim[i].off == DSIM_UNDERRUN_CTRL_R && fModeLpVal == 1) {
			continue;
		}
		if (dsim[i].off == DSIM_UNDERRUN_CTRL_R && fModeLpVal > 1) {
			fhd = (dsim[i].fhd & ~dsim[i].mask) | (fModeLpVal & dsim[i].mask);
		}
		dpuWrite(fDsim, dsim[i].off, (dpuRead(fDsim, dsim[i].off) & ~dsim[i].mask) | (fhd & dsim[i].mask));
	}
	for (unsigned i = 0; i < sizeof(deconModeRegs) / sizeof(deconModeRegs[0]) && deconModeRegs[i].mask; i++) {
		dpuWrite(fDecon, deconModeRegs[i].off,
		    (dpuRead(fDecon, deconModeRegs[i].off) & ~deconModeRegs[i].mask) | ((hd ? deconHdValues[i] : deconModeRegs[i].fhd) & deconModeRegs[i].mask));
	}
	if (gBootPic && !gBootPicDone && !fScalerOn && fBase != NULL && fRowBytes >= 1440 * 4) {
		/* (the trigger is masked: no frame goes out half scaled.) The boot framebuffer is uncached: read it out in one
		 * block, scale in ordinary memory, write it back in one block -- a pixel-by-pixel pass over the framebuffer
		 * took long enough to tear (user, 2026-11-15) */
		size_t size = (size_t) fRowBytes * 3200;
		uint8_t * copy = (uint8_t *) IOMallocData(size);
		uint64_t t0 = mach_absolute_time(), t1, ns;

		gBootPicDone = true;
		if (copy != NULL) {
			memcpy(copy, fBase, size);
			bootPictureScale(copy, fRowBytes, 1440, 3200, width, height);
			for (uint32_t y = 0; y < height; y++) {
				memcpy(fBase + (size_t) y * fRowBytes, copy + (size_t) y * fRowBytes, (size_t) width * 4);
			}
			IOFreeData(copy, size);
		} else {
			bootPictureScale(fBase, fRowBytes, 1440, 3200, width, height);
		}
		t1 = mach_absolute_time();
		absolutetime_to_nanoseconds(t1 - t0, &ns);
		fbSay("fb_mode: the loader's picture scaled 1440x3200 -> %ux%u in %llu ms%s (the boot logo keeps its size and place)\n",
		    width, height, ns / NSEC_PER_MSEC, copy != NULL ? "" : " (in place: no memory for a copy)");
	}
	dpuWrite(fDecon, 0x100C + 0x30 * (uint32_t) fWin, ((height - 1) << 16) | (width - 1));       /* WIN_END_POSITION */
	/*
	 * (B, 2026-11-07) What feeds the window must fit 1080x2400 from the very first frame: before, the window was cut to
	 * 1080x2400 while its channel still delivered 1440x3200 (S-Boot's channel at boot; the scaler's channel 5, scaled to
	 * 1440x3200, live), DECON refused to start a frame (TE kept coming, the frame count stopped), and the scaler's
	 * "from the next frame" never came. Cropped / re-scaled in the same update.
	 */
	{
		volatile uint8_t * dppS = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DPP_PHYS + 0x1000ULL * (fScanCh + 1)), 0x1000);

		if (fDma != NULL) {
			dpuWrite(fDma, IDMA_IMG_SIZE_REG, (height << 16) | width);
		}
		if (dppS != NULL) {
			dpuWrite(dppS, DPP_IMG_SIZE_REG, (height << 16) | width);
			dpuWrite(dppS, DPP_SCALED_IMG_SIZE_REG, (height << 16) | width);
		}
		if (fScalerReady && fDpp5 != NULL) {
			dpuWrite(fDpp5, DPP_SCALED_IMG_SIZE_REG, (height << 16) | width);
			dpuWrite(fDpp5, DPP_MAIN_H_RATIO_REG, (uint32_t) (((uint64_t) fWidth << 20) / width));
			dpuWrite(fDpp5, DPP_MAIN_V_RATIO_REG, (uint32_t) (((uint64_t) fHeight << 20) / height));
		}
	}
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, DECON_SHADOW_GLOBAL | (1u << fWin));
	dpuWrite(fDecon, DECON_TRIG_CONTROL, trig);
	fPanelWidth = width;
	fPanelHeight = height;
	if (gWarmOn) dpuWrite(fDecon, 0x2004, (height << 16) | width);
	gModeHz = (fModeAsk == 1 || fModeAsk == 3) ? 120 : 60;           /* fb_mode=2: the resolution alone, at 60 Hz */
	if (gFactoryEnabled && fBLCall != NULL) {
		fBLValid = false;
		thread_call_enter(fBLCall); /* reapply the gamma table for the new refresh mode */
	}
	/* Clients may cache timing properties from boot; publish the new scanout rate too. */
	publishModes();
	messageClients(kIOMessageServicePropertyChange);
	fbSay("fb_mode: %ux%u at %u Hz set (panel commands %s); DSIM RESOL %#x THRESHOLD %u SLICE01 %#x; %u DECON registers\n",
	    width, height, gModeHz, ok ? "all sent" : "NOT ALL SENT", dpuRead(fDsim, DSIM_RESOL_R), dpuRead(fDsim, DSIM_THRESHOLD_R),
	    dpuRead(fDsim, DSIM_SLICE01_R), ndecon);
	return ok;
}


/* ---- live display mode (B, 2026-11-07) ------------------------------------------------------------------------------ */

/* DECON's TE and frame counters over 100 ms, and the registers that say whether a frame can start */
void
IOMobileFramebuffer::modeCounters(const char * when, char * out, size_t cap)
{
	uint32_t te0, fc0, te1, fc1;
	size_t at = strlen(out);

	if (fDecon == NULL || fDsim == NULL) {
		snprintf(out + at, cap - at, "%s: DECON/DSIM not mapped\n", when);
		return;
	}
	te0 = dpuRead(fDecon, DECON_TE_COUNT);
	fc0 = dpuRead(fDecon, DECON_FRAME_COUNT);
	IOSleep(100);
	te1 = dpuRead(fDecon, DECON_TE_COUNT);
	fc1 = dpuRead(fDecon, DECON_FRAME_COUNT);
	snprintf(out + at, cap - at, "%s: in 100 ms TE-count delta %#x (raw), %u frames; DECON global %#x trig %#x shadow %#x int-pending %#x "
	    "int-enable %#x; DSIM intsrc %#x fifoctrl %#x link1 %#x config %#x; %ux%u\n", when, te1 - te0, fc1 - fc0,
	    dpuRead(fDecon, DECON_GLOBAL_CONTROL), dpuRead(fDecon, DECON_TRIG_CONTROL), dpuRead(fDecon, DECON_SHADOW_UPDATE),
	    dpuRead(fDecon, DECON_INT_PENDING), dpuRead(fDecon, DECON_INT_ENABLE), dpuRead(fDsim, DSIM_INTSRC),
	    dpuRead(fDsim, DSIM_FIFOCTRL), dpuRead(fDsim, DSIM_LINK_STATUS1), dpuRead(fDsim, DSIM_CONFIG), fPanelWidth, fPanelHeight);
}

void
IOMobileFramebuffer::modeLive(int mode)
{
	fModeReport[0] = 0;
	modeCounters("before", fModeReport, sizeof(fModeReport));
	if (mode >= 1 && mode <= 4) {
		bool ok;

		if (fPanelWidth != 1440) {
			strlcat(fModeReport, "resolution already switched; cold restart required for another mode\n", sizeof(fModeReport));
			return;
		}
		IOLockLock(fDsimLock);
		if (fDisplayOff || fOffPending || fFramesMasked) {
			/* (B, 2026-11-07: a switch while the display was off left the panel black after the wake) */
			IOLockUnlock(fDsimLock);
			strlcat(fModeReport, "the display is off: switch with the screen on\n", sizeof(fModeReport));
			return;
		}
		fModeAsk = (uint32_t) mode;
		ok = modeSetFhd120();
		IOLockUnlock(fDsimLock);
		strlcat(fModeReport, ok ? "switched (see the fb_mode lines in the log)\n" : "NOT switched (see the fb_mode lines)\n",
		    sizeof(fModeReport));
		modeCounters("after", fModeReport, sizeof(fModeReport));
		IOSleep(400);
		modeCounters("0.5 s later", fModeReport, sizeof(fModeReport));
	}
}

extern "C" void
dbg_fb_mode(const uint64_t * v, unsigned n, char * out, size_t cap)
{
	IOMobileFramebuffer * fb = gFbLive;
	AbsoluteTime deadline;

	if (fb == NULL) {
		snprintf(out, cap, "fb.mode: no framebuffer\n");
		return;
	}
	IOLockLock(fb->fSwapLock);
	fb->fModeDone = false;
	fb->fModeLpRef = n > 1 && v[1] != 0;
	fb->fModeLpVal = n > 2 ? (uint32_t) v[2] : 0;
	fb->fModeWant = n > 0 ? (int) v[0] : 0;
	IOLockWakeup(fb->fSwapLock, fb->fSwap, false);
	clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
	while (!fb->fModeDone) {
		if (IOLockSleepDeadline(fb->fSwapLock, (void *) (uintptr_t) &fb->fModeDone, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
			break;
		}
	}
	IOLockUnlock(fb->fSwapLock);
	snprintf(out, cap, "%s", fb->fModeDone ? fb->fModeReport : "fb.mode: the presenter did not answer in 5 s\n");
}

/* Samsung hubble1 POC flash: DT spi_20, gpm4 clock, gpm5 MISO,
 * gpm6 MOSI, gpm7 chip select. Hardware SPI handles the signal ordering.
 * Only JEDEC-ID and READ opcodes are implemented. All pins are restored.
 */
static struct redwood_exynos_spi calBus = {
	.name = "panel-cal-spi", .regs_pa = 0x15d20000, .usi_mode_pa = 0x15c22010,
	.cmu_pa = 0x15c00000, .cmu_size = 0x4000,
	.gates = { 0x2064, 0x2084, 0x2088 }, .ngates = 3,
	.qch = 0x3044, .div = 0x1820, .mux = 0x1020,
	.div_ratio = 15, .fifo_bytes = 64, .mode = 0,
};

/* One bounded, read-only flash transaction, with the DSIM lock held by caller. */
static uint32_t
calRead(bool id, uint32_t address, uint8_t *data, uint32_t length)
{
	volatile uint8_t *pins[4];
	uint32_t con[4], dat[4], savedClock[5], savedUsi, status;
	uint8_t tx[132] = { 0 }, rx[132] = { 0 };
	if (length == 0 || length > 128) return 1;
	for (unsigned i = 0; i < 4; i++) {
		pins[i] = redwood_gpio_bank(0x15c30000, 0x80 + 0x20 * i);
		if (pins[i] == NULL) return 2;
		con[i] = dpuRead(pins[i], 0);
		dat[i] = redwood_gpio_get(pins[i], 0);
	}
	if ((!calBus.regs || !calBus.usi || !calBus.cmu) && !redwood_exynos_spi_map(&calBus, redwood_dsay)) return 3;
	for (unsigned i = 0; i < 3; i++) savedClock[i] = dpuRead((volatile uint8_t *)calBus.cmu, calBus.gates[i]);
	savedClock[3] = dpuRead((volatile uint8_t *)calBus.cmu, calBus.div);
	savedClock[4] = dpuRead((volatile uint8_t *)calBus.cmu, calBus.qch);
	savedUsi = *(volatile uint32_t *)calBus.usi;
	redwood_exynos_spi_power(&calBus);
	redwood_exynos_spi_reset(&calBus);
	for (unsigned i = 0; i < 4; i++) redwood_gpio_func(pins[i], 0, 2);
	unsigned header = id ? 1 : 4;
	tx[0] = id ? 0x9f : 0x03;
	if (!id) { tx[1] = (uint8_t)(address >> 16); tx[2] = (uint8_t)(address >> 8); tx[3] = (uint8_t)address; }
	redwood_exynos_spi_select(&calBus, true);
	status = redwood_exynos_spi_transfer(&calBus, tx, rx, header + length);
	redwood_exynos_spi_select(&calBus, false);
	memcpy(data, rx + header, length);
	for (unsigned i = 0; i < 4; i++) {
		if ((con[i] & 15) == 1) redwood_gpio_set(pins[i], 0, dat[i]);
		redwood_gpio_func(pins[i], 0, con[i] & 15);
	}
	*(volatile uint32_t *)calBus.usi = savedUsi;
	dpuWrite((volatile uint8_t *)calBus.cmu, calBus.div, savedClock[3]);
	for (unsigned wait = 0; wait < 1000 && (dpuRead((volatile uint8_t *)calBus.cmu, calBus.div) & (1u << 16)); wait++) delay(1);
	dpuWrite((volatile uint8_t *)calBus.cmu, calBus.qch, savedClock[4]);
	for (unsigned i = 0; i < 3; i++) dpuWrite((volatile uint8_t *)calBus.cmu, calBus.gates[i], savedClock[i]);
	return status;
}

extern "C" void
dbg_fb_cal(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	IOMobileFramebuffer *fb = gFramebuffer;
	uint8_t data[128];
	uint32_t address = n ? (uint32_t)v[0] : 0, length = n > 1 ? (uint32_t)v[1] : 3;
	if (fb == NULL || !fb->fDsimLock || (n && (n != 2 || !length || length > 128 ||
	    v[0] != address || v[1] != length || address < 0xf0000 || address >= 0xf9000 || length > 0xf9000 - address))) {
		snprintf(out, cap, "fb.cal: ID, or factory address 0xf0000..0xf8fff and 1..128 bytes\n");
		return;
	}
	IOLockLock(fb->fDsimLock);
	uint32_t status = calRead(n == 0, address, data, length);
	IOLockUnlock(fb->fDsimLock);
	if (status) { snprintf(out, cap, "fb.cal: transfer failed %#x\n", status); return; }
	size_t at = (size_t)snprintf(out, cap, "%s %#x %u:", n ? "flash" : "jedec", address, length);
	for (unsigned i = 0; i < length && at < cap; i++) at += (size_t)snprintf(out + at, cap - at, " %02x", data[i]);
	if (at < cap) snprintf(out + at, cap - at, "\n");
}

static bool
factoryLoad(IOMobileFramebuffer *fb)
{
	if (gFactoryReady) return true;
	uint8_t id[3], check[3], gamma[34];
	static const uint8_t keyOn[] = { 0xF0, 0x5A, 0x5A }, keyOff[] = { 0xF0, 0xA5, 0xA5 };
	if (calRead(true, 0, id, 3) || id[0] != 0xc2 || id[1] != 0x28 || id[2] != 0x14) {
		fbSay("fb: factory: unsupported or missing SPI flash; approximate curve retained\n");
		return false;
	}
	for (unsigned part = 0; part < 2; part++) {
		uint32_t base = part ? 0xf3000 : 0xf0000, sum = 0;
		for (unsigned off = 0; off < 7392; off += 128) {
			unsigned bytes = 7392 - off < 128 ? 7392 - off : 128;
			if (calRead(false, base + off, gFactoryData[part] + off, bytes)) return false;
		}
		if (calRead(false, base + 0x26b4, check, 3)) return false;
		for (unsigned i = 0; i < 7392; i++) sum += gFactoryData[part][i];
		uint16_t expected = (uint16_t)((check[0] << 8) | check[1]);
		if ((sum & 0xffff) != expected || check[2] != 1) {
			fbSay("fb: factory: %s checksum %#x != %#x or magic %u; approximate curve retained\n",
			    part ? "120 HS" : "60 NS", sum & 0xffff, expected, check[2]);
			return false;
		}
		for (unsigned i = 0; i < 90; i++) {
			uint32_t aor = (gFactoryData[part][2550 + 2 * i] << 8) | gFactoryData[part][2551 + 2 * i];
			if (!aor || aor >= 0xc80) return false;
		}
		gFactoryChecksums[part] = expected;
	}
	(void)fb->dsimWrite(keyOn, sizeof(keyOn));
	int got = fb->dsimRead(0xCA, gamma, 34);
	(void)fb->dsimWrite(keyOff, sizeof(keyOff));
	if (got != 34) {
		fbSay("fb: factory: cannot save original gamma (%d/34); approximate curve retained\n", got);
		return false;
	}
	memcpy(gFactorySavedGamma, gamma, 34);
	gFactoryReady = true;
	fbSay("fb: factory: normal checksum %#x, 120 HS %#x verified; original gamma saved\n",
	    gFactoryChecksums[0], gFactoryChecksums[1]);
	return true;
}

static void
factoryParameters(uint32_t level, uint32_t *aor, uint8_t *gamma)
{
	const uint8_t *data = gFactoryData[refreshHz() == 120 ? 1 : 0];
	unsigned lo = 0, hi = 0;
	uint32_t weight = 0;
	if (level >= ((uint32_t)gPanelNits[73] << 16)) lo = hi = 73;
	else if (level > ((uint32_t)gPanelNits[0] << 16)) {
		for (hi = 1; hi < 74 && level > ((uint32_t)gPanelNits[hi] << 16); hi++) {}
		lo = hi - 1;
		weight = (uint32_t)(((uint64_t)(level - ((uint32_t)gPanelNits[lo] << 16)) << 16) /
		    ((uint32_t)(gPanelNits[hi] - gPanelNits[lo]) << 16));
	}
	/* Gamma has 74 reversed rows; AOR has 90 (including 16 HBM rows). */
	fbCalGamma(data + 34 + fbCalRow(74, lo) * 34, data + 34 + fbCalRow(74, hi) * 34, weight, gamma);
	uint32_t a = (data[2550 + fbCalRow(90, lo) * 2] << 8) | data[2551 + fbCalRow(90, lo) * 2];
	uint32_t b = (data[2550 + fbCalRow(90, hi) * 2] << 8) | data[2551 + fbCalRow(90, hi) * 2];
	*aor = fbCalLerp(a, b, weight);
}

/* Optional software True Tone approximation: a static DQE colour cube.
 * Registers/layout: Samsung dpu30/cal_9830/regs-dqe.h and dqe_drv.c's
 * bypass_cgc_tune / dqe_cgc_lut_set().
 * It runs in the display pipeline, not a CPU/GPU image filter. Save every
 * touched register; OFF restores the exact baseline. No panel gamma changes. */
static bool gWarmSaved;
static uint32_t gWarmControl, gWarmPath, gWarmSize, gWarmWidth, gWarmHeight, gWarmCgc[72];
static void
warmApply(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	IOMobileFramebuffer *fb = gFramebuffer;
	if (fb == NULL || fb->fDecon == NULL || n > 1 || (n && v[0] > 1)) {
		snprintf(out, cap, "fb.warm [0|1]\n"); return;
	}
	if (n) {
		IOLockLock(fb->fDsimLock);
		if (fb->fDisplayOff || fb->fOffPending || fb->fModeWant >= 0) {
			IOLockUnlock(fb->fDsimLock);
			snprintf(out, cap, "warm deferred: display off or mode changing\n"); return;
		}
		/* A global update from the presenter can otherwise latch the DQE LUT
		 * halfway through these writes. Mask TE and finish the current
		 * scan before touching either the LUT or its routing/control bits. */
		uint32_t trigger = dpuRead(fb->fDecon, DECON_TRIG_CONTROL);
		dpuWrite(fb->fDecon, DECON_TRIG_CONTROL, trigger | DECON_TRIG_MASK);
		uint64_t began = mach_absolute_time(), limit;
		nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &limit);
		while (!(dpuRead(fb->fDecon, DECON_GLOBAL_CONTROL) & DECON_GLOBAL_IDLE) &&
		    mach_absolute_time() - began < limit) delay(50);
		if (!(dpuRead(fb->fDecon, DECON_GLOBAL_CONTROL) & DECON_GLOBAL_IDLE)) {
			dpuWrite(fb->fDecon, DECON_TRIG_CONTROL, trigger);
			IOLockUnlock(fb->fDsimLock);
			snprintf(out, cap, "warm deferred: frame still scanning\n"); return;
		}
		if (v[0] && !gWarmOn) {
			gWarmControl = dpuRead(fb->fDecon, 0x2000);
			gWarmSize = dpuRead(fb->fDecon, 0x2004);
			gWarmPath = dpuRead(fb->fDecon, 0x0230);
			gWarmWidth = fb->fPanelWidth; gWarmHeight = fb->fPanelHeight;
			for (unsigned i = 0; i < 72; i++) gWarmCgc[i] = dpuRead(fb->fDecon, 0x2010 + 4 * i);
			gWarmSaved = true;
			const uint32_t gains[] = { 1000, 980, 940 };
			bool existing = (gWarmPath & (7u << 12)) && (gWarmControl & 3);
			if (!existing) {
				/* Samsung's bypass settings: gamma encode/decode off,
				 * grey adjustment bypassed, 3x3x3 identity colour cube. */
				for (unsigned i = 0; i < 30; i++)
					dpuWrite(fb->fDecon, 0x2010 + 4 * i, i == 0 ? 0x3000 : i == 1 ? 0x10 : 0);
			}
			for (unsigned c = 0; c < 3; c++) for (unsigned i = 0; i < 14; i++) {
				uint32_t original = gWarmCgc[30 + c * 14 + i], packed = 0;
				for (unsigned half = 0; half < 2; half++) {
					unsigned node = i * 2 + half;
					uint32_t value = 0;
					if (node < 27) {
						if (existing) value = (original >> (16 * half)) & 0x1fff;
						else {
							unsigned coordinate = c == 0 ? node / 9 : c == 1 ? (node / 3) % 3 : node % 3;
							value = coordinate == 2 ? 1023 : coordinate * 512;
						}
						value = value * gains[c] / 1000;
					}
					packed |= value << (16 * half);
				}
				dpuWrite(fb->fDecon, 0x2088 + c * 0x38 + i * 4, packed);
			}
			dpuWrite(fb->fDecon, 0x2004, (fb->fPanelHeight << 16) | fb->fPanelWidth);
			dpuWrite(fb->fDecon, 0x2000, existing ? (gWarmControl | 3) : 3);
			dpuWrite(fb->fDecon, 0x0230, (gWarmPath & ~(7u << 12)) | (2u << 12));
			gWarmOn = true;
		} else if (!v[0] && gWarmOn && gWarmSaved) {
			for (unsigned i = 0; i < 72; i++) dpuWrite(fb->fDecon, 0x2010 + 4 * i, gWarmCgc[i]);
			uint32_t size = gWarmSize;
			if ((gWarmPath & (7u << 12)) && (gWarmWidth != fb->fPanelWidth || gWarmHeight != fb->fPanelHeight))
				size = (fb->fPanelHeight << 16) | fb->fPanelWidth;
			dpuWrite(fb->fDecon, 0x2004, size);
			dpuWrite(fb->fDecon, 0x2000, gWarmControl);
			dpuWrite(fb->fDecon, 0x0230, gWarmPath);
			gWarmOn = false;
		}
		dpuWrite(fb->fDecon, DECON_SHADOW_UPDATE,
		    dpuRead(fb->fDecon, DECON_SHADOW_UPDATE) | (1u << 31) | (1u << 28));
		__asm__ volatile ("dsb sy" ::: "memory");
		dpuWrite(fb->fDecon, DECON_TRIG_CONTROL, trigger);
		IOLockUnlock(fb->fDsimLock);
	}
	snprintf(out, cap, "warm %s (software approximation; R 100%% G 98%% B 94%%)\n", gWarmOn ? "on" : "off");
}

extern "C" void
dbg_fb_warm(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	IOMobileFramebuffer *fb = gFramebuffer;
	if (!n || !fb || n > 1 || v[0] > 1) { warmApply(v, n, out, cap); return; }
	/* Run between presentations: show()/teWait() also manipulates the trigger
	 * mask, so fDsimLock alone cannot make a DQE update atomic. */
	IOLockLock(fb->fSwapLock);
	fb->fWarmWant = (int)v[0]; fb->fWarmDone = false;
	IOLockWakeup(fb->fSwapLock, fb->fSwap, false);
	uint64_t deadline;
	clock_interval_to_deadline(2, NSEC_PER_SEC, &deadline);
	while (!fb->fWarmDone) {
		if (IOLockSleepDeadline(fb->fSwapLock, &fb->fWarmDone, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) break;
	}
	snprintf(out, cap, "%s", fb->fWarmDone ? fb->fWarmReport : "warm deferred: presenter busy\n");
	IOLockUnlock(fb->fSwapLock);
}

extern "C" void
dbg_fb_factory(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	IOMobileFramebuffer *fb = gFramebuffer;
	if (fb == NULL || fb->fDsimLock == NULL || n > 1 || (n && v[0] > 1)) {
		snprintf(out, cap, "fb.factory [0|1]: status, disable/restore, load/enable\n");
		return;
	}
	IOLockLock(fb->fDsimLock);
	if (n) {
		if (v[0] && factoryLoad(fb)) { gFactoryEnabled = true; gFactoryRestore = false; }
		else if (v[0] == 0 && gFactoryEnabled) { gFactoryEnabled = false; gFactoryRestore = true; }
		fb->fBLValid = false;
	}
	snprintf(out, cap, "factory %s; loaded %u; checksums normal %#x / 120 HS %#x; target %u.%03u nominal nits; "
	    "AOD %u, held %u.%03u nits (from %u.%03u)\n",
	    gFactoryEnabled ? "enabled" : "disabled", gFactoryReady, gFactoryChecksums[0], gFactoryChecksums[1],
	    fb->fBLTarget >> 16, (unsigned)(((uint64_t)(fb->fBLTarget & 0xffff) * 1000) >> 16), gAODDim,
	    gAODLevel >> 16, (unsigned)(((uint64_t)(gAODLevel & 0xffff) * 1000) >> 16),
	    gAODBase >> 16, (unsigned)(((uint64_t)(gAODBase & 0xffff) * 1000) >> 16));
	IOLockUnlock(fb->fDsimLock);
	if (n && fb->fBLCall != NULL) thread_call_enter(fb->fBLCall);
}

/* fb.aod [PCT]: the AOD level as PCT (10..60, steps of 5) % of the slider position, live; then re-taken at once */
extern "C" void
dbg_fb_aod(const uint64_t * v, unsigned n, char * out, size_t cap)
{
	IOMobileFramebuffer *fb = gFramebuffer;

	if (n > 0 && v[0] >= 10 && v[0] <= 60) {
		gAODPctLive = (uint32_t) v[0];
		if (fb != NULL && fb->fBLLock != NULL && fb->fBLCall != NULL) {
			IOLockLock(fb->fBLLock);
			if (gAODBase != 0) {
				gAODLevel = aodFrom(gAODBase);
			}
			IOLockUnlock(fb->fBLLock);
			thread_call_enter(fb->fBLCall);
		}
	}
	snprintf(out, cap, "fb.aod: %u %% of the slider (boot %u); AOD %u, held %u.%03u nits from %u.%03u\n",
	    gAODPctLive ? gAODPctLive : gAODPct, gAODPct, gAODDim, gAODLevel >> 16,
	    (unsigned)(((uint64_t)(gAODLevel & 0xffff) * 1000) >> 16), gAODBase >> 16,
	    (unsigned)(((uint64_t)(gAODBase & 0xffff) * 1000) >> 16));
}

extern "C" void
dbg_fb_fade(const uint64_t * v, unsigned n, char * out, size_t cap)
{
	if (n > 0) {
		gFadeDD = v[0] != 0;
	}
	if (n > 1) {
		gOffFadeMs = v[1] > 2000 ? 2000 : (uint32_t) v[1];
	}
	if (n > 2) {
		gOffAlpha = v[2] != 0;
	}
	if (n > 3) {
		gOnFadeMs = v[3] > 2000 ? 2000 : (uint32_t) v[3];
	}
	snprintf(out, cap, "fb.fade: panel dimming during fades %s, power-off fade %s, on the %s\n", gFadeDD ? "on" : "off",
	    gOffFadeMs ? "stretched" : "as iOS asks (185 ms)", gOffAlpha ? "picture (window alpha)" : "backlight");
	if (gOffFadeMs) {
		size_t at = strlen(out);

		snprintf(out + at, cap - at, "  (at least %u ms)\n", gOffFadeMs);
	}
}

/*
 * The S20+'s (y2s, Exynos 990) loudspeakers: what iOS plays (/dev/redwood-audio, redwood_audio.c) through the
 * SoC's audio block (ABOX) to the two Cirrus Logic CS35L41 amplifiers. Boot-arg snd=1.
 *
 * Android drives ABOX through its DSP (the "Calliope" Cortex-A32, which Samsung's firmware runs): the AP's driver
 * sends it the stream's buffer by IPC and the firmware programs the DMA. Here the DSP stays in reset and the AP
 * programs the hardware itself, with the same registers Samsung's kernel names (sound/soc/samsung/abox:
 * abox_soc_3.h, abox_if.c, abox_cmpnt.c, abox_dma.c, which writes these DMA registers itself for its dump DMA):
 *
 *   power       pd-aud: PMU AUD_CONFIGURATION 0x1900 bit 0, AUD_STATUS 0x1904 (flexpmu_cal_local_exynos9830.h
 *               aud_on), then the DTZPC restore of exynos-pd for need_smc 0x18e10204, then the audio pads out of
 *               retention (PMU 0x1920 bit 11, abox.c ABOX_PAD_NORMAL)
 *   clocks      CMU_AUD 0x18c00000: PLL_AUD0 1179.648 MHz (M 181 P 4 S 0 K 31740, pll_aud0_rate_table);
 *               AUDIF /16 = 73.728 MHz (abox.c: 384000 * 96 * 2 for uaif_max_div 512); UAIF2's bit clock
 *               AUDIF /24 = 3.072 MHz = 48 kHz * 2 slots * 32 bits; the bus /6
 *   SysMMU      0x18e50000 (ABOX's): left off, so that the DMA's addresses are physical (logged)
 *   the path    RDMA3 (16-bit stereo from our buffer) -> SPUS chain 3 (no ASRC) -> SIFS0, the mixer (32-bit
 *               stereo) -> UAIF2 (ROUTE_CTRL0) -> pins gpb1-0..3 (aud-i2s2-bus): DSP_A, NB_NF, ABOX the master,
 *               2 slots of 32 bits. Each DMA and the mixer need a slice of ABOX's staging SRAM (SBANK), which
 *               the firmware hands out: given here (RDMA3 at 0, the mixer at 512, 512 bytes each)
 *   amplifiers  two CS35L41 on a bit-banged I2C bus (gpp4-2 SCL, gpp4-3 SDA): 0x41 the earpiece/top speaker
 *               (left, slot 0, reset gpb2-4), 0x40 the bottom speaker (right, slot 1, reset gpb2-5). Set up as
 *               Cirrus's driver does (cs35l41.c: cs35l41_init, otp_unpack, apply_pdata for this phone's device
 *               tree, set_dai_fmt, hw_params, set_sysclk from SCLK, main_amp_event) with the PCM straight to the
 *               DAC (ASPRX1 -> DAC_PCM1_SRC): the amplifiers' own DSP (Cirrus's speaker protection) is not loaded;
 *               the samples go through redwood_guard.c instead, and the amplifiers are kept under the chip's
 *               full level (snd_below, eighths of a dB; default 18 dB) with iOS's volume on top, in their
 *               digital volume
 *
 * One thread does everything after the bring-up: keeps the DMA buffer filled ahead of where the DMA reads (from
 * what iOS wrote, else silence, through the guard), powers the amplifiers up when there is sound and down after
 * three seconds without, follows iOS's volume, and looks at the amplifiers' fault bits twice a second (a fault:
 * both off for the rest of the run).
 *
 * Boot-args: snd=1, snd_below=N (amplifiers N/8 dB under the chip's full level), snd_gain=N (the guard's gain,
 * eighths of a dB, at most 12 dB), snd_tone=MS (a 1 kHz tone at -20 dBFS for MS ms once the amplifiers are up: a
 * self-test without iOS).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_bbi2c.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <vm/memory_types.h>
#include <kern/kalloc.h>
#include <sys/kauth.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/redwood_dbgport.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);
extern void IOSleep(unsigned int milliseconds);
extern void loader_log_line(const char *line);
extern boolean_t ml_get_interrupts_enabled(void);
extern int redwood_wifi_dma_pool(uint64_t *phys, vm_offset_t *virt, size_t *size);
extern int redwood_spmic_read(uint8_t type, uint8_t reg, uint8_t *value);
extern int redwood_pmic_read(uint8_t type, uint8_t reg, uint8_t *value);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern int redwood_pmic_write(uint8_t type, uint8_t reg, uint8_t value);    /* (redwood_acpm_pmic.c: the sub-PMIC) */
extern int redwood_spmic_write(uint8_t type, uint8_t reg, uint8_t value);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);   /* (redwood_image.c) */

/* ---- addresses ------------------------------------------------------------------------------------------------ */

#define PMU_PHYS                0x15860000U
#define PMU_AUD_CONFIGURATION   0x1900U
#define PMU_AUD_STATUS          0x1904U
#define PMU_ABOX_PAD            0x1920U
#define PMU_ABOX_PAD_NORMAL     (1U << 11)
#define AUD_NEED_SMC            0x18e10204U
#define SMC_PREPARE_PD_ONOFF    0x82000410U

#define CMU_AUD_PHYS            0x18c00000U
#define PLL_LOCKTIME_AUD0       0x0000U
#define PLL_CON0_AUD0           0x0100U         /* bit 4: PLL (1) or OSC; bit 7 busy */
#define PLL_CON3_AUD0           0x010cU         /* ENABLE 31, STABLE 29, M 25:16, P 13:8, S 2:0 */
#define PLL_CON5_AUD0           0x0114U         /* K 15:0 */
#define MUX_UAIF2               0x1014U
#define DIV_AUDIF               0x1800U
#define MUX_AUD_CNT             0x1000U         /* SELECT bit 0: 0 DIV_CLK_AUD_AUDIF, 1 PLL_AUD1 (exynos9830 cmucal) */
#define DIV_AUD_CNT             0x180cU         /* DIVRATIO 8:0: the CNT clock, which paces the SIFS (abox "cnt") */
#define DIV_BUS                 0x1804U
#define DIV_BUSP                0x1808U
#define DIV_SCLK                0x1828U
#define DIV_UAIF2               0x1834U
#define CLK_BUSY                (1U << 16)
/* gates (cmucal-sfr.c, exynos9830): MANUAL 20 -> CG_VAL 21 decides; else ENABLE_AUTOMATIC_CLKGATING 28 (1: gated when idle) */
#define GATE_CG_VAL             (1U << 21)
#define GATE_MANUAL             (1U << 20)
#define GATE_AUTO               (1U << 28)
/* Q-channels: ENABLE 0 (1: the IP's handshake may stop the clock); with it 0, CLOCK_REQ 1 keeps the clock asked for */
#define QCH_ENABLE              (1U << 0)
#define QCH_CLOCK_REQ           (1U << 1)

#define SYSMMU_PHYS             0x18e50000U
#define MMU_CTRL                0x0000U
#define MMU_CFG                 0x0004U
#define MMU_STATUS              0x0008U
#define MMU_VERSION             0x0034U
#define MMU_CTRL_VM             0x8000U

#define ABOX_PHYS               0x18c50000U
#define ABOX_IP_INDEX           0x0000U
#define ABOX_VERSION            0x0004U
#define ABOX_SYSPOWER_CTRL      0x0010U
#define ABOX_SYSPOWER_STATUS    0x0014U
#define ABOX_DYN_CLOCK_OFF      0x0030U
#define ABOX_QCHANNEL_DISABLE   0x0038U         /* one bit per clock (abox.h enum qchannel): 1 ACLK, 2 + n BCLK_UAIFn */
#define QCH_ABOX_ACLK           1
#define QCH_ABOX_BCLK_UAIF(n)   (2 + (n))
#define ABOX_ROUTE_CTRL0        0x0040U         /* UAIFx's speaker source, 4 bits each: SIFSn + 1 */
#define ABOX_SPUS_CTRL_FC0      0x0200U         /* chains 0..3, 8 bits each: IN 6:5 (0 RDMA), OUT 4:1 (1 SIFS0), ASRC 0 */
#define ABOX_SPUS_CTRL1         0x0204U         /* MIXP_FORMAT 4:0 */
#define ABOX_SPUS_SBANK_RDMA(x) (0x0220U + 4 * (x))     /* SIZE 29:20, STR 13:4 */
#define ABOX_SPUS_SBANK_MIXP    0x0280U
#define ABOX_SIFS_CNT0          0x0290U
#define UAIF_ID                 2
#define ABOX_UAIF_CTRL0         (0x0500U + 0x10 * UAIF_ID)     /* MODE 2 (1 master), SPK_ENABLE 0 */
#define ABOX_UAIF_CTRL1         (0x0504U + 0x10 * UAIF_ID)
#define ABOX_UAIF_STATUS        (0x050cU + 0x10 * UAIF_ID)
static uint32_t snd_rdma = 3;           /* RDMA3 for the bypass; Calliope's firmware has a PCM task for RDMA0 only (run 6) */
#define RDMA_ID                 (snd_rdma)
#define RDMA(off)               (0x1000U + 0x100 * RDMA_ID + (off))
#define RDMA_CTRL0              0x00U           /* BURST_LEN 22:19, FUNC 18:16, FORMAT 12:8, ENABLE 0 */
#define RDMA_BUF_STR            0x08U
#define RDMA_BUF_END            0x0cU
#define RDMA_BUF_OFFSET         0x10U
#define RDMA_STR_POINT          0x14U
#define RDMA_VOL_FACTOR         0x18U
#define RDMA_STATUS             0x30U           /* PROGRESS 31, RBUF_OFFSET 27:12 (16 bytes), RBUF_CNT 11:0 */

#define ABOX_FORMAT(width, channels) (((channels) - 1) | (((width) == 16 ? 1U : (width) == 24 ? 2U : 3U) << 3))

#define PINCTRL_PERIC1_GPB1     0x100U
#define PINCTRL_PERIC1_GPB2     0x120U
#define PINCTRL_PERIC0_GPP4     0x080U

/* ---- CS35L41 (cs35l41-registers.h) --------------------------------------------------------------------------- */

#define L41_DEVID               0x00000000U
#define L41_REVID               0x00000004U
#define L41_OTPID               0x00000010U
#define L41_TEST_KEY_CTL        0x00000040U
#define L41_OTP_MEM0            0x00000400U
#define L41_PWR_CTRL1           0x00002014U
#define L41_PWR_CTRL2           0x00002018U
#define L41_PLL_CLK_CTRL        0x00002c04U
#define L41_GLOBAL_CLK_CTRL     0x00002c0cU
#define L41_BSTCVRT_PEAK_CUR    0x00003808U
#define L41_SP_ENABLES          0x00004800U
#define L41_SP_RATE_CTRL        0x00004804U
#define L41_SP_FORMAT           0x00004808U
#define L41_SP_HIZ_CTRL         0x0000480cU
#define L41_SP_FRAME_RX_SLOT    0x00004820U
#define L41_SP_RX_WL            0x00004840U
#define L41_DAC_PCM1_SRC        0x00004c00U
#define L41_AMP_DIG_VOL_CTRL    0x00006000U     /* volume 13:3, 1/8 dB two's complement; 0x400 mute */
#define L41_AMP_GAIN_CTRL       0x00006c04U
#define L41_IRQ1_STATUS1        0x00010010U
#define L41_IRQ1_STATUS4        0x0001001cU
#define L41_IRQ1_MASK1          0x00010110U

#define L41_CHIP_ID             0x35a40U
#define L41R_CHIP_ID            0x35b40U
#define L41_OTP_BOOT_DONE       0x02U
#define L41_PUP_DONE            0x01000000U
#define L41_PDN_DONE            0x00800000U
#define L41_FAULTS              0x800281c0U     /* amp short, temperature, boost short / DCM UVP / OVP */
#define L41_VOL_MUTE            0x400U
#define L41_SCLK_3072000        0x21U           /* cs35l41_pll_sysclk */
#define L41_SCLK_6144000        0x28U           /* (snd_samsung: UAIF2 4 x 32 bit) */
#define L41_FS_48000            0x03U
#define L41_BST_IPK_4100MA      (((4100 - 1600) / 50) + 0x10)   /* cirrus,boost-peak-milliamp of the phone's tree */

/* cs35l41-tables.c otp_map_2 (OTP IDs 2 and 3); ID 1 differs only in element 93, which it skips */
static const struct { uint32_t reg; uint8_t shift, size; } otp_map[99] = {
	{0x02030,0,4}, {0x02030,7,1}, {0x0208c,24,6}, {0x02090,14,4}, {0x02090,10,4}, {0x0300c,11,4}, {0x0394c,23,2}, {0x03950,0,7},
	{0x03950,8,7}, {0x03950,16,8}, {0x03950,24,8}, {0x03954,0,7}, {0x03954,8,7}, {0x03954,16,8}, {0x03954,24,8}, {0x03958,0,7},
	{0x03958,8,7}, {0x03958,16,8}, {0x03958,24,8}, {0x0395c,0,7}, {0x0395c,8,7}, {0x0395c,16,8}, {0x0395c,24,8}, {0x0416c,0,8},
	{0x04160,0,7}, {0x0416c,8,8}, {0x04160,16,10}, {0x0416c,16,12}, {0x0416c,28,1}, {0x04170,0,6}, {0x04170,6,1}, {0x04170,8,6},
	{0x04170,14,1}, {0x04170,16,9}, {0x04360,0,5}, {0x04360,6,9}, {0x04448,0,8}, {0x04448,8,8}, {0x04448,16,8}, {0x04448,24,8},
	{0x0444c,0,3}, {0x06e30,0,5}, {0x06e30,8,5}, {0x06e30,16,5}, {0x06e30,24,5}, {0x06e34,0,5}, {0x06e34,8,5}, {0x06e34,16,5},
	{0x06e34,24,5}, {0x06e38,0,5}, {0x06e38,8,5}, {0x06e38,16,5}, {0x06e38,24,5}, {0x06e3c,0,5}, {0x06e3c,8,5}, {0x06e3c,16,5},
	{0x06e3c,24,5}, {0x06e40,0,5}, {0x06e40,8,5}, {0x06e40,16,5}, {0x06e40,24,5}, {0x06e44,0,5}, {0x06e48,0,10}, {0x06e48,10,10},
	{0x06e48,20,10}, {0x06e4c,0,10}, {0x06e4c,10,10}, {0x06e4c,20,10}, {0x06e50,0,10}, {0x06e50,10,10}, {0x06e50,20,10}, {0x06e54,0,10},
	{0x06e54,10,10}, {0x06e54,20,10}, {0x06e58,0,10}, {0x06e58,10,10}, {0x06e58,20,10}, {0x06e5c,0,10}, {0x06e5c,10,10}, {0x06e5c,20,10},
	{0x06e60,0,10}, {0x06e60,10,10}, {0x06e60,20,10}, {0x06e64,0,10}, {0x07418,7,5}, {0x0741c,0,5}, {0x0741c,11,4}, {0x0741c,19,4},
	{0x07434,17,1}, {0x07434,18,7}, {0x07068,0,9}, {0x0410c,7,1}, {0x0400c,0,7}, {0x04000,11,1}, {0x17040,0,8}, {0x17040,8,8},
	{0x17040,16,8}, {0x17040,24,8}, {0x17044,0,24},
};
#define OTP_ID1_SKIP            93

/* ---- state ---------------------------------------------------------------------------------------------------- */

#define BUF_BYTES               32768U          /* the DMA's ring: 8192 frames, 171 ms */
#define BUF_FRAMES              (BUF_BYTES / 4)
#define PERIOD_BYTES            4096U
#define LEAD_FRAMES             2880U           /* written this far ahead of the DMA: 60 ms */
#define CHUNK                   1024U           /* the guard's most at once */
#define RING_FRAMES             16384U          /* iOS's samples waiting: 341 ms; a power of two */
#define SILENCE                 8
/*
 * The gate (B, 2026-11-07: "volume 0 is not fully silent"): iOS keeps our device's volume control at the top and
 * scales the samples itself, so its lowest step is a faint signal, which the guard's gain then lifts. A block from
 * iOS whose peak stays under snd_gate (of 32767; 64 = -54 dBFS) is played as silence and counts as quiet (the
 * amplifiers go off). snd_gate=N at boot, `snd.gate N` live.
 */
static uint32_t snd_gate = 128;         /* -48 dBFS */
static uint32_t gate_peak_max, gate_blocks, gate_closed;       /* since the last snd.gate: the loudest block, blocks, gated */
#define QUIET_OFF_US            3000000ull

static volatile uint8_t *pmu, *cmu, *mmu, *abox;
static volatile uint8_t *gpb1, *gpb2;
static uint64_t buf_phys;
static int16_t *buf;
static uint32_t snd_samsung = 1;            /* (abox_route: Samsung's playback register set; snd_samsung=0 the old one) */
static bool started, amps_ok, amps_on, faulted, use_status;
static volatile bool sound_ready;
static volatile uint32_t amp_recovery_requested;
static bool amp_start_retryable;
static uint32_t snd_below = 109, snd_gain = 6 * 8, snd_tone;     /* (the user, 2026-11-07 17:10: 70 % of the old top = 3.1 dB more: 84 -> 109) */
/* (B, 2026-11-07: 10.5 dB under full and a 6 dB lift: at the top the 12 dB lift drove the limiter into distortion) */
static uint32_t snd_calliope;           /* 1: boot Calliope only; 2: and play through it (stage 3) */
/* snd_debug=1: routine lines (amps on/off, stats, mic start/stop) and per-sample mic peaks; 0 (default): errors only */
static uint32_t snd_debug;
#define snd_dbg(...) do { if (snd_debug) { snd_log(__VA_ARGS__); } } while (0)
/* self-check (snd.health): repairs done and the last time both amplifiers answered */
static uint32_t heal_amp_fault, heal_amp_idle, heal_mic_stall, dma_stalls;
static volatile uint64_t amps_answered_us;

struct amp {
	const char *name;
	uint8_t addr;
	uint32_t reset_pin;
	uint32_t slot;
	bool present;
};
static struct amp amps[2] = {
	{ "left (top)", 0x41, 4, 0, false },
	{ "right (bottom)", 0x40, 5, 1, false },
};
static struct redwood_bbi2c amp_bus = { .name = "amp bus gpp4-2/3", .scl = 2, .sda = 3, .half_us = 3 };

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_head, ring_tail;
static volatile uint64_t last_sound_us;
/*
 * Quiet: the amplifiers off, no tone, no sound from iOS for QUIET_OFF_US (it keeps sending silence): the DMA's
 * whole ring is silence (it goes on reading it round and round) and the thread sleeps until iOS sends sound
 * (exynos_sink), a second at most, the silence meanwhile dropped -- instead
 * of feeding silence every 4 ms (some 1000 wake-ups a second on the big cores: the cpuidle wake record,
 * 27 October).
 */
static volatile bool snd_quiet;
static uint64_t quiet_spells, quiet_us;
static uint64_t ios_frames, fed_frames, silent_frames, underruns;

static const int16_t tone[48] = {
	0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3028, 3165, 3249, 3277, 3249, 3165, 3028, 2838, 2600, 2317, 1995, 1638,
	1254, 848, 428, 0, -428, -848, -1254, -1638, -1995, -2317, -2600, -2838, -3028, -3165, -3249, -3277, -3249, -3165, -3028,
	-2838, -2600, -2317, -1995, -1639, -1254, -848, -428,
};

static void snd_log(const char *fmt, ...) __printflike(1, 2);
static void
snd_log(const char *fmt, ...)
{
	char line[320];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (ml_get_interrupts_enabled()) {         /* (not from loader_return()'s quiesce with interrupts off) */
		printf("%s", line);
	}
	loader_log_line(line);
}

static inline uint32_t
rd(volatile uint8_t *base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(volatile uint8_t *base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * The speaker path's clocks through CMU_AUD's gates and Q-channels, opened as Samsung's cal-if does it
 * (ra.c: ra_set_gate pass, ra_req_enable_qch). Without the DSP's driver nothing asks for them, and an
 * automatically gated BCLK_UAIF2 is a UAIF that never pulls a sample: RDMA3 stayed "in progress" with 0
 * frames read and UAIF2's status 0 (PLANS/08-audio.md). Each register is said before and after.
 */
static void
snd_clocks_on(void)
{
	static const struct { uint32_t off; const char *name; } gates[] = {
		{ 0x2010, "ABOX ACLK" }, { 0x2018, "ABOX BCLK_CNT" }, { 0x2028, "ABOX BCLK_UAIF2" },
		{ 0x2048, "ABOX SCLK" }, { 0x2058, "LHM_AXI_P" }, { 0x205c, "LHS_AXI_D" },
		{ 0x2070, "RSTNSYNC BUSP" }, { 0x2074, "RSTNSYNC BUS" }, { 0x2078, "RSTNSYNC CNT" },
		{ 0x2094, "RSTNSYNC SCLK" }, { 0x20a0, "RSTNSYNC UAIF2" },
	};
	static const struct { uint32_t off; const char *name; } qchs[] = {
		{ 0x301c, "QCH ACLK" }, { 0x3028, "QCH BCLK2" }, { 0x3044, "QCH CNT" }, { 0x3048, "QCH SCLK" },
	};
	char line[600];
	size_t at = 0;

	at += snprintf(line + at, sizeof(line) - at, "snd: CMU_AUD gates before -> after:");
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		uint32_t was = rd(cmu, gates[i].off), now = was;

		if (was & GATE_MANUAL) {
			now |= GATE_CG_VAL;
		} else {
			now &= ~GATE_AUTO;
		}
		if (now != was) {
			wr(cmu, gates[i].off, now);
		}
		at += snprintf(line + at, sizeof(line) - at, " %s %#x->%#x;", gates[i].name, was, rd(cmu, gates[i].off));
	}
	snd_log("%s\n", line);
	at = 0;
	at += snprintf(line + at, sizeof(line) - at, "snd: CMU_AUD Q-channels before -> after:");
	for (unsigned i = 0; i < sizeof(qchs) / sizeof(qchs[0]); i++) {
		uint32_t was = rd(cmu, qchs[i].off);

		wr(cmu, qchs[i].off, (was & ~QCH_ENABLE) | QCH_CLOCK_REQ);
		at += snprintf(line + at, sizeof(line) - at, " %s %#x->%#x;", qchs[i].name, was, rd(cmu, qchs[i].off));
	}
	snd_log("%s\n", line);
}

static inline void
upd(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t value)
{
	wr(base, off, (rd(base, off) & ~mask) | (value & mask));
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

/*
 * kern.redwood.snd_quiet_ms: how long the speakers have had nothing to play (ms; -1 never since boot). backboardd's
 * idle timer (userland inject.c) does not turn the screen off while sound plays (B, 2026-11-07).
 */
static int
sysctl_snd_quiet_ms SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	uint64_t heard = last_sound_us;
	int ms = heard == 0 ? -1 : (int)((now_us() - heard) / 1000 > 0x7fffffff ? 0x7fffffff : (now_us() - heard) / 1000);

	return SYSCTL_OUT(req, &ms, sizeof(ms));
}
SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, snd_quiet_ms, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    sysctl_snd_quiet_ms, "I", "ms since the speakers last had sound (-1: never)");

static bool
wait_bits(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t i = 0; i < us; i += 10) {
		if ((rd(base, off) & mask) == want) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return (rd(base, off) & mask) == want;
}

static uint64_t
smc4(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/*
 * Samsung's SFR access through EL3 (include/linux/smc.h: SMC_CMD_REG -101, SFR_W class 1 << 30, SFR_R 3 << 30, the
 * address >> 2; arch/arm64/kernel/exynos-smc.S). ABOX's GIC on the Exynos 990 past rev 0 is secure (abox_util.c
 * is_secure_gic): abox_gic.c writes its distributor and CPU interface this way; plain writes are dropped.
 */
#define SMC_CMD_REG             0xFFFFFFFFFFFFFF9BULL
static uint64_t
sfr_smc_write(uint32_t addr, uint32_t value)
{
	return smc4(SMC_CMD_REG, (1U << 30) | (addr >> 2), value, 0);
}

static bool
sfr_smc_read(uint32_t addr, uint32_t *value)
{
	register uint64_t x0 __asm__("x0") = SMC_CMD_REG & 0xffffffffULL;
	register uint64_t x1 __asm__("x1") = (3U << 30) | (addr >> 2);
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	*value = (uint32_t)x2;
	return x0 == 0;
}

/* ---- the amplifiers ------------------------------------------------------------------------------------------- */

static bool
amp_write(struct amp *a, uint32_t reg, uint32_t value)
{
	uint8_t b[8] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg,
		         (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };

	return redwood_bbi2c_xfer(&amp_bus, a->addr, b, sizeof(b), NULL, 0);
}

static bool
amp_read(struct amp *a, uint32_t reg, uint32_t *value)
{
	uint8_t r[4] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg }, v[4] = { 0 };
	bool ok = redwood_bbi2c_xfer(&amp_bus, a->addr, r, sizeof(r), v, sizeof(v));

	*value = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
	return ok;
}

static bool
amp_update(struct amp *a, uint32_t reg, uint32_t mask, uint32_t value)
{
	uint32_t old;

	return amp_read(a, reg, &old) && amp_write(a, reg, (old & ~mask) | (value & mask));
}

static bool
amp_unlock(struct amp *a, bool unlock)
{
	return unlock ? amp_write(a, L41_TEST_KEY_CTL, 0x5555) && amp_write(a, L41_TEST_KEY_CTL, 0xaaaa) :
	       amp_write(a, L41_TEST_KEY_CTL, 0xcccc) && amp_write(a, L41_TEST_KEY_CTL, 0x3333);
}

/* cs35l41_otp_unpack: the chip's trims, from its OTP into the registers they belong in */
static bool
amp_otp_unpack(struct amp *a)
{
	uint32_t mem[32], id = 0, bit = 16, word = 2;

	if (!amp_unlock(a, true) || !amp_read(a, L41_OTPID, &id)) {
		return false;
	}
	for (unsigned i = 0; i < 32; i++) {
		if (!amp_read(a, L41_OTP_MEM0 + 4 * i, &mem[i])) {
			return false;
		}
	}
	id &= 0xf;
	if ((mem[1] & 0xfffffffe) != 0x1500 || (mem[2] & 0xffff) != 0x300 || id < 1 || id > 3) {
		snd_log("snd: amp %s: OTP header %#x %#x, id %u: not unpacked\n", a->name, mem[1], mem[2], id);
		amp_unlock(a, false);
		return false;
	}
	for (unsigned i = 0; i < 99; i++) {
		uint32_t size = otp_map[i].size, value;

		if (bit + size - 1 >= 32) {
			value = (mem[word] >> bit) & ((1U << (32 - bit)) - 1);
			word++;
			value |= (mem[word] & ((1U << (bit + size - 32)) - 1)) << (32 - bit);
			bit += size - 32;
		} else {
			value = (mem[word] >> bit) & (size == 32 ? ~0U : (1U << size) - 1);
			bit += size;
		}
		if (bit == 32) {
			bit = 0;
			word++;
		}
		if (otp_map[i].reg != 0 && !(id == 1 && i == OTP_ID1_SKIP)) {
			uint32_t mask = (size == 32 ? ~0U : (1U << size) - 1) << otp_map[i].shift;

			if (!amp_update(a, otp_map[i].reg, mask, value << otp_map[i].shift)) {
				amp_unlock(a, false);
				return false;
			}
		}
	}
	return amp_unlock(a, false);
}

static const char *
amp_bad(struct amp *a, const char *what)
{
	snd_log("snd: amp %s (%#x): %s\n", a->name, a->addr, what);
	a->present = false;
	return NULL;
}

/* reset, identified, trimmed and set up for our stream; powered down (GLOBAL_EN 0) and muted */
static bool
amp_prepare(struct amp *a)
{
	uint32_t devid = 0, revid = 0, status = 0, fmt = 0, slot = 0, vol = 0, gain = 0, pwr2 = 0, pll = 0;
	unsigned i;

	if (!amp_read(a, L41_DEVID, &devid) || !amp_read(a, L41_REVID, &revid)) {
		amp_bad(a, "does not answer");
		return false;
	}
	if (devid != ((revid & 0xf) % 2 ? L41R_CHIP_ID : L41_CHIP_ID)) {
		snd_log("snd: amp %s: device ID %#x, revision %#x: not a CS35L41\n", a->name, devid, revid);
		a->present = false;
		return false;
	}
	amp_write(a, L41_IRQ1_MASK1, 0x7ffcfe3f);
	if (revid == 0xa0) {
		amp_unlock(a, true);
		amp_write(a, 0x3854, 0x05180240);
		amp_write(a, 0x7418, 0x9091a1c8);
		amp_write(a, 0x3014, 0x0200ee0e);
		amp_write(a, 0x381c, 0x00000051);
		amp_write(a, 0x0054, 0x00000004);
		amp_write(a, 0x10318, 0);
		amp_write(a, 0x10b18, 0);
		amp_unlock(a, false);
	}
	for (i = 0; i < 100; i++) {
		if (amp_read(a, L41_IRQ1_STATUS4, &status) && (status & L41_OTP_BOOT_DONE)) {
			break;
		}
		IOSleep(1);
	}
	if (!(status & L41_OTP_BOOT_DONE)) {
		amp_bad(a, "OTP_BOOT_DONE never came");
		return false;
	}
	if (!amp_otp_unpack(a)) {
		amp_bad(a, "the OTP trims could not be unpacked");
		return false;
	}
	/* the phone's tree (apply_pdata): ASP SDOUT hi-z when unused, the boost's peak current */
	amp_update(a, L41_SP_HIZ_CTRL, 0x3, 0x3);
	amp_update(a, L41_BSTCVRT_PEAK_CUR, 0x7f, L41_BST_IPK_4100MA);
	/* set_dai_fmt DSP_A | NB_NF | CBS_CFS: slave, ASP_FMT 0, no inversions; hw_params: 48 kHz, slots of 32, 24 bits */
	amp_update(a, L41_SP_FORMAT, 0xff000000 | 0x0700 | 0x40 | 0x10 | 0x04 | 0x01, 32U << 24);
	amp_update(a, L41_SP_RX_WL, 0x3f, 24);
	amp_update(a, L41_SP_FRAME_RX_SLOT, 0x3f3f, a->slot | (a->slot ^ 1) << 8);
	amp_update(a, L41_GLOBAL_CLK_CTRL, 0x1f, L41_FS_48000);
	/* set_sysclk from SCLK at 3.072 MHz */
	amp_update(a, L41_SP_RATE_CTRL, 0x3f, snd_samsung ? L41_SCLK_6144000 : L41_SCLK_3072000);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0800, 0x0800);
	amp_update(a, L41_PLL_CLK_CTRL, 0x07e0, (snd_samsung ? L41_SCLK_6144000 : L41_SCLK_3072000) << 5);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0010, 0);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0007, 0);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0800, 0);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0010, 0x0010);
	/* the PCM to the DAC, muted; ASPRX1 on */
	amp_write(a, L41_DAC_PCM1_SRC, 0x08);
	amp_update(a, L41_AMP_DIG_VOL_CTRL, 0x7ff << 3, L41_VOL_MUTE << 3);
	amp_update(a, L41_SP_ENABLES, 1U << 16, 1U << 16);

	amp_read(a, L41_SP_FORMAT, &fmt);
	amp_read(a, L41_SP_FRAME_RX_SLOT, &slot);
	amp_read(a, L41_AMP_DIG_VOL_CTRL, &vol);
	amp_read(a, L41_AMP_GAIN_CTRL, &gain);
	amp_read(a, L41_PWR_CTRL2, &pwr2);
	amp_read(a, L41_PLL_CLK_CTRL, &pll);
	snd_log("snd: amp %s (%#x): CS35L41 %#x rev %#x, trimmed; SP_FORMAT %#x RX_SLOT %#x PLL %#x VOL %#x GAIN %#x PWR2 %#x\n",
	    a->name, a->addr, devid, revid, fmt, slot, pll, vol, gain, pwr2);
	a->present = true;
	return true;
}

static uint32_t amps_volume_reg[2] = { L41_VOL_MUTE, L41_VOL_MUTE };   /* as amp_prepare() leaves them */

/*
 * Calls (A, 2026-11-14): kern.redwood.snd_route 0 = the speakers (both amplifiers, as always), 1 = the earpiece: the
 * top amplifier alone (it drives the earpiece/top speaker), kern.redwood.snd_ear_below eighths of a dB further down
 * (default 18 dB), the bottom one muted. redwood-ims sets 1 when a call's voice starts and 0 when it ends; the call
 * screen's speaker button switches (RedwoodPhone).
 */
static int snd_route;
static int snd_ear_below = 18 * 8;
static int snd_call;                    /* a VoLTE call's voice is playing (redwood-ims): the audio plug-in writes nothing */
SYSCTL_INT(_kern_redwood, OID_AUTO, snd_call, CTLFLAG_RW | CTLFLAG_LOCKED, &snd_call, 0,
    "a VoLTE call in progress: iOS's audio plug-in leaves /dev/redwood-audio to redwood-ims");
SYSCTL_DECL(_kern_redwood);
SYSCTL_INT(_kern_redwood, OID_AUTO, snd_route, CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, &snd_route, 0,
    "speaker route: 0 speakers, 1 earpiece (top amplifier only, quieter)");
SYSCTL_INT(_kern_redwood, OID_AUTO, snd_ear_below, CTLFLAG_RW | CTLFLAG_LOCKED, &snd_ear_below, 0,
    "earpiece level: eighths of a dB below the speakers' level");

static void
amps_volume(uint32_t below, bool muted)
{
	for (unsigned i = 0; i < 2; i++) {
		uint32_t b = below;
		bool m = muted;

		if (snd_route == 1) {
			if (i == 1) {
				m = true;               /* the bottom speaker: off in the earpiece route */
			} else {
				b += (uint32_t)(snd_ear_below > 0 ? snd_ear_below : 0);
			}
		}
		uint32_t reg = (m || b >= 816) ? L41_VOL_MUTE : (uint32_t)(-(int32_t)b) & 0x7ff;

		if (reg == amps_volume_reg[i] || !amps[i].present) {
			continue;
		}
		amps_volume_reg[i] = reg;
		amp_update(&amps[i], L41_AMP_DIG_VOL_CTRL, 0x7ff << 3, reg << 3);
	}
}

/* main_amp_event POST_PMU: the power-up patch, GLOBAL_EN, PUP_DONE */
static bool
amps_up(void)
{
	bool ok = true;

	amp_start_retryable = true;

	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];
		uint32_t status = 0;
		unsigned t;
		bool observed = false;

		if (!a->present) {
			continue;
		}
		amp_write(a, L41_TEST_KEY_CTL, 0x55);
		amp_write(a, L41_TEST_KEY_CTL, 0xaa);
		amp_write(a, 0x2084, 0x002f1aa0);
		amp_write(a, L41_TEST_KEY_CTL, 0xcc);
		amp_write(a, L41_TEST_KEY_CTL, 0x33);
		amp_update(a, L41_PWR_CTRL1, 1, 1);
		for (t = 0; t < 20; t++) {
			if (amp_read(a, L41_IRQ1_STATUS1, &status)) {
				observed = true;
				if (status & (L41_PUP_DONE | L41_FAULTS)) {
					break;
				}
			}
			IOSleep(1);
		}
		amp_write(a, L41_IRQ1_STATUS1, L41_PUP_DONE);
		if (!(status & L41_PUP_DONE) || (status & L41_FAULTS)) {
			snd_log("snd: amp %s: power-up %s (IRQ1_STATUS1 %#x)\n", a->name,
			    (status & L41_FAULTS) ? "FAULT" : "not done", status);
			ok = false;
			if (!observed || (status & L41_FAULTS)) {
				amp_start_retryable = false;
			}
		}
	}
	return ok;
}

static void
amps_down(void)
{
	amps_volume(0, true);
	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];
		uint32_t status = 0;

		if (!a->present) {
			continue;
		}
		amp_update(a, L41_PWR_CTRL1, 1, 0);
		for (unsigned t = 0; t < 20; t++) {
			if (amp_read(a, L41_IRQ1_STATUS1, &status) && (status & L41_PDN_DONE)) {
				break;
			}
			IOSleep(1);
		}
		amp_write(a, L41_IRQ1_STATUS1, L41_PDN_DONE);
		amp_write(a, L41_TEST_KEY_CTL, 0x55);
		amp_write(a, L41_TEST_KEY_CTL, 0xaa);
		amp_write(a, 0x2084, 0x002f1aa3);
		amp_write(a, L41_TEST_KEY_CTL, 0xcc);
		amp_write(a, L41_TEST_KEY_CTL, 0x33);
	}
}

static uint32_t
amps_faults(void)
{
	uint32_t faults = 0;

	for (unsigned i = 0; i < 2; i++) {
		uint32_t status = 0;

		if (amps[i].present && amp_read(&amps[i], L41_IRQ1_STATUS1, &status)) {
			faults |= status & L41_FAULTS;
		}
	}
	return faults;
}

/* both present amplifiers answer on I2C and report no fault (the self-check) */
static bool
amps_answer(void)
{
	for (unsigned i = 0; i < 2; i++) {
		uint32_t status = 0;

		if (!amps[i].present || !amp_read(&amps[i], L41_IRQ1_STATUS1, &status) || (status & L41_FAULTS)) {
			return false;
		}
	}
	return true;
}

/*
 * The amplifiers' VA (1.8 V): the sub-PMIC's LDO4S (dtbo: cs35l41 VA-supply = <&s_ldo4_reg>, "regulator-always-on",
 * 1.6-1.95 V; S2MPS22 CTRL 0x2D: enable [7:6], 0xC0 on). Some boots it is off and neither chip answers (B, 2026-11-06:
 * "does not answer" / "OTP_BOOT_DONE never came" on about half the boots since 15:05). On here when it is not.
 */
#define SPMIC_LDO4S_CTRL        0x2d
static void
amps_supply(char *report, size_t cap)
{
	uint8_t before = 0, after = 0;
	int r = redwood_spmic_read(1, SPMIC_LDO4S_CTRL, &before);

	if (r != 0) {
		snprintf(report, cap, "LDO4S not read (%d)", r);
		return;
	}
	after = before;
	if ((before & 0xc0) != 0xc0) {
		if (redwood_spmic_write(1, SPMIC_LDO4S_CTRL, before | 0xc0) == 0) {
			(void)redwood_spmic_read(1, SPMIC_LDO4S_CTRL, &after);
		}
		IOSleep(5);
	}
	snprintf(report, cap, "LDO4S (amps' VA) CTRL %#04x%s%#04x", before, before == after ? " kept " : " -> ", after);
}

static bool
amps_bring_up(void)
{
	char report[200];
	bool any = false;

	/* A reset invalidates cached volume and previous presence results. */
	amps_volume_reg[0] = amps_volume_reg[1] = L41_VOL_MUTE;
	for (unsigned i = 0; i < 2; i++) {
		amps[i].present = false;
	}
	amps_supply(report, sizeof(report));
	snd_log("snd: %s\n", report);

	gpb2 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, PINCTRL_PERIC1_GPB2);
	amp_bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, PINCTRL_PERIC0_GPP4);
	for (unsigned i = 0; i < 2; i++) {
		redwood_gpio_set(gpb2, amps[i].reset_pin, 0);
	}
	IOSleep(2);
	for (unsigned i = 0; i < 2; i++) {
		redwood_gpio_set(gpb2, amps[i].reset_pin, 1);
	}
	IOSleep(3);
	if (!redwood_bbi2c_setup(&amp_bus, true, report, sizeof(report))) {
		snd_log("snd: %s: the bus is not free\n", report);
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		any |= amp_prepare(&amps[i]);
	}
	return any;
}

/* ---- ABOX ----------------------------------------------------------------------------------------------------- */

static bool
aud_power_on(void)
{
	bool was_on = (rd(pmu, PMU_AUD_STATUS) & 1) != 0;
	uint64_t ret;

	if (!was_on) {
		upd(pmu, PMU_AUD_CONFIGURATION, 1, 1);
		if (!wait_bits(pmu, PMU_AUD_STATUS, 1, 1, 10000)) {
			snd_log("snd: pd-aud does not power on (status %#x)\n", rd(pmu, PMU_AUD_STATUS));
			return false;
		}
	}
	ret = smc4(SMC_PREPARE_PD_ONOFF, 1, AUD_NEED_SMC, 2);
	upd(pmu, PMU_ABOX_PAD, PMU_ABOX_PAD_NORMAL, PMU_ABOX_PAD_NORMAL);
	snd_log("snd: pd-aud %s; DTZPC restore answers %#llx; pads %#x\n", was_on ? "was on" : "powered on", ret,
	    rd(pmu, PMU_ABOX_PAD));
	return true;
}

static void
set_div(uint32_t off, uint32_t mask, uint32_t value)
{
	upd(cmu, off, mask, value);
	wait_bits(cmu, off, CLK_BUSY, 0, 1000);
}

static bool
aud_clocks(void)
{
	uint32_t con3;
	uint32_t found_con3 = rd(cmu, PLL_CON3_AUD0);
	uint32_t found_con5 = rd(cmu, PLL_CON5_AUD0);
	uint32_t fp = (found_con3 >> 8) & 0x3fU, fs = found_con3 & 7U;
	uint64_t fm = (found_con3 >> 16) & 0x3ffU, fk = found_con5 & 0xffffU;
	bool locked = (found_con3 & (1U << 29)) != 0;
	/*
	 * The rate, not the fields: S-Boot locks the same 1179.648 MHz with a different decomposition
	 * (M 45 P 1 K 24319 where this driver writes M 181 P 4 K 31740), so comparing M/P/K called a
	 * working PLL "wrong" and reset it.
	 */
	uint64_t found_rate = (fp == 0 || fs > 6) ? 0 :
	    (26000000ULL * ((fm << 16) + fk)) / ((uint64_t)fp << (16 + fs));
	bool ours = locked && found_rate > 1179648000ULL - 118000ULL && found_rate < 1179648000ULL + 118000ULL;

	snd_log("snd: CMU_AUD as found: LOCKTIME %#x CON0 %#x CON1 %#x CON2 %#x CON3 %#x CON4 %#x CON5 %#x (%s%s)\n",
	    rd(cmu, PLL_LOCKTIME_AUD0), rd(cmu, PLL_CON0_AUD0), rd(cmu, 0x104), rd(cmu, 0x108),
	    found_con3, rd(cmu, 0x110), found_con5, locked ? "locked" : "not locked",
	    ours ? ", the rate we want" : "");

	/*
	 * S-Boot plays its own sounds, so it may have left the audio PLL running. Keep it when it is
	 * locked at the rate this driver wants: resetting a working PLL is what failed before.
	 */
	if (locked && ours) {
		snd_log("snd: PLL_AUD0 was already locked at our rate: leaving it alone\n");
	} else {
		/* PLL_AUD0 off its output while it is set (ra_enable_pll / ra_pll_set_pmsk) */
		upd(cmu, PLL_CON0_AUD0, 1U << 4, 0);
		wait_bits(cmu, PLL_CON0_AUD0, 1U << 7, 0, 1000);
		upd(cmu, PLL_CON3_AUD0, 1U << 31, 0);
		wr(cmu, PLL_LOCKTIME_AUD0, 4 * 500);
		upd(cmu, PLL_CON5_AUD0, 0xffff, 31740);
		con3 = rd(cmu, PLL_CON3_AUD0) & ~((0x3ffU << 16) | (0x3fU << 8) | 7U);
		wr(cmu, PLL_CON3_AUD0, con3 | 181U << 16 | 4U << 8 | 0U | 1U << 4 | 1U << 31);
		if (!wait_bits(cmu, PLL_CON3_AUD0, 1U << 29, 1U << 29, 10000)) {
			snd_log("snd: PLL_AUD0 does not lock: CON0 %#x CON1 %#x CON2 %#x CON3 %#x CON4 %#x CON5 %#x\n",
			    rd(cmu, PLL_CON0_AUD0), rd(cmu, 0x104), rd(cmu, 0x108), rd(cmu, PLL_CON3_AUD0),
			    rd(cmu, 0x110), rd(cmu, PLL_CON5_AUD0));
			return false;
		}
		upd(cmu, PLL_CON0_AUD0, 1U << 4, 1U << 4);
		wait_bits(cmu, PLL_CON0_AUD0, 1U << 7, 0, 1000);
	}

	set_div(DIV_BUS, 0x7, 5);               /* 196.6 MHz */
	set_div(DIV_BUSP, 0x3, 2);              /* 65.5 MHz */
	set_div(DIV_AUDIF, 0x1ff, 15);          /* 73.728 MHz */
	set_div(DIV_SCLK, 0x1ff, 0);            /* ABOX's SCLK: 73.728 MHz */
	snd_clocks_on();
	upd(cmu, MUX_UAIF2, 0x3, 0);            /* UAIF2 from AUDIF */
	wait_bits(cmu, MUX_UAIF2, CLK_BUSY, 0, 1000);
	set_div(DIV_UAIF2, 0x1ff, 23);          /* 3.072 MHz */
	snd_log("snd: CMU_AUD: PLL_AUD0 CON0 %#x CON3 %#x CON5 %#x; BUS %#x BUSP %#x AUDIF %#x SCLK %#x UAIF2 mux %#x div %#x\n",
	    rd(cmu, PLL_CON0_AUD0), rd(cmu, PLL_CON3_AUD0), rd(cmu, PLL_CON5_AUD0), rd(cmu, DIV_BUS), rd(cmu, DIV_BUSP),
	    rd(cmu, DIV_AUDIF), rd(cmu, DIV_SCLK), rd(cmu, MUX_UAIF2), rd(cmu, DIV_UAIF2));
	return true;
}

static uint32_t snd_sifs_cnt;           /* snd_sifs_cnt=V: SIFS0's count as given (0: computed as Samsung's driver does) */

/*
 * The SIFS's count (ABOX_SPUS_CTRL_SIFS_CNT, 0x290 + 4n, 23:0): how many CNT-clock cycles per sample period it takes
 * its next sample from the chain. Samsung's driver (abox_cmpnt.c set_cnt_val, sifsx_cnt_val) writes
 *   round(cnt_hz * n / (channels * rate)) - 1,  n = 2 * (32 / physical width)
 * for every stream; left at 0 nothing paces the SIFS, UAIF2 is never fed (STATUS 0) and RDMA3 waits with PROGRESS set
 * and 0 frames read -- what this phone did (2026-11-06). Here SIFS0 (the mixer) runs 32-bit stereo at 48 kHz.
 */
static uint32_t
sifs_count(void)
{
	uint32_t mux = rd(cmu, MUX_AUD_CNT) & 1, div = rd(cmu, DIV_AUD_CNT) & 0x1ff;
	uint32_t audif_div = rd(cmu, DIV_AUDIF) & 0x1ff;
	uint64_t audif_hz = 1179648000ULL / (audif_div + 1), cnt_hz, n = 2 * (32 / 32), d = 2 * 48000ULL, v;

	if (mux != 0) {
		/* PLL_AUD1's rate is not known here: the CNT clock is moved to AUDIF, whose rate is */
		wr(cmu, MUX_AUD_CNT, rd(cmu, MUX_AUD_CNT) & ~1U);
		IOSleep(1);
		mux = rd(cmu, MUX_AUD_CNT) & 1;
	}
	cnt_hz = audif_hz / (div + 1);
	v = (cnt_hz * n + d / 2) / d - 1;
	snd_log("snd: SIFS count: CNT clock mux %u div %u -> %llu Hz (AUDIF %llu Hz); 32-bit stereo 48 kHz: count %llu%s\n",
	    mux, div, cnt_hz, audif_hz, v, snd_sifs_cnt ? " (overridden by snd_sifs_cnt)" : "");
	return snd_sifs_cnt ? snd_sifs_cnt : (uint32_t)(v & 0xffffff);
}

static uint32_t snd_sblimit;            /* snd_sblimit=V: RDMA3 SBANK_LIMIT (+0x20, v3 field 13:4), as calliope sets it */
static uint32_t snd_burst = 4;          /* snd_burst=N: RDMA3 CTRL0 BURST_LEN (22:19); Scion's S9 runs 8 */
static uint32_t snd_bufupd;             /* snd_bufupd=1: RDMA3 CTRL0 BUF_UPDATE (bit 5), as calliope sets it on the S9 */
static uint32_t snd_sifs1;              /* snd_sifs1=1: chain 3 out to SIFS1 and UAIF2 from it, not SIFS0 (the mixer) */
/*
 * snd_samsung=1 (default; B, 2026-11-07: THE SPEAKER PLAYS): the ABOX's state as Samsung's own kernel left it while it
 * played on this phone (work/stock-audio-test/ref_abox.txt: the stock ArtisanKRNL booted from BOOT with a test ramdisk,
 * regmap debugfs read during tinyplay): QCHANNEL_DISABLE, ROUTE_CTRL1, all of SPUS (function chains: RDMA3 -> ASRC ->
 * SIFS0; SPUS_CTRL4/5 the ASRC ids: chain 3 on ASRC 7; MIXP 32-bit x 4; every sample bank), the SPUM banks, and ASRC 7
 * as asrc_config(7, 0, 11, 48000 Hz, 11, 48000 Hz, 32 bit) left it. Written with RDMA3 and UAIF2 stopped. With it and
 * UAIF2 at 4 x 32 bit / 6.144 MHz (and the amplifiers' SCLK to match) RDMA3 streams and the speaker sounds.
 */
static const struct { uint16_t off; uint32_t val; } samsung_abox[] = {
	{ 0x0038, 0x00000010 },
	{ 0x0044, 0x00000140 },
	{ 0x0200, 0x03020202 },
	{ 0x0204, 0x0000001b },
	{ 0x0208, 0x00000000 },
	{ 0x020c, 0x00000000 },
	{ 0x0210, 0x72100000 },
	{ 0x0214, 0xffff3654 },
	{ 0x0218, 0x02020202 },
	{ 0x021c, 0x02000202 },
	{ 0x0220, 0x02000000 },
	{ 0x0224, 0x02000200 },
	{ 0x0228, 0x02000400 },
	{ 0x022c, 0x00800600 },
	{ 0x0230, 0x02000800 },
	{ 0x0234, 0x02000a00 },
	{ 0x0238, 0x02000c00 },
	{ 0x023c, 0x02000e00 },
	{ 0x0240, 0x02001000 },
	{ 0x0244, 0x02001200 },
	{ 0x0248, 0x02001400 },
	{ 0x024c, 0x02001600 },
	{ 0x0250, 0x02c01800 },
	{ 0x0254, 0x01a01ac0 },
	{ 0x0258, 0x01a01c60 },
	{ 0x025c, 0x01101e00 },
	{ 0x0260, 0x02c02000 },
	{ 0x0264, 0x01a022c0 },
	{ 0x0268, 0x01a02460 },
	{ 0x026c, 0x01102600 },
	{ 0x0280, 0x00802800 },
	{ 0x0284, 0x02002a00 },
	{ 0x0288, 0x00000000 },
	{ 0x028c, 0x00000000 },
	{ 0x0290, 0x00000000 },
	{ 0x0294, 0x00000000 },
	{ 0x0298, 0x00000000 },
	{ 0x029c, 0x00000000 },
	{ 0x02a0, 0x00010008 },
	{ 0x02b0, 0x20010101 },
	{ 0x02b4, 0x01010101 },
	{ 0x02b8, 0x01010101 },
	{ 0x0300, 0x00000000 },
	{ 0x0304, 0x00000948 },
	{ 0x030c, 0x00000000 },
	{ 0x0310, 0x00732100 },
	{ 0x0328, 0x02000000 },
	{ 0x032c, 0x02000200 },
	{ 0x0330, 0x02000400 },
	{ 0x0334, 0x02000600 },
	{ 0x0338, 0x02000800 },
	{ 0x034c, 0x02c00a00 },
	{ 0x0350, 0x01a00cc0 },
	{ 0x0354, 0x01a00e60 },
	{ 0x0358, 0x01901000 },
	{ 0x2710, 0x00010000 },
	{ 0x2714, 0x00000ccc },
	{ 0x2718, 0x00010000 },
	{ 0x271c, 0x00000ccc },
	{ 0x2700, 0x9bb000fc },
};

static void
abox_route(void)
{
	uint32_t ctrl0, ctrl1;

	snd_log("snd: ABOX IP %#x version %#x; SysMMU CTRL %#x CTRL_VM %#x CFG %#x STATUS %#x VERSION %#x\n",
	    rd(abox, ABOX_IP_INDEX), rd(abox, ABOX_VERSION), rd(mmu, MMU_CTRL), rd(mmu, MMU_CTRL_VM), rd(mmu, MMU_CFG),
	    rd(mmu, MMU_STATUS), rd(mmu, MMU_VERSION));
	/*
	 * Translating when any of bits 0-2 is set: Linux's CTRL_ENABLE is 0x5 and CTRL_BLOCK 0x7, and the boot leaves 0x4
	 * (bit 2 alone: translating) -- read on the S20 (x1slte) and on the S9 (Scion's hakios.exynos_abox.c, step 7). The
	 * old test of bit 0 alone took 0x4 for off: RDMA3's physical ring was then an IOVA nothing mapped, and the DMA
	 * read 0 frames.
	 */
	if (snd_calliope < 2 && ((rd(mmu, MMU_CTRL) & 0x7) || (rd(mmu, MMU_CTRL_VM) & 1))) {
		wr(mmu, MMU_CTRL_VM, 0);
		wr(mmu, MMU_CTRL, 0);
		snd_log("snd: SysMMU was on: off (CTRL %#x CTRL_VM %#x)\n", rd(mmu, MMU_CTRL), rd(mmu, MMU_CTRL_VM));
	}

	wr(abox, ABOX_SYSPOWER_CTRL, 1);        /* abox_request_dram_on */
	wait_bits(abox, ABOX_SYSPOWER_STATUS, 1, 1, 10000);

	if (snd_samsung != 0 && snd_calliope < 2) {
		wr(abox, RDMA(RDMA_CTRL0), rd(abox, RDMA(RDMA_CTRL0)) & ~1U);
		wr(abox, ABOX_UAIF_CTRL0, rd(abox, ABOX_UAIF_CTRL0) & ~1U);
		for (unsigned i = 0; i < sizeof(samsung_abox) / sizeof(samsung_abox[0]); i++) {
			wr(abox, samsung_abox[i].off, samsung_abox[i].val);
		}
		wr(abox, ABOX_SIFS_CNT0, 0);                    /* UAIF master: Samsung resets it (abox_cmpnt_reset_cnt_val) */
		upd(abox, ABOX_ROUTE_CTRL0, 0xfU << (4 * UAIF_ID), 1U << (4 * UAIF_ID));
		set_div(DIV_UAIF2, 0x1ff, 11);                  /* 6.144 MHz: 48 kHz x 4 slots x 32 bits */
		wr(abox, ABOX_UAIF_CTRL0, (rd(abox, ABOX_UAIF_CTRL0) & ~(1U << 2 | 1U)) | 1U << 2 | 1U);
		wr(abox, ABOX_UAIF_CTRL1, 0x1bcdf7dfU);         /* Samsung's: DSP_A, 4 slots x 32 bits */
		gpb1 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, PINCTRL_PERIC1_GPB1);
		for (uint32_t pin = 0; pin < 4; pin++) {
			redwood_gpio_pull(gpb1, pin, REDWOOD_GPIO_PULL_NONE);
			redwood_gpio_func(gpb1, pin, 2);
		}
		snd_log("snd: samsung: %u ABOX registers as the stock kernel's playback; UAIF2 4 x 32 bit at 6.144 MHz\n",
		    (unsigned)(sizeof(samsung_abox) / sizeof(samsung_abox[0])));
		goto rdma;
	}
	/* RDMA3 -> chain 3 -> SIFS0 (the mixer, 32-bit stereo) -> UAIF2 */
	/* OUT 4:1 is SIFSn + 1: 1 the mixer (SIFS0), 2 SIFS1 */
	upd(abox, ABOX_SPUS_CTRL_FC0, 0xffU << (8 * RDMA_ID), (snd_sifs1 ? 0x04U : 0x02U) << (8 * RDMA_ID));
	upd(abox, ABOX_SPUS_CTRL1, 0x1f, ABOX_FORMAT(32, 2));
	if (rd(abox, ABOX_SPUS_SBANK_RDMA(RDMA_ID)) == 0) {
		wr(abox, ABOX_SPUS_SBANK_RDMA(RDMA_ID), (512U << 16) | 0x000);
	}
	if (rd(abox, ABOX_SPUS_SBANK_MIXP) == 0) {
		wr(abox, ABOX_SPUS_SBANK_MIXP, (512U << 16) | 0x200);
	}
	wr(abox, ABOX_SIFS_CNT0, sifs_count());
	if (snd_sifs1) {
		wr(abox, ABOX_SIFS_CNT0 + 4, sifs_count());     /* SIFS1's count */
	}
	upd(abox, ABOX_ROUTE_CTRL0, 0xfU << (4 * UAIF_ID), (snd_sifs1 ? 2U : 1U) << (4 * UAIF_ID));

	/*
	 * UAIF2's bit clock past the ABOX's own Q-channel: Samsung's driver disables it when the interface's BCLK
	 * widget powers up (abox_if.c, abox_if_bclk_event -> abox_disable_qchannel(ABOX_BCLK_UAIF0 + id, 1)), and
	 * with nothing asking for the clock the UAIF never pulls a sample -- RDMA3 "in progress", 0 frames read,
	 * with CMU_AUD's gates and Q-channels all open (run 30). The bus clock (ACLK) as well.
	 */
	{
		uint32_t qch = rd(abox, ABOX_QCHANNEL_DISABLE), dyn = rd(abox, ABOX_DYN_CLOCK_OFF);

		wr(abox, ABOX_QCHANNEL_DISABLE, qch | (1U << QCH_ABOX_BCLK_UAIF(UAIF_ID)) | (1U << QCH_ABOX_ACLK));
		snd_log("snd: ABOX Q-channels disabled %#x -> %#x (BCLK_UAIF%u, ACLK); dynamic clock off %#x\n", qch,
		    rd(abox, ABOX_QCHANNEL_DISABLE), UAIF_ID, dyn);
	}

	/* UAIF2: set_fmt DSP_A | NB_NF | CBS_CFS, hw_params 32 bits x 2 */
	ctrl0 = rd(abox, ABOX_UAIF_CTRL0);
	/*
	 * bit 2 = MODE (1: master, it makes the BCLK), bit 0 = SPK_ENABLE: Samsung sets the latter
	 * from abox_uaif_mute_stream on unmute, and with it clear the mixer has no consumer, so the
	 * RDMA never advances (it sat at 0 with the enable bit set).
	 */
	ctrl0 = (ctrl0 & ~(1U << 2 | 1U)) | 1U << 2 | 1U;
	wr(abox, ABOX_UAIF_CTRL0, ctrl0);
	ctrl1 = ABOX_FORMAT(32, 2) << 24 | 1U << 23 | 1U << 22 | 0U << 21 | (2U - 1) << 18 | 31U << 12 | 31U << 6 | 31U;
	wr(abox, ABOX_UAIF_CTRL1, ctrl1);

	gpb1 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, PINCTRL_PERIC1_GPB1);
	for (uint32_t pin = 0; pin < 4; pin++) {
		redwood_gpio_pull(gpb1, pin, REDWOOD_GPIO_PULL_NONE);
		redwood_gpio_func(gpb1, pin, 2);
	}

	if (snd_calliope >= 2) {
		snd_log("snd: calliope: routing and UAIF2 set by the AP; RDMA3 is Calliope's (ROUTE0 %#x FC0 %#x UAIF2 %#x %#x)\n",
		    rd(abox, ABOX_ROUTE_CTRL0), rd(abox, ABOX_SPUS_CTRL_FC0), rd(abox, ABOX_UAIF_CTRL0), rd(abox, ABOX_UAIF_CTRL1));
		return;
	}
rdma:
	/* RDMA3: our ring, 16-bit stereo */
	if (snd_samsung != 0) {
		wr(abox, RDMA(0x18), 0x800000);                 /* VOL_FACTOR, VOL_CHANGE, BIT_CTRL0 as Samsung's */
		wr(abox, RDMA(0x1c), 0x10000);
		wr(abox, RDMA(0x24), 0x3);
	}
	wr(abox, RDMA(RDMA_CTRL0), ABOX_FORMAT(16, 2) << 8 | (snd_burst & 0xfU) << 19 | (snd_bufupd ? 1U << 5 : 0));
	if (snd_sblimit) {
		wr(abox, RDMA(0x20), snd_sblimit);
	}
	snd_log("snd: x1slte knobs: snd_burst=%u snd_bufupd=%u snd_sifs1=%u snd_sblimit=%#x (reads %#x)\n", snd_burst, snd_bufupd,
	    snd_sifs1, snd_sblimit, rd(abox, RDMA(0x20)));
	wr(abox, RDMA(RDMA_BUF_STR), (uint32_t)buf_phys);
	wr(abox, RDMA(RDMA_BUF_END), (uint32_t)buf_phys + BUF_BYTES);
	wr(abox, RDMA(RDMA_BUF_OFFSET), PERIOD_BYTES);
	wr(abox, RDMA(RDMA_STR_POINT), (uint32_t)buf_phys);
	if ((rd(abox, RDMA(RDMA_VOL_FACTOR)) & 0xffffff) == 0) {
		wr(abox, RDMA(RDMA_VOL_FACTOR), 0x800000);
	}
	/*
	 * Start it: CTRL0 bit 0 is ABOX_RDMA_ENABLE (abox_soc_2.h), and Samsung's PCM trigger
	 * normally sets it from the DSP's mailbox. Written last, so the DMA never runs over a
	 * buffer that is not set up yet.
	 */
	upd(abox, RDMA(RDMA_CTRL0), 1U, 1U);
	snd_log("snd: RDMA3 enabled: CTRL0 %#x STATUS %#x\n", rd(abox, RDMA(RDMA_CTRL0)),
	    rd(abox, RDMA(RDMA_STATUS)));
	snd_log("snd: ABOX: SYSPOWER %#x/%#x ROUTE0 %#x FC0 %#x SPUS1 %#x SBANK %#x/%#x UAIF2 %#x %#x RDMA3 %#x %#x-%#x vol %#x\n",
	    rd(abox, ABOX_SYSPOWER_CTRL), rd(abox, ABOX_SYSPOWER_STATUS), rd(abox, ABOX_ROUTE_CTRL0),
	    rd(abox, ABOX_SPUS_CTRL_FC0), rd(abox, ABOX_SPUS_CTRL1), rd(abox, ABOX_SPUS_SBANK_RDMA(RDMA_ID)),
	    rd(abox, ABOX_SPUS_SBANK_MIXP), rd(abox, ABOX_UAIF_CTRL0), rd(abox, ABOX_UAIF_CTRL1), rd(abox, RDMA(RDMA_CTRL0)),
	    rd(abox, RDMA(RDMA_BUF_STR)), rd(abox, RDMA(RDMA_BUF_END)), rd(abox, RDMA(RDMA_VOL_FACTOR)));
}

/* where the DMA reads, in frames into the ring (abox_rdma_pointer) */
static uint32_t
dma_position(uint32_t *raw)
{
	uint32_t st = rd(abox, RDMA(RDMA_STATUS)), bytes = (((st >> 12) & 0xffff) << 4) + (st & 0xfff);

	if (raw != NULL) {
		*raw = st;
	}
	return (bytes % BUF_BYTES) / 4;
}

static void
abox_start(void)
{
	uint32_t raw[8], pos[8];
	uint64_t t0, t[8];

	bzero(buf, BUF_BYTES);
	__asm__ volatile ("dsb sy" ::: "memory");
	upd(abox, ABOX_UAIF_CTRL0, 1, 1);
	upd(abox, RDMA(RDMA_CTRL0), 1, 1);
	t0 = now_us();
	for (unsigned i = 0; i < 8; i++) {
		IOSleep(5);
		pos[i] = dma_position(&raw[i]);
		t[i] = now_us() - t0;
	}
	snd_log("snd: started; RDMA3 STATUS %#x@%llu %#x@%llu %#x@%llu %#x@%llu ... %#x@%llu us (frames %u %u %u %u .. %u); "
	    "UAIF2 STATUS %#x\n", raw[0], t[0], raw[1], t[1], raw[2], t[2], raw[3], t[3], raw[7], t[7], pos[0], pos[1], pos[2],
	    pos[3], pos[7], rd(abox, ABOX_UAIF_STATUS));
}

/* ---- iOS's samples -------------------------------------------------------------------------------------------- */

/* from the writer's thread, under redwood_audio.c's lock: never waits */
static uint32_t
exynos_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = ring_head, room = RING_FRAMES - (head - ring_tail), taken = count < room ? count : room;
	bool sound = false;

	for (uint32_t i = 0; i < taken; i++) {
		int16_t l = frames[2 * i], r = frames[2 * i + 1];

		ring[2 * ((head + i) % RING_FRAMES)] = l;
		ring[2 * ((head + i) % RING_FRAMES) + 1] = r;
		sound |= l > (int32_t)snd_gate || l < -(int32_t)snd_gate || r > (int32_t)snd_gate || r < -(int32_t)snd_gate;
	}
	OSMemoryBarrier();
	ring_head = head + taken;
	ios_frames += taken;
	if (sound) {
		last_sound_us = now_us();
	}
	if (sound && snd_quiet) {
		thread_wakeup((event_t)&ring_head);     /* the thread is waiting for something to play */
	}
	return taken;
}

static uint32_t tone_left, tone_phase;

/* `n' frames (at most CHUNK) at frame `at' of the DMA's ring: from iOS (or the tone), the rest silence, through the guard */
static void
fill(uint64_t at, uint32_t n)
{
	static int16_t chunk[CHUNK * 2];
	uint32_t tail = ring_tail, have = ring_head - tail, take = have < n ? have : n, i = 0;

	if (tone_left != 0) {
		for (; i < n && tone_left != 0; i++, tone_left--) {
			chunk[2 * i] = chunk[2 * i + 1] = tone[tone_phase];
			tone_phase = (tone_phase + 1) % 48;
		}
		take = 0;
	} else {
		OSMemoryBarrier();
		for (; i < take; i++) {
			chunk[2 * i] = ring[2 * ((tail + i) % RING_FRAMES)];
			chunk[2 * i + 1] = ring[2 * ((tail + i) % RING_FRAMES) + 1];
		}
		OSMemoryBarrier();
		ring_tail = tail + take;
	}
	if (take != 0 && snd_gate != 0) {
		int32_t peak = 0;

		for (uint32_t k = 0; k < 2 * take; k++) {
			int32_t v = chunk[k] < 0 ? -(int32_t)chunk[k] : chunk[k];

			peak = v > peak ? v : peak;
		}
		gate_peak_max = (uint32_t)peak > gate_peak_max ? (uint32_t)peak : gate_peak_max;
		gate_blocks++;
		if (peak < (int32_t)snd_gate) {
			bzero(chunk, take * 4);         /* under the gate: silence */
			gate_closed++;
		}
	}
	fed_frames += i;
	silent_frames += n - i;
	bzero(chunk + 2 * i, (n - i) * 4);
	redwood_guard_process(chunk, n);
	if (take != 0 && redwood_audio_recording_fed()) {
		redwood_audio_record_fed(chunk, take);
	}
	memcpy(buf + 2 * (at % BUF_FRAMES), chunk, n * 4);
}

/* ---- the thread ----------------------------------------------------------------------------------------------- */

/* ---- Calliope (snd_calliope=1, B 2026-11-06): Samsung's DSP firmware, stage 1 -- load, map, start, see it live ------ */
/*
 * proposals/sound-calliope/NOTES.md. On the S20 Linux never programs an RDMA; Calliope does (abox_rdma.c only sends IPC),
 * and the bypass (above) stays idle on ABOX v3. Samsung's order (abox.c abox_enable): ABOX's GIC (abox_gic_init_gic),
 * the CPU clock, the timer (abox_start_timer), the firmware (abox_core_download_firmware), DRAM on (SYSPOWER), the cores'
 * PMU power then enable. Calliope sees its DRAM at IOVA 0x80000000 through ABOX's SysMMU (abox.c: IOVA_DRAM_FIRMWARE ->
 * abox_rmem, DRAM_FIRMWARE_SIZE 0x2A00000, and 0x10000000 one to one), so the SysMMU is turned ON here with a first-level
 * table of 1 MB sections (exynos-iommu.h: mk_lv1ent_sect(pa) = pa >> 4 | 2; FLPT base by page number, the VM register
 * set when CAPA1 says VCR). Facts: exynos9830.dts abox@18c50000, abox-core@18c55000/@18c55080, abox-gic@18cf0000,
 * abox-rmem 0xF8000000 (free in UEFI's map: f2195000-100000000 type 7, read 2026-11-06).
 */
#define CAL_SRAM_PHYS           0x18d00000U
#define CAL_SRAM_SIZE           0x78c00U
#define CAL_DRAM_PHYS           0xf8000000ULL
#define CAL_DRAM_SIZE           0x2a00000U
#define CAL_IOVA_DRAM           0x80000000U
#define CAL_IOVA_SLOG           0xE0000000U     /* IOVA_SILENT_LOG */
#define CAL_SLOG_PHYS           0xF2200000ULL
#define CAL_SLOG_SIZE           0x2800000U
#define CAL_IOVA_DUMP           0xD0000000U     /* IOVA_DUMP_BUFFER */
#define CAL_DUMP_PHYS           0xFAA00000ULL
#define CAL_DUMP_SIZE           0x5600000U
#define CAL_IOVA_VSS            0xA0000000U     /* IOVA_VSS_FIRMWARE; VSS_PARAMETER +16 MB, VSS_PCI +32 MB, doorbell +48 MB */
#define CAL_IOVA_PCI            0xA2000000U     /* IOVA_VSS_PCI */
#define CAL_PCI_PHYS            0xF4A00000ULL   /* Samsung's abox-pci-rmem is 0xF4200000, under our silent log: moved up */
#define CAL_PCI_SIZE            0x800000U
#define CAL_VSS_SPARE_PHYS      0xF5200000ULL   /* one cleared MB behind every other voice-call section */
static volatile uint8_t *cal_slog;              /* Calliope's own log, mapped for us to read */
#define CAL_GPR_PHYS            0x18c55000U
#define CAL_GICD_PHYS           0x18cf1000U
#define CAL_GICC_PHYS           0x18cf2000U
#define CAL_TIMER_PHYS          0x18ce0000U
#define CAL_IPC_RX_OFF          0x868000U       /* samsung,ipc-rx-area <1 0x868000 0x18000>: DRAM */
#define CAL_IPC_TX_OFF          0x850000U       /* samsung,ipc-tx-area <1 0x850000 0x18000>: DRAM */
#define CAL_IPC_AREA            0x18000U
/* abox_msg.h: struct abox_msg_queue = cmd queue (idx_s, idx_e, 128 cmds of 56 bytes), then data queue (idx_s, idx_e, len, bytes) */
#define CAL_Q_CMD_ELEM          8U
#define CAL_Q_CMD_SIZE          56U
#define CAL_Q_CMD_LEN           128U
#define CAL_Q_DATA              (8U + CAL_Q_CMD_LEN * CAL_Q_CMD_SIZE)
#define CAL_Q_DATA_ELEM         (CAL_Q_DATA + 12U)
#define CAL_HNDSHK_OFF          0x2f2e0U        /* samsung,handshake-area <0 0x2f2e0 0x20>: SRAM */
#define MMU_CAPA0_V7            0x870U
#define MMU_CAPA1_V7            0x874U

/*
 * A firmware file from the system image's /fw once iOS's root is mounted (A, 2026-11-06): Calliope's 3 MB do not fit
 * in the small image beside everything else (its slot is 14.4 MB), so userland/build.sh puts them in the system image
 * (on super), as redwood_adsp.c's root_file does for the Qualcomm DSP. Returns kalloc'd memory (*owned set) or NULL.
 */
static uint8_t *
calliope_root_file(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > 64 * 1024 * 1024) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool
calliope_copy(volatile uint8_t *to, uint32_t room, const char *name, uint32_t offset)
{
	uint64_t size = 0;
	const uint8_t *fw = redwood_image_file("fw", name, &size, NULL);
	uint8_t *owned = NULL;

	if (fw == NULL) {
		fw = owned = calliope_root_file(name, &size);
	}
	if (fw == NULL || size == 0 || offset + size > room) {
		snd_log("snd: calliope: %s: %s\n", name, fw == NULL ? "neither in the small image's fw/ nor in /fw" : "does not fit");
		if (owned != NULL) {
			kfree_data(owned, size);
		}
		return false;
	}
	for (uint64_t i = 0; i + 4 <= size; i += 4) {
		uint32_t w;

		memcpy(&w, fw + i, 4);
		*(volatile uint32_t *)(to + offset + i) = w;
	}
	for (uint64_t i = size & ~3ULL; i < size; i++) {
		to[offset + i] = fw[i];
	}
	snd_log("snd: calliope: %s (%llu bytes) at +%#x%s\n", name, size, offset, owned ? " (from the system image)" : "");
	if (owned != NULL) {
		kfree_data(owned, size);
	}
	return true;
}


/*
 * Stage 2: read Calliope's messages to the AP the way abox_msg_recv does (the AP owns the rx queue's idx_s) and say what
 * each one is (ABOX_IPC_MSG: ipcid, task_id, then for IPC_SYSTEM msgtype, param1..3). BOOT_DONE's param3 is the
 * firmware's version, 4 characters ("Calliope is ready to sing").
 */
static uint32_t cal_msgs, cal_pointers;
static volatile uint8_t *cal_dram, *cal_gicd, *cal_pt, *cal_gpr, *cal_timer;
static void calliope_timers(const char *what);
static uint32_t cal_inv;
static volatile bool cal_booted;
static bool cal_timers;
static bool cal_sgi_smc, cal_sgi_tested;
static volatile uint32_t cal_logbuf;   /* ABOX_REPORT_LOG's address (IOVA in the DRAM firmware) */
static volatile uint32_t cal_pointer;   /* the last PCM_PLTDAI_POINTER Calliope sent for RDMA3 */

/* Calliope's own log (struct ABOX_LOG_BUFFER: index_writer, index_reader, size, text), what is new, line by line */
static void
calliope_log(void)
{
	volatile uint8_t *lb;
	uint32_t wi, ri, size;
	char line[160];
	unsigned n = 0, lines = 0;

	if (cal_dram == NULL || cal_logbuf == 0) {
		return;
	}
	lb = cal_dram + (cal_logbuf - 0x80000000U);
	wi = *(volatile uint32_t *)(lb + 0);
	ri = *(volatile uint32_t *)(lb + 4);
	size = *(volatile uint32_t *)(lb + 8);
	if (size == 0 || size > 0x400000 || wi >= size || ri >= size) {
		return;
	}
	while (ri != wi && lines < 60) {
		char c = (char)lb[12 + ri];

		ri = (ri + 1) % size;
		if (c == '\n' || n + 1 >= sizeof(line)) {
			line[n] = 0;
			if (n > 0) {
				snd_log("snd: calliope says: %s\n", line);
				lines++;
			}
			n = 0;
		} else if (c >= 0x20 && c < 0x7f) {
			line[n++] = c;
		}
	}
	*(volatile uint32_t *)(lb + 4) = ri;
}

/*
 * The AP's side of ABOX's GIC, as abox_gic_irq_handler: what Calliope raised for the AP (its IPC SGI) acknowledged and
 * ended -- IAR read, EOI, and for SGIs DIR -- through EL3 (the GIC is secure). Polled with the message queue: without it
 * the SGI stays pending on the AP's interface and Calliope takes one message, then waits (run 9: ISPENDR0 bit 15).
 */
static uint32_t cal_acks, cal_ack_ids[4];
static volatile bool cal_poking;
static thread_t cal_poker;

static void
calliope_ack(void)
{
	for (int n = 0; n < 16; n++) {
		uint32_t iar = 1023, id;

		if (!sfr_smc_read(CAL_GICC_PHYS + 0x0c, &iar)) {
			return;
		}
		id = iar & 0x3ff;
		if (id >= 1020) {
			return;
		}
		(void)sfr_smc_write(CAL_GICC_PHYS + 0x10, iar);                 /* GIC_CPU_EOI */
		if (id < 16) {
			(void)sfr_smc_write(CAL_GICC_PHYS + 0x1000, iar);       /* GIC_CPU_DEACTIVATE */
		}
		if (cal_acks < 4) {
			cal_ack_ids[cal_acks] = iar;
		}
		if (++cal_acks <= 4) {
			snd_log("snd: calliope: the AP acknowledged its interrupt %u (IAR %#x)\n", id, iar);
		}
	}
}

static void
calliope_drain(volatile uint8_t *dram)
{
	volatile uint8_t *rx = dram + CAL_IPC_RX_OFF;
	uint32_t data_len = *(volatile uint32_t *)(rx + CAL_Q_DATA + 8);

	if (cal_poking && current_thread() != cal_poker) {
		return;
	}
	calliope_ack();

	for (int n = 0; n < 32; n++) {
		int32_t s = *(volatile int32_t *)(rx + 0), e = *(volatile int32_t *)(rx + 4);
		volatile uint8_t *cmd, *d;
		uint32_t idx, size, w[6];

		if (s == e || s < 0 || s >= (int32_t)CAL_Q_CMD_LEN || data_len == 0 || data_len > CAL_IPC_AREA) {
			return;
		}
		cmd = rx + CAL_Q_CMD_ELEM + (uint32_t)s * CAL_Q_CMD_SIZE;
		idx = *(volatile uint32_t *)(cmd + 20);
		if (idx >= data_len) {
			snd_log("snd: calliope: message %u: data index %#x is beyond the queue (%#x): reading stops\n", cal_msgs, idx,
			    data_len);
			*(volatile int32_t *)(rx + 0) = e;
			return;
		}
		d = rx + CAL_Q_DATA_ELEM + idx;
		size = *(volatile uint32_t *)d;
		for (int i = 0; i < 6; i++) {
			w[i] = (size >= 4U * (i + 1)) ? *(volatile uint32_t *)(d + 4 + 4 * i) : 0;
		}
		cal_msgs++;
		if (w[0] == 2 && w[2] == 18) {                 /* IPC_PCMPLAYBACK, PCM_PLTDAI_POINTER: a period played */
			cal_pointers++;
			cal_pointer = w[4];
		} else if (w[0] == 1 && w[2] == 0x10 && w[3] == 0 && w[4] >= 0x80000000U && w[4] < 0x80000000U + CAL_DRAM_SIZE) {
			cal_logbuf = w[4];
			snd_log("snd: calliope: its log buffer at %#x\n", w[4]);
		} else if (w[0] == 1 && w[2] == 3) {
			cal_booted = true;
			snd_log("snd: calliope: Calliope is ready to sing (version %c%c%c%c), message %u\n", (char)(w[5] >> 24),
			    (char)(w[5] >> 16), (char)(w[5] >> 8), (char)w[5], cal_msgs);
		} else if (cal_msgs <= 80) {
			snd_log("snd: calliope: message %u: id %d cmd %d, %u bytes: ipc %u task %d type %#x p %#x %#x %#x\n", cal_msgs,
			    *(volatile int32_t *)cmd, *(volatile int32_t *)(cmd + 4), size, w[0], (int)w[1], w[2], w[3], w[4], w[5]);
		}
		*(volatile int32_t *)(rx + 0) = (s + 1) % (int32_t)CAL_Q_CMD_LEN;
		*(volatile int32_t *)(rx + CAL_Q_DATA + 0) = (int32_t)((idx + size + 4) % data_len);
		__asm__ volatile ("dsb sy" ::: "memory");
	}
}

static void
calliope_boot(void)
{
	volatile uint8_t *sram, *dram, *gpr, *gicd, *gicc, *timer, *pt;
	uint64_t pool_phys = 0, pool_size = 0, pt_phys;
	vm_offset_t pool_virt = 0;
	uint32_t capa0, capa1 = 0, flpt, inv, rx0;
	bool vcr = false;
	static const struct { uint32_t off; const char *name; } gates[] = {
		{ 0x2040, "CCLK_CA32" }, { 0x2084, "RSTNSYNC CPU_ACLK" }, { 0x2088, "RSTNSYNC CPU" }, { 0x208c, "RSTNSYNC CPU_PCLKDBG" },
		{ 0x207c, "CPU0 SW_RESET" }, { 0x2080, "CPU1 SW_RESET" },
	};

	sram = (volatile uint8_t *)ml_io_map(CAL_SRAM_PHYS, CAL_SRAM_SIZE);
	dram = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)CAL_DRAM_PHYS, CAL_DRAM_SIZE, VM_WIMG_WCOMB);
	gpr = (volatile uint8_t *)ml_io_map(CAL_GPR_PHYS, 0x1000);
	gicd = (volatile uint8_t *)ml_io_map(CAL_GICD_PHYS, 0x1000);
	gicc = (volatile uint8_t *)ml_io_map(CAL_GICC_PHYS, 0x1000);
	timer = (volatile uint8_t *)ml_io_map(CAL_TIMER_PHYS, 0x1000);
	if (sram == NULL || dram == NULL || gpr == NULL || gicd == NULL || gicc == NULL || timer == NULL ||
	    redwood_wifi_dma_pool(&pool_phys, &pool_virt, &pool_size) < 0 || pool_size < (8U << 20)) {
		snd_log("snd: calliope: cannot map (or no DMA pool for the SysMMU table)\n");
		return;
	}
	/* the CPU's clocks (Samsung: clk_cpu, abox_request_cpu_gear): gates passed, the CPU Q-channel asked for */
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		uint32_t was = rd(cmu, gates[i].off), now = (was & GATE_MANUAL) ? (was | GATE_CG_VAL) : (was & ~GATE_AUTO);

		if (now != was) {
			wr(cmu, gates[i].off, now);
		}
	}
	wr(cmu, 0x3000, (rd(cmu, 0x3000) & ~QCH_ENABLE) | QCH_CLOCK_REQ);
	snd_log("snd: calliope: CPU clock mux %u (0 PLL divider, 1 CMU_TOP), ACLK div %#x, CA32 gate %#x, QCH CPU %#x\n",
	    rd(cmu, 0x1004) & 1, rd(cmu, 0x1810), rd(cmu, 0x2040), rd(cmu, 0x3000));
	/* ABOX's GIC and timer, as abox_enable */
	wr(gicc, 0x04, 0xff);                   /* GIC_CPU_PRIMASK */
	wr(gicd, 0x000, 0x3);                   /* GIC_DIST_CTRL */
	wr(gicc, 0x000, 0x3);                   /* GIC_CPU_CTRL */
	/* the secure GIC (A, 2026-11-06: stage 3's messages sat unread): the same three writes through EL3, as abox_gic.c */
	{
		uint64_t r1 = sfr_smc_write(CAL_GICC_PHYS + 0x04, 0xff), r2 = sfr_smc_write(CAL_GICD_PHYS + 0x000, 0x3),
		    r3 = sfr_smc_write(CAL_GICC_PHYS + 0x000, 0x3);
		uint32_t dctl = 0xfafafafa, cctl = 0xfafafafa;
		bool ok1 = sfr_smc_read(CAL_GICD_PHYS, &dctl), ok2 = sfr_smc_read(CAL_GICC_PHYS, &cctl);

		snd_log("snd: calliope: GIC by SMC: writes %lld %lld %lld; GICD_CTLR %#x (%s, plain %#x) GICC_CTLR %#x (%s)\n",
		    (long long)r1, (long long)r2, (long long)r3, dctl, ok1 ? "read" : "unreadable", rd(gicd, 0), cctl,
		    ok2 ? "read" : "unreadable");
	}
	wr(timer, 0x600 + 0x20 * 0, 1);         /* ABOX_TIMER_CTRL0(0): START */
	wr(timer, 0x600 + 0x20 * 2, 1);         /* ABOX_TIMER_CTRL0(2) */
	/* the firmware (abox_core_download_firmware): SRAM and DRAM cleared, the four images at the dts offsets */
	for (uint32_t i = 0; i < CAL_SRAM_SIZE; i += 4) {
		*(volatile uint32_t *)(sram + i) = 0;
	}
	for (uint32_t i = 0; i < CAL_DRAM_SIZE; i += 4) {
		*(volatile uint32_t *)(dram + i) = 0;
	}
	if (!calliope_copy(sram, CAL_SRAM_SIZE, "calliope_sram.bin", 0) || !calliope_copy(dram, CAL_DRAM_SIZE, "calliope_dram.bin", 0) ||
	    !calliope_copy(sram, CAL_SRAM_SIZE, "calliope_sram_2.bin", 0x32000) ||
	    !calliope_copy(dram, CAL_DRAM_SIZE, "calliope_dram_2.bin", 0x280000)) {
		return;
	}
	*(volatile uint32_t *)(sram + CAL_HNDSHK_OFF) = 0x656b6177;   /* hndshk_tag->suspend_wait_flag = "wake" */
	__asm__ volatile ("dsb sy" ::: "memory");
	/* the SysMMU on, IOVA 0x80000000 -> abox_rmem: a 16 KB first-level table 64 KB below the sound ring in the pool */
	pt_phys = pool_phys + pool_size - 0x30000;
	pt = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)pt_phys, 0x4000, VM_WIMG_WCOMB);
	if (pt == NULL) {
		snd_log("snd: calliope: cannot map the SysMMU table\n");
		return;
	}
	for (uint32_t i = 0; i < 4096; i++) {
		*(volatile uint32_t *)(pt + 4 * i) = 0;
	}
	for (uint32_t mb = 0; mb < CAL_DRAM_SIZE >> 20; mb++) {
		*(volatile uint32_t *)(pt + 4 * ((CAL_IOVA_DRAM >> 20) + mb)) = (uint32_t)((CAL_DRAM_PHYS + ((uint64_t)mb << 20)) >> 4) | 2;
	}
	*(volatile uint32_t *)(pt + 4 * (0x10000000U >> 20)) = (0x10000000U >> 4) | 2;
	/*
	 * A, 2026-11-06: Calliope's first access beyond its firmware faulted at 0xE0803000 (SysMMU INT 0x2): its silent log,
	 * IOVA_SILENT_LOG 0xE0000000 (abox.h:55), Samsung's reserved "exynos,abox_slog" of 0x2800000 (exynos9830.dts:2640;
	 * abox_dbg.c:139 maps it). Also its dump buffer, IOVA_DUMP_BUFFER 0xD0000000, "exynos,abox_dbg" 0x5600000
	 * (abox_dbg.c:392). Both in the low bank's free run after the framebuffer (UEFI's map: f2195000-100000000 free):
	 * the log at 0xF2200000 (1 MB aligned, cleared), the dump buffer from 0xFAA00000 to the 4 GB line, right after
	 * abox_rmem -- the same hole Samsung's sizes fill exactly.
	 */
	/*
	 * cpcall=1 (A, 2026-11-13, VoLTE voice): the S20's modem is external (S5123 on PCIe), so call audio does not use a
	 * VSS region in the modem's memory (the S20's cp_shmem has only IPC, MSI, PKTPROC) but ABOX's PCI path:
	 * abox-pci-rmem 0xF4200000 + 8 MB (dtb reserved-memory; abox_pci.c maps it at IOVA_VSS_PCI and grants the modem's
	 * HSI2 S2MPU access) and the modem's PCIe doorbell, s51xx_pcie dbaddr (pci_db_addr 0x14d60000) + 0x10000, 16 KB at
	 * IOVA_VSS_PCI_DOORBELL 0xA3000000 (abox_pci_doorbell_paddr_set). The silent log gives up 8 MB so the PCI region
	 * can sit where the modem's firmware expects it.
	 */
	uint32_t cpcall = 0, snd_arg = 0;
	PE_parse_boot_argn("cpcall", &cpcall, sizeof(cpcall));
	/* snd=3 (bit 1) is the same: the boot line has no room left for another argument */
	if (PE_parse_boot_argn("snd", &snd_arg, sizeof(snd_arg)) && (snd_arg & 2)) {
		cpcall = 1;
	}
	uint32_t slog_size = cpcall ? 0x2000000U : CAL_SLOG_SIZE;
	uint64_t pci_phys = cpcall ? 0xF4200000ULL : CAL_PCI_PHYS;
	{
		volatile uint8_t *slog = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)CAL_SLOG_PHYS, slog_size,
		    VM_WIMG_WCOMB);

		if (slog != NULL) {
			for (uint32_t i = 0; i < slog_size; i += 4) {
				*(volatile uint32_t *)(slog + i) = 0;
			}
			cal_slog = slog;
		}
		for (uint32_t mb = 0; mb < slog_size >> 20; mb++) {
			*(volatile uint32_t *)(pt + 4 * ((CAL_IOVA_SLOG >> 20) + mb)) = (uint32_t)((CAL_SLOG_PHYS + ((uint64_t)mb << 20)) >> 4) | 2;
		}
		for (uint32_t mb = 0; mb < CAL_DUMP_SIZE >> 20; mb++) {
			*(volatile uint32_t *)(pt + 4 * ((CAL_IOVA_DUMP >> 20) + mb)) = (uint32_t)((CAL_DUMP_PHYS + ((uint64_t)mb << 20)) >> 4) | 2;
		}
		/*
		 * A, 2026-11-06: next fault 0xA2000002, IOVA_VSS_PCI (abox.h:52; abox_pci.c:140 maps abox-pci-rmem, 8 MB,
		 * cleared). The voice-call regions (VSS firmware/parameter in the modem's shared memory, the PCI doorbell) have
		 * no modem behind them here: their 64 MB of IOVA all point at one cleared spare MB, the PCI 8 MB at its own.
		 */
		{
			volatile uint8_t *pci = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)pci_phys,
			    CAL_PCI_SIZE + (1U << 20), VM_WIMG_WCOMB);

			if (pci != NULL) {
				for (uint32_t i = 0; i < CAL_PCI_SIZE + (1U << 20); i += 4) {
					*(volatile uint32_t *)(pci + i) = 0;
				}
			}
			for (uint32_t mb = 0; mb < 64; mb++) {
				*(volatile uint32_t *)(pt + 4 * ((CAL_IOVA_VSS >> 20) + mb)) = (uint32_t)(CAL_VSS_SPARE_PHYS >> 4) | 2;
			}
			for (uint32_t mb = 0; mb < CAL_PCI_SIZE >> 20; mb++) {
				*(volatile uint32_t *)(pt + 4 * ((CAL_IOVA_PCI >> 20) + mb)) = (uint32_t)((pci_phys + ((uint64_t)mb << 20)) >> 4) | 2;
			}
			if (cpcall) {
				/* our own playback ring (RDMA3 is given its physical address, snd_dma 0xFEFF0000): once Calliope turns
				 * the SysMMU on, RDMA3's reads go through it too and fault (FAULT_VA 0xfeff0b40 seen): map that MB 1:1 */
				*(volatile uint32_t *)(pt + 4 * (0xFEF00000U >> 20)) = (uint32_t)(0xFEF00000ULL >> 4) | 2;
				/* the doorbell: a 1 KB second-level table right after the first-level one (exynos-iommu.h:
				 * mk_lv1ent_page(pa) = pa >> 4 | 1, lv2 small page pa >> 4 | 2, 256 entries, 1 KB aligned) */
				uint64_t l2_phys = pt_phys + 0x4000;
				volatile uint8_t *l2 = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)l2_phys, 0x1000,
				    VM_WIMG_WCOMB);

				if (l2 != NULL) {
					for (uint32_t i = 0; i < 256; i++) {
						*(volatile uint32_t *)(l2 + 4 * i) = 0;
					}
					for (uint32_t i = 0; i < 4; i++) {
						*(volatile uint32_t *)(l2 + 4 * i) = (uint32_t)((0x14d70000ULL + 0x1000ULL * i) >> 4) | 2;
					}
					*(volatile uint32_t *)(pt + 4 * (0xA3000000U >> 20)) = (uint32_t)(l2_phys >> 4) | 1;
				}
				snd_log("snd: calliope: cpcall: PCI region %#llx at IOVA %#x, modem doorbell 0x14d70000 at IOVA 0xA3000000%s\n",
				    pci_phys, CAL_IOVA_PCI, l2 ? "" : " (NOT mapped)");
			}
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	capa0 = rd(mmu, MMU_CAPA0_V7);
	if ((capa0 >> 11) & 1) {
		capa1 = rd(mmu, MMU_CAPA1_V7);
		vcr = (capa1 >> 14) & 1;
	}
	flpt = vcr ? 0x800c : 0x000c;
	inv = vcr ? 0x8010 : 0x0010;
	wr(mmu, MMU_CTRL, 0x7);                 /* CTRL_BLOCK while it is set up */
	wr(mmu, MMU_CFG, 0);
	if (vcr) {
		wr(mmu, 0x8004, 0);             /* REG_MMU_CFG_VM */
	}
	wr(mmu, flpt, (uint32_t)(pt_phys >> 12));
	wr(mmu, inv, 1);                        /* all TLBs invalidated */
	wr(mmu, MMU_CTRL, 0x5);                 /* CTRL_ENABLE */
	if (vcr) {
		wr(mmu, MMU_CTRL_VM, 0x5 | 0x8);        /* CTRL_ENABLE | CTRL_FAULT_STALL_MODE */
	}
	snd_log("snd: calliope: SysMMU on: CAPA0 %#x CAPA1 %#x (VCR %u), table at %#llx, CTRL %#x CTRL_VM %#x FLPT %#x\n", capa0, capa1,
	    vcr, pt_phys, rd(mmu, MMU_CTRL), rd(mmu, MMU_CTRL_VM), rd(mmu, flpt));
	/* DRAM on, then the cores: power, then enable (abox_core_power, abox_core_enable) */
	wr(abox, ABOX_SYSPOWER_CTRL, 1);
	wait_bits(abox, ABOX_SYSPOWER_STATUS, 1, 1, 10000);
	rx0 = *(volatile uint32_t *)(dram + CAL_IPC_RX_OFF);
	/*
	 * A, 2026-11-06, stage 2: Samsung's abox_msg_init sets up both message queues before the DSP runs (empty, and the
	 * data queue's length = the area less the headers). Without it the first run's queue length was 0.
	 */
	for (uint32_t q = 0; q < 2; q++) {
		volatile uint8_t *a = dram + (q ? CAL_IPC_RX_OFF : CAL_IPC_TX_OFF);

		*(volatile int32_t *)(a + 0) = 0;
		*(volatile int32_t *)(a + 4) = 0;
		*(volatile int32_t *)(a + CAL_Q_DATA + 0) = 0;
		*(volatile int32_t *)(a + CAL_Q_DATA + 4) = 0;
		*(volatile uint32_t *)(a + CAL_Q_DATA + 8) = CAL_IPC_AREA - CAL_Q_DATA_ELEM;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	upd(pmu, 0x3180, 1, 1);
	upd(pmu, 0x3200, 1, 1);
	IOSleep(2);
	upd(pmu, 0x31a0, 1, 1);
	upd(pmu, 0x3220, 1, 1);
	snd_log("snd: calliope: cores powered %#x/%#x, enabled %#x/%#x; IPC rx word 0 before: %#x\n", rd(pmu, 0x3180), rd(pmu, 0x3200),
	    rd(pmu, 0x31a0), rd(pmu, 0x3220), rx0);
	cal_dram = dram;
	cal_gpr = gpr;
	cal_timer = timer;
	cal_gicd = gicd;
	cal_pt = pt;
	cal_inv = inv;
	/* signs of life for 30 s: its messages as they come (every 20 ms); every 500 ms for the first 5 s the GPR, a fault */
	/* (snd_calliope=2: on to playback 1 s after its boot-done message; 10 s at most) */
	for (int tick = 1; tick <= 1500; tick++) {
		int t = tick / 25 - 1;

		IOSleep(20);
		calliope_drain(dram);
		if (cal_booted && !cal_timers) {
			/*
			 * A, run 13: Calliope sets timers 0 and 2 up (1 ms, mode 1) as its scheduler's tick and leaves them stopped;
			 * the AP starts them (abox_start_timer). START alone does not take: FLUSH | START does. Without it every
			 * wait in the firmware hangs (the mixer rate, PCM open after one message).
			 */
			cal_timers = true;
			wr(timer, 0x600 + 0x20 * 0, 0x4);       /* clear, then start (see calliope_timer_kick) */
			wr(timer, 0x600 + 0x20 * 0, 0x1);
			wr(timer, 0x600 + 0x20 * 2, 0x4);
			wr(timer, 0x600 + 0x20 * 2, 0x1);
			snd_log("snd: calliope: its tick timers started (timer 0 now %#x)\n", rd(timer, 0x600 + 0x10));
			calliope_timers("boot done");
		}
		if (snd_calliope >= 2 && ((cal_booted && tick >= 50) || tick >= 500)) {
			break;
		}
		if (tick % 25 != 0 || t >= 10) {
			continue;
		}
		snd_log("snd: calliope: +%d ms: GPR0 %08x %08x %08x %08x .. pc? %08x %08x; rx %08x %08x %08x %08x; SysMMU INT %#x FAULT_VA %#x\n",
		    (t + 1) * 500, rd(gpr, 0x0), rd(gpr, 0x4), rd(gpr, 0x8), rd(gpr, 0xc), rd(gpr, 0x3c), rd(gpr, 0x40),
		    *(volatile uint32_t *)(dram + CAL_IPC_RX_OFF), *(volatile uint32_t *)(dram + CAL_IPC_RX_OFF + 4),
		    *(volatile uint32_t *)(dram + CAL_IPC_RX_OFF + 8), *(volatile uint32_t *)(dram + CAL_IPC_RX_OFF + 12),
		    rd(mmu, 0x060), rd(mmu, vcr ? 0x1000 : 0x0070));
		/* Calliope's own words: the printable text in its silent log so far (the first 4 KB with something in them) */
		if (cal_slog != NULL) {
			char text[200];
			unsigned n = 0;

			for (uint32_t i = 0; i < 0x100000 && n + 1 < sizeof(text); i++) {
				char c = (char)cal_slog[i];

				if (c >= 0x20 && c < 0x7f) {
					text[n++] = c;
				} else if (c == '\n' && n > 0 && text[n - 1] != '|') {
					text[n++] = '|';
				}
			}
			text[n] = 0;
			if (n > 0) {
				snd_log("snd: calliope: its log: %s\n", text);
			}
		}
	}
}


/*
 * Stage 3 (snd_calliope=2): the AP's side of abox_msg_send + abox_ipc_send: the message (ABOX_IPC_MSG, 768 bytes) into
 * the tx data queue, its command (id = ipcid, cmd = msgtype, args) into the tx command queue, then SGI 15 to Calliope
 * through ABOX's GIC (abox_gic_generate_interrupt: GICD_SGIR = 1 << 16 | 15).
 */
#define CAL_MSG_BYTES           768U
#define CAL_IOVA_RDMA3          (0x91000000U + 0x100000U * RDMA_ID)     /* IOVA_RDMA_BUFFER(n) (abox.h:39) */

static bool
calliope_send(uint32_t ipcid, uint32_t msgtype, const uint32_t *p, unsigned np)
{
	static uint32_t m[CAL_MSG_BYTES / 4];
	volatile uint8_t *tx = cal_dram + CAL_IPC_TX_OFF, *c, *d;
	uint32_t len = *(volatile uint32_t *)(tx + CAL_Q_DATA + 8), size = 4 + CAL_MSG_BYTES, e, sidx, blank;
	int32_t cs, ce;

	bzero(m, sizeof(m));
	m[0] = ipcid;
	m[1] = (ipcid == 2 || ipcid == 3) ? (np > 0 ? p[0] : 0) : 0;  /* task_id = the channel for PCM messages */
	m[2] = msgtype;
	for (unsigned i = 0; i < np && i < 64; i++) {
		m[3 + i] = p[i];
	}
	for (int w = 0; ; w++) {
		e = *(volatile uint32_t *)(tx + CAL_Q_DATA + 4);
		sidx = *(volatile uint32_t *)(tx + CAL_Q_DATA + 0);
		if (sidx > e) {
			blank = sidx - e;
		} else {
			blank = len - e;
			if (blank < size) {
				e = 0;
				blank = sidx;
			}
		}
		cs = *(volatile int32_t *)(tx + 0);
		ce = *(volatile int32_t *)(tx + 4);
		if (blank >= size && cs != (ce + 1) % (int32_t)CAL_Q_CMD_LEN) {
			break;
		}
		if (w >= 100) {
			snd_log("snd: calliope: send %u/%u: the queue stays full (data s %u e %u, cmd s %d e %d)\n", ipcid, msgtype,
			    sidx, e, cs, ce);
			return false;
		}
		IOSleep(1);
	}
	d = tx + CAL_Q_DATA_ELEM + e;
	*(volatile uint32_t *)d = CAL_MSG_BYTES;
	for (uint32_t i = 0; i < CAL_MSG_BYTES / 4; i++) {
		*(volatile uint32_t *)(d + 4 + 4 * i) = m[i];
	}
	*(volatile uint32_t *)(tx + CAL_Q_DATA + 4) = (e + size) % len;
	c = tx + CAL_Q_CMD_ELEM + (uint32_t)ce * CAL_Q_CMD_SIZE;
	for (uint32_t i = 0; i < CAL_Q_CMD_SIZE; i += 4) {
		*(volatile uint32_t *)(c + i) = 0;
	}
	*(volatile uint32_t *)(c + 0) = ipcid;
	*(volatile uint32_t *)(c + 4) = msgtype;
	*(volatile uint32_t *)(c + 8) = np > 0 ? p[0] : 0;
	if (ipcid == 1 || ipcid == 0xA) {
		*(volatile uint32_t *)(c + 12) = np > 1 ? p[1] : 0;
		*(volatile uint32_t *)(c + 16) = np > 2 ? p[2] : 0;
	}
	*(volatile uint32_t *)(c + 20) = e;
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile int32_t *)(tx + 4) = (ce + 1) % (int32_t)CAL_Q_CMD_LEN;
	__asm__ volatile ("dsb sy" ::: "memory");
	/* run 4: a plain GICD_SGIR write is never taken (the GIC is secure); run 3: through EL3 the first one was */
	{
		uint64_t r = sfr_smc_write(CAL_GICD_PHYS + 0xf00, (1U << 16) | 15U);

		if (r != 0) {
			snd_log("snd: calliope: the SGI through EL3 answers %lld\n", (long long)r);
		}
	}
	/*
	 * A, run 3: the EL3 SGI (after a plain one not taken in 50 ms) was never taken; the first message was, late. Plain
	 * only now, and how long each took (up to 500 ms) is logged.
	 */
	{
		int i;

		for (i = 0; i < 500 && *(volatile int32_t *)(tx + 0) == cs; i++) {
			IOSleep(1);
		}
		snd_log("snd: calliope: sent %u/%u (%d): %s after %d ms\n", ipcid, msgtype, np > 0 ? (int)p[0] : -1,
		    *(volatile int32_t *)(tx + 0) != cs ? "taken" : "NOT taken", i);
	}
	(void)cal_sgi_smc;
	(void)cal_sgi_tested;
	return true;
}

/* its answer: what it sent in the next `ms' milliseconds, and how far it took our queue */
/*
 * Calliope's timers (B, 2026-11-06): ABOX_TIMER_*(x) at timer + 0x600 + 0x20 x: CTRL0 (+0, START 0 / FLUSH 1),
 * CTRL1 (+4, MODE), PRESET_LSB (+8), CURVALUD_LSB (+0x10) -- the registers Samsung's abox.c reads (timer_regmap,
 * abox_cpu_pm_ipc). Timer 1's PRESET_LSB is the firmware's WFI flag (-2 = in WFI, abox_cpu_suspend_complete).
 * On a fresh boot Samsung never starts them (abox_start_timer only on a wake from WFI): the firmware does.
 */
static uint32_t snd_cal_kick = 1, cal_kicks, cal_t0_last;
static void
calliope_timers(const char *what)
{
	volatile uint8_t *t = cal_timer;

	if (t == NULL) {
		return;
	}
	snd_log("snd: calliope: %s: timers CTRL0/CTRL1/PRESET/CUR: 0 %x/%x/%x/%x 1 %x/%x/%x/%x 2 %x/%x/%x/%x 3 %x/%x/%x/%x\n", what,
	    rd(t, 0x600), rd(t, 0x604), rd(t, 0x608), rd(t, 0x610), rd(t, 0x620), rd(t, 0x624), rd(t, 0x628), rd(t, 0x630),
	    rd(t, 0x640), rd(t, 0x644), rd(t, 0x648), rd(t, 0x650), rd(t, 0x660), rd(t, 0x664), rd(t, 0x668), rd(t, 0x670));
}

/* timer 0 (the firmware's 1 ms tick) set up (CTRL1 or PRESET non-zero) but not counting: FLUSH | START it again */
static void
calliope_timer_kick(void)
{
	volatile uint8_t *t = cal_timer;
	uint32_t now;

	if (t == NULL || !cal_booted || snd_cal_kick == 0) {
		return;
	}
	now = rd(t, 0x610);
	if (now == cal_t0_last && (rd(t, 0x604) != 0 || rd(t, 0x608) != 0)) {
		/*
		 * B, 2026-11-06 (live): a periodic timer whose interrupt was not cleared stops at 0, and neither START nor
		 * FLUSH|START takes it out of that; CTRL0 = 4 (the firmware's own "clear", 0x800421e4) then 1 (start) does.
		 * Calliope's RTX tick (timer 0 for core 0, IRQ 34; timer 2 for core 1, IRQ 36: OS_Tick_Setup at SRAM 0x868)
		 * then runs, and Calliope takes every message (PCM open .. trigger, all within 1 ms).
		 */
		wr(t, 0x600, 0x4);
		wr(t, 0x600, 0x1);
		if (rd(t, 0x644) != 0) {
			wr(t, 0x640, 0x4);
			wr(t, 0x640, 0x1);
		}
		if (++cal_kicks <= 5) {
			calliope_timers("timer 0 stopped: kicked");
		}
	}
	cal_t0_last = rd(t, 0x610);
}

static void
calliope_settle(const char *what, unsigned ms)
{
	volatile uint8_t *tx = cal_dram + CAL_IPC_TX_OFF;

	for (unsigned i = 0; i < ms / 10; i++) {
		IOSleep(10);
		calliope_drain(cal_dram);
	}
	calliope_log();
	calliope_timers(what);
	snd_log("snd: calliope: %s: tx cmd s %d e %d; RDMA3 CTRL0 %#x STATUS %#x BUF %#x-%#x; UAIF2 STATUS %#x; GPR %08x %08x pc? %08x %08x\n",
	    what, *(volatile int32_t *)(tx + 0), *(volatile int32_t *)(tx + 4), rd(abox, RDMA(RDMA_CTRL0)), rd(abox, RDMA(RDMA_STATUS)),
	    rd(abox, RDMA(RDMA_BUF_STR)), rd(abox, RDMA(RDMA_BUF_END)), rd(abox, ABOX_UAIF_STATUS),
	    cal_gpr ? rd(cal_gpr, 0x0) : 0, cal_gpr ? rd(cal_gpr, 0x4) : 0, cal_gpr ? rd(cal_gpr, 0x3c) : 0,
	    cal_gpr ? rd(cal_gpr, 0x40) : 0);
	/* abox_soc_3.h: ABOX_CA32_CORE0_R(x) 0x5000 + 4x, PC = R(31) at 0x7c; core 1 at 0x5080; status 0x5100 */
	if (cal_gpr != NULL) {
		for (int pass = 0; pass < 3; pass++) {
			snd_log("snd: calliope: %s: core0 pc %08x r28-30 %08x %08x %08x r0-3 %08x %08x %08x %08x; core1 pc %08x; status %08x\n",
			    what, rd(cal_gpr, 0x7c), rd(cal_gpr, 0x70), rd(cal_gpr, 0x74), rd(cal_gpr, 0x78), rd(cal_gpr, 0x0),
			    rd(cal_gpr, 0x4), rd(cal_gpr, 0x8), rd(cal_gpr, 0xc), rd(cal_gpr, 0x80 + 0x7c), rd(cal_gpr, 0x100));
			IOSleep(3);
		}
	}
	{
		/* ABOX's GIC as EL3 reads it: groups, enables, pending, active for IRQs 0-127 */
		static const uint32_t offs[] = { 0x080, 0x084, 0x088, 0x08c, 0x100, 0x104, 0x108, 0x10c, 0x200, 0x204, 0x208, 0x20c,
			                         0x300, 0x304, 0x308, 0x30c };
		uint32_t v[16];

		for (int i = 0; i < 16; i++) {
			if (!sfr_smc_read(CAL_GICD_PHYS + offs[i], &v[i])) {
				v[i] = 0xdeadbeef;
			}
		}
		snd_log("snd: calliope: %s: GIC group %08x %08x %08x %08x en %08x %08x %08x %08x pend %08x %08x %08x %08x "
		    "act %08x %08x %08x %08x\n", what, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
		    v[12], v[13], v[14], v[15]);
	}
	if (rd(mmu, 0x060) != 0) {
		/* only INT and FAULT_VA (0x1000), as the boot-time line reads them: 0x0070 is no register on this SysMMU and its
		 * read is a synchronous external abort (A, 2026-11-13: kernel data abort ESR 0x96000210 far ...0070, twice,
		 * the first time Calliope faulted after START_VSS) */
		snd_log("snd: calliope: %s: SysMMU fault: INT %#x FAULT_VA %#x\n", what, rd(mmu, 0x060), rd(mmu, 0x1000));
	}
}

/*
 * The RDMA "update trigger" (Bentlybro/z3s-mainline-linux docs/AUDIO_DSP.md, B 2026-11-06): the firmware's
 * __rdma_primcom_source_update (VA 0x8000bd40, the same build as ours) returns "update trigger not ready" unless
 * (word & 0x1111) == 0x1111, word = DSP VA 0x81004038 + rdma * 0x170 + 0xa8 (0x81004530 for RDMA3; checked in our
 * calliope_dram.bin: movw 0x4038 / movt 0x8100, mla 0x170, ldr [#0xa8], movw 0x1111). Nothing we send sets it: set it,
 * and again whenever the firmware clears it. snd_cal_trig=0 leaves it alone.
 */
static uint32_t snd_cal_trig = 1, cal_trig_sets;
static void
calliope_trigger_word(void)
{
	volatile uint32_t *w;

	if (cal_dram == NULL || !cal_booted || snd_cal_trig == 0) {
		return;
	}
	w = (volatile uint32_t *)(cal_dram + 0x1004038U + RDMA_ID * 0x170U + 0xa8U);
	if ((*w & 0x1111U) != 0x1111U) {
		uint32_t was = *w;

		*w = was | 0x1111U;
		__asm__ volatile ("dsb sy" ::: "memory");
		if (++cal_trig_sets <= 5) {
			snd_log("snd: calliope: RDMA%u update trigger %#x -> %#x (set %u)\n", RDMA_ID, was, *w, cal_trig_sets);
		}
	}
}

/* abox_rdma.c's order: open, set_buffer + hw_params, prepare, trigger -- for RDMA3, our ring behind IOVA_RDMA_BUFFER(3) */
static bool
calliope_play(void)
{
	uint32_t ch = RDMA_ID, iova = CAL_IOVA_RDMA3 + (uint32_t)(buf_phys & 0xfffffU);
	uint32_t a[4];

	if (cal_dram == NULL || !cal_booted) {
		snd_log("snd: calliope: not booted: no playback\n");
		return false;
	}
	*(volatile uint32_t *)(cal_pt + 4 * (CAL_IOVA_RDMA3 >> 20)) = (uint32_t)((buf_phys & ~0xfffffULL) >> 4) | 2;
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(mmu, cal_inv, 1);
	bzero(buf, BUF_BYTES);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (snd_calliope >= 3) {
		snd_log("snd: calliope: snd_calliope=3: no messages from here; kern.redwood.snd_poke drives it (ring IOVA %#x)\n",
		    iova);
		return true;
	}
	/*
	 * ABOX_RESUME as abox_boot_done_work -> abox_cpu_pm_ipc: bundle.param_u64[0] = the AP's clock in ns,
	 * [1] = ABOX timer 1 (26 MHz) in ns (atime * 500 / 13). Off with snd_cal_resume=0.
	 */
	a[0] = 1;
	PE_parse_boot_argn("snd_cal_resume", a, sizeof(a[0]));
	if (a[0] != 0) {
		uint64_t kt, at = 0;
		uint32_t r[7] = { 0, 0, 0, 0, 0, 0, 0 };

		absolutetime_to_nanoseconds(mach_absolute_time(), &kt);
		if (cal_timer != NULL) {
			at = ((uint64_t)rd(cal_timer, 0x600 + 0x20 + 0x14) << 32) | rd(cal_timer, 0x600 + 0x20 + 0x10);
			at = at * 500 / 13;
		}
		r[3] = (uint32_t)kt; r[4] = (uint32_t)(kt >> 32); r[5] = (uint32_t)at; r[6] = (uint32_t)(at >> 32);
		(void)calliope_send(1, 2, r, 7);        /* ABOX_RESUME */
		calliope_settle("resume", 100);
	}
	calliope_log();
	/* the mixer's side, as abox_cmpnt.c's controls do: SIFS0 48 kHz, 32-bit stereo (IPC_ABOX_CONFIG) */
	/* (run 5: SET_SIFS0_RATE hung its IPC thread at "Changing MIXER sample rate": only with snd_cal_cfg=1) */
	if (PE_parse_boot_argn("snd_cal_cfg", a, sizeof(a[0])) && a[0] != 0) {
		a[0] = 48000;
		(void)calliope_send(0xA, 1, a, 1);      /* SET_SIFS0_RATE */
		a[0] = 32; a[1] = 2;
		(void)calliope_send(0xA, 0x10, a, 2);   /* SET_SIFS0_FORMAT */
		calliope_settle("sifs0", 50);
	}
	a[0] = ch;
	(void)calliope_send(2, 11, a, 1);               /* PCM_PLTDAI_OPEN */
	calliope_settle("open", 100);
	a[0] = ch; a[1] = iova; a[2] = PERIOD_BYTES; a[3] = BUF_BYTES / PERIOD_BYTES;
	(void)calliope_send(2, 20, a, 4);               /* PCM_SET_BUFFER: phyaddr, period bytes, periods */
	calliope_settle("set_buffer", 50);
	{
		uint32_t h[5] = { ch, 48000, 16, 2, 0 };

		(void)calliope_send(2, 14, h, 5);       /* PCM_PLTDAI_HW_PARAMS: rate, width, channels, packed */
	}
	calliope_settle("hw_params", 100);
	a[0] = ch;
	(void)calliope_send(2, 16, a, 1);               /* PCM_PLTDAI_PREPARE */
	calliope_settle("prepare", 100);
	a[0] = ch; a[1] = 1;
	(void)calliope_send(2, 17, a, 2);               /* PCM_PLTDAI_TRIGGER 1 */
	calliope_trigger_word();
	calliope_settle("trigger", 200);
	calliope_trigger_word();
	snd_log("snd: calliope: playback asked for (ring IOVA %#x = %#llx); %u pointer messages so far\n", iova, buf_phys,
	    cal_pointers);
	return true;
}

/*
 * ---- the debug port (A, 2026-11-06): experiments without a reboot ------------------------------------------------
 * kern.redwood.snd_poke (root; write a command, read the answer). From the Mac: ssh s20 rwsys kern.redwood.snd_poke 'CMD'
 *   r PA [N]          N words (at most 64) at physical PA            w PA V        one word
 *   sr PA / sw PA V   the same through EL3 (Samsung's SMC_CMD_REG: the secure GIC and such)
 *   send ID TYPE [a0..a6]   one message to Calliope (ABOX_IPC_MSG: ipcid, msgtype, then the words after msgtype),
 *                     then 200 ms of its messages and log into the kernel log; the answer says if it was taken
 *   log               Calliope's log now                             ack           the AP's GIC acknowledged now
 * Physical pages are mapped once and kept (32 at most).
 */
static void
dbg_snd_send(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	uint32_t a[8] = { 0 };
	int32_t before;

	if (cal_dram == NULL || n < 2) {
		snprintf(out, cap, cal_dram == NULL ? "Calliope is not up\n" : "snd.send ID TYPE [a0..a7]\n");
		return;
	}
	before = *(volatile int32_t *)(cal_dram + CAL_IPC_TX_OFF);
	for (unsigned i = 2; i < n && i < 10; i++) {
		a[i - 2] = (uint32_t)v[i];
	}
	cal_poker = current_thread();
	cal_poking = true;
	(void)calliope_send((uint32_t)v[0], (uint32_t)v[1], a, n > 2 ? n - 2 : 0);
	calliope_settle("poke", 200);
	cal_poking = false;
	snprintf(out, cap, "sent %llu/%llu: %s (tx s %d -> %d, e %d); its words are in the log\n", v[0], v[1],
	    *(volatile int32_t *)(cal_dram + CAL_IPC_TX_OFF) != before ? "taken" : "NOT taken", before,
	    *(volatile int32_t *)(cal_dram + CAL_IPC_TX_OFF), *(volatile int32_t *)(cal_dram + CAL_IPC_TX_OFF + 4));
}

static void
dbg_snd_log(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	calliope_log();
	snprintf(out, cap, "Calliope's new lines are in the kernel log\n");
}

static void
dbg_snd_state(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	uint32_t st = rd(abox, RDMA(RDMA_STATUS));

	snprintf(out, cap, "RDMA%u CTRL0 %08x STATUS %08x (progress %u, offset %#x, count %u); UAIF2 CTRL0 %08x CTRL1 %08x STATUS %08x; "
	    "ROUTE0 %08x FC0 %08x SPUS1 %08x; amps %s; frames from iOS %llu; Calliope %s, %u messages, %u pointers\n", RDMA_ID,
	    rd(abox, RDMA(RDMA_CTRL0)), st, st >> 31, ((st >> 12) & 0xffff) << 4, st & 0xfff, rd(abox, ABOX_UAIF_CTRL0),
	    rd(abox, ABOX_UAIF_CTRL1), rd(abox, ABOX_UAIF_STATUS), rd(abox, ABOX_ROUTE_CTRL0), rd(abox, ABOX_SPUS_CTRL_FC0),
	    rd(abox, ABOX_SPUS_CTRL1), amps_on ? "on" : amps_ok ? "ready" : "missing", ios_frames, cal_booted ? "up" : "down",
	    cal_msgs, cal_pointers);
}

static void
dbg_snd_amp(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	uint32_t x = 0;
	struct amp *a;

	if (n < 2 || v[0] > 1) {
		snprintf(out, cap, "snd.amp SIDE(0 left/top, 1 right/bottom) REG [VALUE]\n");
		return;
	}
	a = &amps[v[0]];
	if (n >= 3) {
		bool ok = amp_write(a, (uint32_t)v[1], (uint32_t)v[2]);

		(void)amp_read(a, (uint32_t)v[1], &x);
		snprintf(out, cap, "%s %#llx <- %#x: %s, reads %#x\n", a->name, v[1], (uint32_t)v[2], ok ? "ok" : "FAILED", x);
	} else {
		bool ok = amp_read(a, (uint32_t)v[1], &x);

		snprintf(out, cap, "%s %#llx: %#010x%s\n", a->name, v[1], x, ok ? "" : " (no answer)");
	}
}

/*
 * snd.cal 0|1|2|3 (B, 2026-11-06): Calliope driven live, no reboot -- 1 boots it now (firmware from /fw, its own
 * IPC, tick timers), 2 asks it for playback (open .. trigger on RDMA0), 3 says its timers, 0 halts it (cores off,
 * SysMMU off). The loader stops a Calliope left running at every power-on, and the run's end halts it too.
 */
static void calliope_halt(volatile uint8_t *p, volatile uint8_t *m);
static void
dbg_snd_cal(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	uint32_t what = n >= 1 ? (uint32_t)v[0] : 3;

	if (what == 0) {
		calliope_halt(pmu, mmu);
		cal_booted = false;
		cal_timers = false;
		cal_dram = NULL;
		snprintf(out, cap, "Calliope halted (cores %#x/%#x)\n", rd(pmu, 0x31a0), rd(pmu, 0x3220));
	} else if (what == 1) {
		if (cal_booted) {
			snprintf(out, cap, "Calliope is up already (0 halts it)\n");
			return;
		}
		snd_calliope = 2;
		snd_rdma = 0;
		cal_poker = current_thread();
		cal_poking = true;
		calliope_boot();
		cal_poking = false;
		snprintf(out, cap, "Calliope %s; %u messages; its log and timers are in the kernel log\n",
		    cal_booted ? "booted" : "NOT booted", cal_msgs);
	} else if (what == 2) {
		bool ok;

		cal_poker = current_thread();
		cal_poking = true;
		ok = calliope_play();
		cal_poking = false;
		snprintf(out, cap, "playback %s; RDMA0 CTRL0 %08x STATUS %08x; %u messages, %u pointers\n", ok ? "asked for" : "NOT asked",
		    rd(abox, 0x1000), rd(abox, 0x1030), cal_msgs, cal_pointers);
	} else {
		volatile uint8_t *t = cal_timer;

		if (t == NULL) {
			snprintf(out, cap, "Calliope was not booted this run (no timer mapping)\n");
			return;
		}
		/* (timers 0-3 only: Samsung's timer_regmap names no more; a read of a hole can hang the bus) */
		snprintf(out, cap, "timers CTRL1/PRESET/CUR: 0 %x/%x/%x 1 %x/%x/%x 2 %x/%x/%x; kicks %u; booted %d, %u msgs\n",
		    rd(t, 0x604), rd(t, 0x608), rd(t, 0x610), rd(t, 0x624), rd(t, 0x628), rd(t, 0x630), rd(t, 0x644), rd(t, 0x648),
		    rd(t, 0x650), cal_kicks, cal_booted, cal_msgs);
	}
}


/* ---- the phone's microphones: diagnostics and iOS input, hardware enabled only while recording ------------------ */
/*
 * Samsung's path (Android's media-mic; reference run under the stock kernel, logs/runs/resume-20261107): DMIC1 ->
 * VTS's DMIC_AUD0 (0x15350000) -> its serial LIF (0x15340000) -> ABOX UAIF6 -> NSRC4 -> WDMA4. VTS sits in its own
 * power domain (PMU 0x15862d00, need_smc 0x15410204), clocked from CMU_APM (CLKCMU_VTS_BUS) and reached through BUS1's
 * LHS_AXI_P_VTS / BAAW_P_VTS. An access to VTS with any of that off hangs the bus (a read of CMU_VTS froze the phone,
 * 2026-11-07), so: mic.look reads only always-on blocks; mic.bus opens the bridges; mic.pd powers the domain and asks
 * EL3 to open it (DTZPC); mic.cmu reads CMU_VTS only after both.
 */
#define PMU_PHYS                0x15860000U
#define PMU_VTS_CONF            0x2d00U
#define PMU_VTS_STATUS          0x2d04U
#define VTS_NEED_SMC            0x15410204U
#define CMU_APM_PHYS            0x15800000U
#define CMU_BUS1_PHYS           0x1a400000U
#define CMU_VTS_PHYS            0x15500000U

static volatile uint8_t *mic_pmu, *mic_apm, *mic_bus1, *mic_cmu_vts;
static bool mic_bus_open, mic_pd_open;
static volatile bool mic_ios_wants, mic_hw_busy;
static thread_t mic_worker;

static bool
mic_debug_allowed(char *out, size_t cap)
{
	if ((mic_ios_wants || mic_hw_busy) && current_thread() != mic_worker) {
		snprintf(out, cap, "mic: iOS owns capture; stop recording before changing the path (mic.peek is read-only)\n");
		return false;
	}
	return true;
}

static const struct { const char *name; uint32_t off; } mic_apm_regs[] = {
	{ "MUX_CLKCMU_VTS_BUS", 0x1004 }, { "DIV_CLKCMU_VTS_BUS", 0x1804 }, { "GATE_CLKCMU_VTS_BUS", 0x2010 },
	{ "LHM_AXI_C_VTS", 0x2044 }, { "LHS_AXI_LP_VTS", 0x205c }, { "MAILBOX_APM_VTS", 0x2064 },
	{ "QCH LHM_AXI_C_VTS", 0x3060 }, { "QCH LHS_AXI_LP_VTS", 0x3078 }, { "QCH MAILBOX_APM_VTS", 0x3080 },
}, mic_bus1_regs[] = {
	{ "BAAW_P_VTS", 0x2034 }, { "LHM_AXI_D_VTS", 0x2084 }, { "LHS_AXI_P_VTS", 0x20b4 },
	{ "QCH BAAW_P_VTS", 0x3088 }, { "QCH LHM_AXI_D_VTS", 0x30d8 }, { "QCH LHS_AXI_P_VTS", 0x3108 },
}, mic_vts_regs[] = {
	{ "VTS_AUD0_USER", 0x600 }, { "VTS_AUD1_USER", 0x610 }, { "VTS_BUS_USER", 0x620 }, { "VTS_RCO_USER", 0x630 },
	{ "MUX_VTS_BUS", 0x1000 }, { "MUX_DMIC_AUD", 0x1004 }, { "MUX_DMIC_IF", 0x1008 }, { "MUX_SERIAL_LIF", 0x100c },
	{ "DIV_VTS_BUS", 0x1800 }, { "DIV_DMIC_AUD", 0x1804 }, { "DIV_DMIC_AUD_DIV2", 0x1808 },
	{ "DIV_DMIC_AUD_PAD", 0x180c }, { "DIV_SERIAL_LIF", 0x181c },
	{ "GAT DMIC_AUD0_CLK", 0x2058 }, { "GAT DMIC_AUD0_DIV2", 0x205c }, { "GAT DMIC_AUD0_PCLK", 0x2060 },
	{ "GAT RST_DMIC_AUD", 0x20fc }, { "GAT RST_SERIAL_LIF", 0x2104 }, { "GAT SLIF_BCLK", 0x2108 },
	{ "GAT SLIF_CLK", 0x210c }, { "GAT SLIF_HCLK", 0x2110 }, { "GAT SLIF_PCLK", 0x2114 },
	{ "GAT PAD_CLK0", 0x211c }, { "QCH DMIC_AUD0_PCLK", 0x307c }, { "QCH DMIC_AUD0_DMIC", 0x3000 },
};

static bool
mic_map(void)
{
	if (mic_pmu == NULL) {
		mic_pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);
		mic_apm = (volatile uint8_t *)ml_io_map(CMU_APM_PHYS, 0x4000);
		mic_bus1 = (volatile uint8_t *)ml_io_map(CMU_BUS1_PHYS, 0x4000);
	}
	return mic_pmu != NULL && mic_apm != NULL && mic_bus1 != NULL;
}

static size_t
mic_dump(char *out, size_t cap, size_t at, volatile uint8_t *base, const char *block,
    const struct { const char *name; uint32_t off; } *r, unsigned n)
{
	for (unsigned i = 0; i < n && at < cap; i++) {
		at += (size_t)snprintf(out + at, cap - at, "%s %-20s +%#06x %08x\n", block, r[i].name, r[i].off,
		    rd(base, r[i].off));
	}
	return at;
}

/* mic.look: the VTS domain and its bridges, read from always-on blocks only */
static void
dbg_mic_look(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	size_t at;

	if (!mic_map()) {
		snprintf(out, cap, "mic: no mappings\n");
		return;
	}
	at = (size_t)snprintf(out, cap, "PMU VTS_CONFIGURATION %08x STATUS %08x; bus %s, domain %s\n",
	    rd(mic_pmu, PMU_VTS_CONF), rd(mic_pmu, PMU_VTS_STATUS), mic_bus_open ? "opened" : "as found",
	    mic_pd_open ? "opened" : "as found");
	at = mic_dump(out, cap, at, mic_apm, "APM ", (const void *)mic_apm_regs, sizeof(mic_apm_regs) / sizeof(mic_apm_regs[0]));
	(void)mic_dump(out, cap, at, mic_bus1, "BUS1", (const void *)mic_bus1_regs, sizeof(mic_bus1_regs) / sizeof(mic_bus1_regs[0]));
}

/* a gate on (manual and on, or no longer automatic), a Q-channel's clock requested */
static void
mic_open_reg(volatile uint8_t *base, uint32_t off)
{
	uint32_t was = rd(base, off);

	if (off >= 0x3000) {
		wr(base, off, (was & ~QCH_ENABLE) | QCH_CLOCK_REQ);
	} else if (off >= 0x2000) {
		wr(base, off, (was & GATE_MANUAL) ? (was | GATE_CG_VAL) : (was & ~GATE_AUTO));
	}
}

/* mic.bus: CMU_APM's VTS bus clock and the bridges on (gates and Q-channels only; muxes and dividers as found) */
static void
dbg_mic_bus(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	if (!mic_debug_allowed(out, cap)) {
		return;
	}

	if (!mic_map()) {
		snprintf(out, cap, "mic: no mappings\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(mic_apm_regs) / sizeof(mic_apm_regs[0]); i++) {
		mic_open_reg(mic_apm, mic_apm_regs[i].off);
	}
	for (unsigned i = 0; i < sizeof(mic_bus1_regs) / sizeof(mic_bus1_regs[0]); i++) {
		mic_open_reg(mic_bus1, mic_bus1_regs[i].off);
	}
	mic_bus_open = true;
	dbg_mic_look(v, 0, out, cap);
}

/* mic.pd: the VTS power domain on (Samsung's vts_on: CONFIGURATION bit 0, wait STATUS bit 0), then EL3's DTZPC */
static void
dbg_mic_pd(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	if (!mic_debug_allowed(out, cap)) {
		return;
	}

	uint32_t st = 0;
	uint64_t ret = ~0ULL;
	unsigned t;

	if (!mic_map() || !mic_bus_open) {
		snprintf(out, cap, "mic: mic.bus first\n");
		return;
	}
	mic_pd_open = false;
	if (!(rd(mic_pmu, PMU_VTS_STATUS) & 1)) {
		wr(mic_pmu, PMU_VTS_CONF, rd(mic_pmu, PMU_VTS_CONF) | 1);
	}
	for (t = 0; t < 100 && !((st = rd(mic_pmu, PMU_VTS_STATUS)) & 1); t++) {
		IOSleep(1);
	}
	if (st & 1) {
		ret = smc4(SMC_PREPARE_PD_ONOFF, 1, VTS_NEED_SMC, 2);
		mic_pd_open = true;
	}
	snprintf(out, cap, "mic: VTS domain %s after %u ms (STATUS %08x); DTZPC restore answers %#llx\n",
	    (st & 1) ? "on" : "NOT on", t, st, ret);
}

/* mic.cmu: CMU_VTS's clocks for the microphone path, read (only once the domain is on and open) */
static void
dbg_mic_cmu(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	if (!mic_debug_allowed(out, cap)) {
		return;
	}

	if (!mic_pd_open) {
		snprintf(out, cap, "mic: mic.bus and mic.pd first (CMU_VTS hangs the bus otherwise)\n");
		return;
	}
	if (mic_cmu_vts == NULL) {
		mic_cmu_vts = (volatile uint8_t *)ml_io_map(CMU_VTS_PHYS, 0x4000);
	}
	(void)mic_dump(out, cap, 0, mic_cmu_vts, "VTS ", (const void *)mic_vts_regs, sizeof(mic_vts_regs) / sizeof(mic_vts_regs[0]));
}


/*
 * mic.clk: the microphone's clocks as Samsung's vts_s_lif sets them for 48 kHz / 24 bits (vts_s_lif_clk_table.c):
 * CMU_AUD's DIV_CLK_AUD_DMIC0 (0x1818) = PLL_AUD0 / 16 = 73.728 MHz, its gate (0x2000) on; CMU_VTS AUD0_USER (0x600 bit 4)
 * from it; DMIC_AUD mux 1 (AUD0) / 6 = 12.288 MHz, PAD and DIV2 / 4 = 3.072 MHz; SERIAL_LIF mux 1 / 8 = 9.216 MHz.
 */
#define VTS_SYS_PHYS            0x15511000U     /* sys_sfr at +0x10: SEL_PAD_AUD, +0x14: SEL_DIV2_CLK */
#define VTS_DMIC_AUD0_PHYS      0x15350000U
#define VTS_SLIF_PHYS           0x15340000U
#define VTS_GPIO_PHYS           0x15580000U     /* gpv0 at +0: 0 the DMIC clock, 3 its data (function 2, pull-down) */

static volatile uint8_t *mic_sys, *mic_dmic, *mic_slif;
static volatile uint8_t *mic_dmic_extra[2];
static bool mic_front_ready;

static void
mic_div(volatile uint8_t *base, uint32_t off, uint32_t ratio)
{
	wr(base, off, (rd(base, off) & ~0x1ffU) | ratio);
	for (unsigned t = 0; t < 100 && (rd(base, off) & (1U << 16)); t++) {
		delay(10);            /* BUSY */
	}
}

static void
dbg_mic_clk(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	if (!mic_debug_allowed(out, cap)) {
		return;
	}

	if (!mic_pd_open || mic_cmu_vts == NULL || cmu == NULL) {
		snprintf(out, cap, "mic: mic.bus, mic.pd, mic.cmu first\n");
		return;
	}
	/* Samsung's cal_dll_apm_enable (vts_start_runtime_resume): DLL_APM locked, CMU_APM's MUX_DLL_USER and the VTS bus on it */
	{
		volatile uint8_t *dll = (volatile uint8_t *)ml_io_map(0x158b0000, 0x1000);
		volatile uint8_t *sys_apm = (volatile uint8_t *)ml_io_map(0x15820000, 0x1000);

		if (dll != NULL && sys_apm != NULL && mic_apm != NULL) {
			wr(dll, 0x4, 0x726);
			wr(sys_apm, 0x440, 0x111);
			for (unsigned t = 0; t < 1000 && !(rd(sys_apm, 0x444) & 1); t++) {
				delay(10);
			}
			mic_div(mic_apm, 0x640, rd(mic_apm, 0x640) | (1U << 4));
			mic_div(mic_apm, 0x1004, rd(mic_apm, 0x1004) | 1U);
		}
	}
	mic_div(cmu, 0x1818, 15);                               /* DIV_CLK_AUD_DMIC0: PLL_AUD0 / 16 */
	mic_open_reg(cmu, 0x2000);                              /* CLK_AUD_DMIC0 */
	wr(mic_cmu_vts, 0x600, rd(mic_cmu_vts, 0x600) | (1U << 4));     /* VTS_AUD0_USER: CLK_AUD_DMIC0 */
	delay(10);
	mic_div(mic_cmu_vts, 0x1004, 1);                        /* MUX_DMIC_AUD: AUD0 */
	mic_div(mic_cmu_vts, 0x1804, 5);                        /* DIV_DMIC_AUD: / 6 */
	mic_div(mic_cmu_vts, 0x180c, 3);                        /* DIV_DMIC_AUD_PAD: / 4 */
	mic_div(mic_cmu_vts, 0x1808, 3);                        /* DIV_DMIC_AUD_DIV2: / 4 */
	mic_div(mic_cmu_vts, 0x100c, 1);                        /* MUX_SERIAL_LIF: AUD0 */
	mic_div(mic_cmu_vts, 0x181c, 7);                        /* DIV_SERIAL_LIF: / 8 */
	snprintf(out, cap, "mic: AUD DMIC0 div %08x gate %08x; VTS AUD0_USER %08x MUX_DMIC_AUD %08x DIV %08x PAD %08x DIV2 %08x "
	    "MUX_SLIF %08x DIV_SLIF %08x\n", rd(cmu, 0x1818), rd(cmu, 0x2000), rd(mic_cmu_vts, 0x600), rd(mic_cmu_vts, 0x1004),
	    rd(mic_cmu_vts, 0x1804), rd(mic_cmu_vts, 0x180c), rd(mic_cmu_vts, 0x1808), rd(mic_cmu_vts, 0x100c),
	    rd(mic_cmu_vts, 0x181c));
}

/*
 * mic.slif: the pins, DMIC_AUD0 and the serial LIF with the values of Samsung's run (vts_s_lif_soc.c, VTS_S_SOC
 * 1.1.1: DMIC_AUD control 0xA0030000, CONTROL_DMIC_AUD 0x8C63004C, GAIN_MODE 0x1717, MAX_SCALE_GAIN 0x85; SEL_PAD_AUD 7,
 * SEL_DIV2_CLK 0x38; the S_LIF registers as read during the stock kernel's capture). Then the sample counter twice.
 */
static void
dbg_mic_slif(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	if (!mic_debug_allowed(out, cap)) {
		return;
	}

	volatile uint8_t *gpv0;
	uint32_t c0, c1;
	uint32_t settle_ms = current_thread() == mic_worker ? 0 : 100;

	if (!mic_pd_open || mic_cmu_vts == NULL) {
		snprintf(out, cap, "mic: mic.bus, mic.pd, mic.cmu, mic.clk first\n");
		return;
	}
	if (mic_sys == NULL) {
		mic_sys = (volatile uint8_t *)ml_io_map(VTS_SYS_PHYS, 0x1000);
		mic_dmic = (volatile uint8_t *)ml_io_map(VTS_DMIC_AUD0_PHYS, 0x1000);
		mic_slif = (volatile uint8_t *)ml_io_map(VTS_SLIF_PHYS, 0x1000);
	}
	for (unsigned i = 0; i < 2; i++) {
		if (mic_dmic_extra[i] == NULL) {
			mic_dmic_extra[i] = (volatile uint8_t *)ml_io_map(VTS_DMIC_AUD0_PHYS + (i + 1) * 0x10000, 0x1000);
		}
	}
	if (mic_sys == NULL || mic_dmic == NULL || mic_slif == NULL ||
	    mic_dmic_extra[0] == NULL || mic_dmic_extra[1] == NULL) {
		snprintf(out, cap, "mic: cannot map all three DMIC converters\n");
		return;
	}
	gpv0 = redwood_gpio_bank(VTS_GPIO_PHYS, 0x000);
	if (gpv0 != NULL) {
		redwood_gpio_func(gpv0, 0, 2);
		redwood_gpio_pull(gpv0, 0, 1);
		redwood_gpio_func(gpv0, 3, 2);
		redwood_gpio_pull(gpv0, 3, 1);
	}
	/* the main microphone's 1.8 V (MICBIAS1: regulator-fixed main_micbias_1p8, gpp4-1 high; Android's DMIC1 -> MICBIAS1) */
	{
		volatile uint8_t *gpp4 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, PINCTRL_PERIC0_GPP4);

		if (gpp4 != NULL) {
			redwood_gpio_func(gpp4, 1, 1);
			redwood_gpio_set(gpp4, 1, 1);
		}
	}
	/* VTS's DMIC_IF: the AUD path's DMIC_AUD0..2 on (vts_s_lif_dmic_if_en: sfr +0x1000 bits 6..8) */
	wr(mic_sys, 0x00, rd(mic_sys, 0x00) | 0x1c0);
	/* UAIF6's bit clock from the serial LIF (CMU_AUD MUX_CLK_AUD_UAIF6 = AUDIO_LIF_BCLKI, its divider / 1) */
	wr(cmu, 0x1024, 1);
	wr(cmu, 0x1844, 0);
	wr(mic_sys, 0x10, 7);                   /* SEL_PAD_AUD */
	wr(mic_sys, 0x14, 0x38);                /* SEL_DIV2_CLK */
	/* INPUT_EN=7 needs all three converters producing valid frames, even with only mic 1 powered. */
	for (unsigned i = 0; i < 3; i++) {
		volatile uint8_t *d = i == 0 ? mic_dmic : mic_dmic_extra[i - 1];

		wr(d, 0x04, 0x8C63004CU);
		wr(d, 0x08, 0x1717);
		wr(d, 0x10, 0x85);
		wr(d, 0x00, 0xA0030000U);
	}
	mic_front_ready = true;
	wr(mic_slif, 0x104, 0x2017);            /* CONFIG_MASTER */
	wr(mic_slif, 0x108, 0x2017);            /* CONFIG_SLAVE */
	wr(mic_slif, 0x110, 0xffffffffU);       /* SAMPLE_PCNT */
	wr(mic_slif, 0x114, 7);                 /* INPUT_EN */
	for (unsigned i = 0; i < 4; i++) {
		wr(mic_slif, 0x118 + 4 * i, 0x800000);  /* VOL_SET */
		wr(mic_slif, 0x128 + 4 * i, 1);         /* VOL_CHANGE */
	}
	wr(mic_slif, 0x138, 0x76543210U);       /* CHANNEL_MAP */
	wr(mic_slif, 0x100, 0x101);             /* CTRL: both interfaces */
	wr(mic_slif, 0x10c, 0x11);              /* CONFIG_DONE */
	c0 = rd(mic_slif, 0x48);         /* (INT_PEND: 0x150/0x154 are not in Samsung's readable list) */
	if (settle_ms != 0) {
		IOSleep(settle_ms);
	}
	c1 = rd(mic_slif, 0x48);
	snprintf(out, cap, "mic: gpv0 %s; SEL_PAD %08x DIV2 %08x; DMIC_AUD0 %08x %08x %08x %08x; S_LIF CTRL %08x CONFIG %08x/%08x DONE %08x "
	    "INPUT %08x; DBG_INFO0 %08x -> %08x in %u ms, DBG_INFO1 %08x\n", gpv0 != NULL ? "set" : "MISSING",
	    rd(mic_sys, 0x10), rd(mic_sys, 0x14), rd(mic_dmic, 0), rd(mic_dmic, 4), rd(mic_dmic, 8), rd(mic_dmic, 0x10),
	    rd(mic_slif, 0x100), rd(mic_slif, 0x104), rd(mic_slif, 0x108), rd(mic_slif, 0x10c), rd(mic_slif, 0x114), c0, c1, settle_ms,
	    rd(mic_slif, 0x40));
}


/*
 * mic.cap [ms]: ABOX's capture side as Samsung's run had it during capture (logs/runs/resume-20261107/phone-mic-v5-abox.txt),
 * driven by the AP like the speakers' RDMA3 (Calliope not involved): UAIF6 slave, DSP_A, 8 x 24 bits (CTRL0 0x01880012,
 * CTRL1 0x175d75d7); NSRC4 from UAIF6 (ROUTE_CTRL1 [24:20] = 14); SPUM CTRL1/CTRL4, its banks and the capture ASRC
 * block as read; WDMA4 (0x3400) into our own 64 KB under the speakers' ring. Then each of the 8 channels' peak.
 */
static const struct { uint32_t off, val; } samsung_abox_cap[] = {
	{ 0x0304, 0x00000000 }, { 0x0310, 0x00032170 },
	{ 0x0328, 0x02000000 }, { 0x032c, 0x02000200 }, { 0x0330, 0x02000400 }, { 0x0334, 0x02000600 },
	{ 0x0338, 0x00800800 }, { 0x034c, 0x02c00a00 }, { 0x0350, 0x01a00cc0 }, { 0x0354, 0x01a00e60 },
	{ 0x0358, 0x01901000 },
	{ 0x4000, 0x9bb000bc }, { 0x4010, 0x00010000 }, { 0x4014, 0x00000ccc }, { 0x4018, 0x00010000 },
	{ 0x401c, 0x00000ccc },
};
#define ABOX_ROUTE_CTRL1        0x0044U
#define MIC_UAIF6_CTRL0         0x0560U
#define MIC_UAIF6_CTRL1         0x0564U
#define MIC_WDMA4               0x3400U
#define MIC_BUF_BYTES           0x10000U

static volatile uint8_t *mic_abox;
static volatile uint32_t *mic_buf;
static uint64_t mic_buf_phys;

static bool
mic_capture_buffers(void)
{
	if (mic_abox == NULL) {
		mic_abox = (volatile uint8_t *)ml_io_map(ABOX_PHYS, 0x10000);
	}
	if (mic_buf == NULL && buf_phys != 0) {
		mic_buf_phys = buf_phys - MIC_BUF_BYTES;
		mic_buf = (volatile uint32_t *)ml_io_map_unmappable((vm_offset_t)mic_buf_phys, MIC_BUF_BYTES, VM_WIMG_WCOMB);
	}
	return mic_abox != NULL && mic_buf != NULL;
}

static void
mic_capture_report(uint32_t st0, uint32_t st1, uint32_t ms, char *out, size_t cap)
{
	uint32_t peak[8] = { 0 };
	size_t at;

	for (uint32_t i = 0; i < MIC_BUF_BYTES / 4; i++) {
		int32_t s24 = (int32_t)(mic_buf[i] << 8) >> 8; /* signed 24-bit PCM in the low bits */
		uint32_t a = (uint32_t)(s24 < 0 ? -s24 : s24);

		peak[i % 8] = a > peak[i % 8] ? a : peak[i % 8];
	}
	at = (size_t)snprintf(out, cap, "mic: WDMA4 CTRL %08x STATUS %08x -> %08x in %u ms; UAIF6 %08x %08x STATUS %08x; ROUTE1 %08x; "
	    "S_LIF INT_PEND %08x\nmic: peak per channel (of 2^23):", rd(mic_abox, MIC_WDMA4), st0, st1, ms, rd(mic_abox, MIC_UAIF6_CTRL0),
	    rd(mic_abox, MIC_UAIF6_CTRL1), rd(mic_abox, 0x056c), rd(mic_abox, ABOX_ROUTE_CTRL1), rd(mic_slif, 0x48));
	for (unsigned c = 0; c < 8 && at < cap; c++) {
		at += (size_t)snprintf(out + at, cap - at, " %u", peak[c]);
	}
	if (at < cap) {
		at += (size_t)snprintf(out + at, cap - at, "\nmic: first words: %08x %08x %08x %08x %08x %08x %08x %08x | %08x %08x %08x %08x\n",
		    mic_buf[0], mic_buf[1], mic_buf[2], mic_buf[3], mic_buf[4], mic_buf[5], mic_buf[6], mic_buf[7],
		    mic_buf[256], mic_buf[257], mic_buf[258], mic_buf[259]);
	}
}

static void
dbg_mic_cap(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (!mic_debug_allowed(out, cap)) {
		return;
	}

	uint32_t ms = n >= 1 && v[0] > 0 && v[0] <= 5000 ? (uint32_t)v[0] : 300, st0, st1;

	if (mic_slif == NULL || buf_phys == 0 || cmu == NULL) {
		snprintf(out, cap, "mic: mic.bus, mic.pd, mic.cmu, mic.clk, mic.slif first (and the speakers' driver up)\n");
		return;
	}
	if (!mic_capture_buffers()) {
		snprintf(out, cap, "mic: cannot map the capture buffer\n");
		return;
	}
	/* UAIF6's clocks: CMU_AUD's BCLK_UAIF6 gate and Q-channel, ABOX's own Q-channel for it */
	mic_open_reg(cmu, 0x2038);
	mic_open_reg(cmu, 0x3038);
	wr(mic_abox, ABOX_QCHANNEL_DISABLE, rd(mic_abox, ABOX_QCHANNEL_DISABLE) | (1U << QCH_ABOX_BCLK_UAIF(6)));
	/* stopped while set up */
	wr(mic_abox, MIC_WDMA4, rd(mic_abox, MIC_WDMA4) & ~1U);
	wr(mic_abox, MIC_UAIF6_CTRL0, rd(mic_abox, MIC_UAIF6_CTRL0) & ~2U);
	for (unsigned i = 0; i < sizeof(samsung_abox_cap) / sizeof(samsung_abox_cap[0]); i++) {
		wr(mic_abox, samsung_abox_cap[i].off, samsung_abox_cap[i].val);
	}
	wr(mic_abox, ABOX_ROUTE_CTRL1, (rd(mic_abox, ABOX_ROUTE_CTRL1) & ~(0x1fU << 20)) | (14U << 20));
	wr(mic_abox, MIC_UAIF6_CTRL1, 0x175d75d7U);
	bzero((void *)(uintptr_t)mic_buf, MIC_BUF_BYTES);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(mic_abox, MIC_WDMA4 + 0x08, (uint32_t)mic_buf_phys);                         /* BUF_STR */
	wr(mic_abox, MIC_WDMA4 + 0x0c, (uint32_t)(mic_buf_phys + MIC_BUF_BYTES));       /* BUF_END */
	wr(mic_abox, MIC_WDMA4 + 0x10, MIC_BUF_BYTES / 2);                              /* BUF_OFFSET */
	wr(mic_abox, MIC_WDMA4 + 0x14, (uint32_t)mic_buf_phys);                         /* STR_POINT */
	wr(mic_abox, MIC_WDMA4 + 0x18, 0x00800000);                                     /* VOL_FACTOR */
	wr(mic_abox, MIC_WDMA4 + 0x1c, 0x00010000);                                     /* VOL_CHANGE */
	wr(mic_abox, MIC_WDMA4 + 0x24, 0x00000002);                                     /* BIT_CTRL0 (Samsung's) */
	/* Hardware reference (cache bypass): 24-bit, eight channels, buffer update. */
	wr(mic_abox, MIC_WDMA4, 0x00201720);
	/* go: UAIF6's receiver, then the DMA */
	wr(mic_abox, MIC_UAIF6_CTRL0, 0x01880012U);
	wr(mic_abox, MIC_WDMA4, 0x00201721);
	st0 = rd(mic_abox, MIC_WDMA4 + 0x30);
	IOSleep(ms);
	st1 = rd(mic_abox, MIC_WDMA4 + 0x30);
	mic_capture_report(st0, st1, ms, out, cap);
}

/* Read the running capture without resetting its registers or buffer. */
static void
dbg_mic_peek(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	if (mic_abox == NULL || mic_buf == NULL) {
		snprintf(out, cap, "mic: no capture buffer yet (mic.cap first)\n");
		return;
	}
	uint32_t st = rd(mic_abox, MIC_WDMA4 + 0x30);
	mic_capture_report(st, st, 0, out, cap);
}

/* mic.stop: WDMA4 and UAIF6's receiver off */
static void
dbg_mic_stop(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	if (!mic_debug_allowed(out, cap)) {
		return;
	}

	if (mic_abox != NULL) {
		wr(mic_abox, MIC_WDMA4, rd(mic_abox, MIC_WDMA4) & ~1U);
		wr(mic_abox, MIC_UAIF6_CTRL0, rd(mic_abox, MIC_UAIF6_CTRL0) & ~2U);
	}
	snprintf(out, cap, "mic: capture stopped\n");
}


/*
 * The watchdog (B, 2026-11-07): after a cold start Samsung's bootloader seems to leave WDT_CLUSTER0 (0x10050000,
 * timeout-sec 30) running and nothing here feeds it: iOS restarted at the lock screen, once or in a loop. Reading it
 * with its clock gated froze the phone, so: wdt.look reads CMU_PERIS's gate (0x10020000 +0x2048, Q-channel +0x3030)
 * and only with that on the watchdog's WTCON/WTDAT/WTCNT; wdt.off opens the gate and clears WTCON (wdt=0 at boot: the
 * same as the sound driver comes up).
 */
#define CMU_PERIS_PHYS          0x10020000U
#define WDT_CL0_PHYS            0x10050000U

static volatile uint8_t *wdt_peris, *wdt_base;

static bool
wdt_gate_on(void)
{
	uint32_t g, q;

	if (wdt_peris == NULL) {
		wdt_peris = (volatile uint8_t *)ml_io_map(CMU_PERIS_PHYS, 0x4000);
	}
	g = rd(wdt_peris, 0x2048);
	q = rd(wdt_peris, 0x3030);
	return (g & GATE_CG_VAL) != 0 && ((q & QCH_ENABLE) == 0 || (q & QCH_CLOCK_REQ) != 0);
}

static size_t
wdt_state(char *out, size_t cap)
{
	size_t at = (size_t)snprintf(out, cap, "wdt: PERIS gate %08x QCH %08x", rd(wdt_peris, 0x2048), rd(wdt_peris, 0x3030));

	if (wdt_gate_on()) {
		if (wdt_base == NULL) {
			wdt_base = (volatile uint8_t *)ml_io_map(WDT_CL0_PHYS, 0x1000);
		}
		at += (size_t)snprintf(out + at, cap - at, "; WTCON %08x WTDAT %08x WTCNT %08x\n", rd(wdt_base, 0), rd(wdt_base, 4),
		    rd(wdt_base, 8));
	} else {
		at += (size_t)snprintf(out + at, cap - at, "; its clock is off: not read\n");
	}
	return at;
}

static void
dbg_wdt_look(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	if (wdt_peris == NULL) {
		wdt_peris = (volatile uint8_t *)ml_io_map(CMU_PERIS_PHYS, 0x4000);
	}
	(void)wdt_state(out, cap);
}

static bool
wdt_disable(void)
{
	if (wdt_peris == NULL) {
		wdt_peris = (volatile uint8_t *)ml_io_map(CMU_PERIS_PHYS, 0x4000);
	}
	mic_open_reg(wdt_peris, 0x2048);
	mic_open_reg(wdt_peris, 0x3030);
	if (!wdt_gate_on()) {
		return false;
	}
	if (wdt_base == NULL) {
		wdt_base = (volatile uint8_t *)ml_io_map(WDT_CL0_PHYS, 0x1000);
	}
	wr(wdt_base, 0, rd(wdt_base, 0) & ~((1U << 5) | (1U << 0)));   /* WTCON: timer off, reset off */
	return true;
}

static void
dbg_wdt_off(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	size_t at = (size_t)snprintf(out, cap, "wdt: %s\n", wdt_disable() ? "disabled" : "its clock would not come on");

	(void)wdt_state(out + at, cap - at);
}


/* pmic.r SUB TYPE REG / pmic.w SUB TYPE REG VAL: the main (SUB 0, S2MPS19) or sub (SUB 1, S2MPS22) PMIC over ACPM */
static void
dbg_pmic_r(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	uint8_t c = 0;
	int r;

	if (n < 3) {
		snprintf(out, cap, "pmic.r SUB(0 main, 1 sub) TYPE REG\n");
		return;
	}
	r = v[0] ? redwood_spmic_read((uint8_t)v[1], (uint8_t)v[2], &c) : redwood_pmic_read((uint8_t)v[1], (uint8_t)v[2], &c);
	snprintf(out, cap, "pmic %s type %llu reg %#04llx = %#04x (answer %d)\n", v[0] ? "sub" : "main", v[1], v[2], c, r);
}

static void
dbg_pmic_w(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	extern int redwood_spmic_write(uint8_t type, uint8_t reg, uint8_t value);
	uint8_t c = 0;
	int r, r2;

	if (n < 4) {
		snprintf(out, cap, "pmic.w SUB TYPE REG VAL\n");
		return;
	}
	r = v[0] ? redwood_spmic_write((uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3]) :
	    redwood_pmic_write((uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3]);
	r2 = v[0] ? redwood_spmic_read((uint8_t)v[1], (uint8_t)v[2], &c) : redwood_pmic_read((uint8_t)v[1], (uint8_t)v[2], &c);
	snprintf(out, cap, "pmic %s type %llu reg %#04llx <- %#04llx (answer %d), reads %#04x (%d)\n", v[0] ? "sub" : "main",
	    v[1], v[2], v[3], r, c, r2);
}


/*
 * mic.vts [0]: VTS's Cortex-M core booted as Samsung's vts_start_runtime_resume does on the Exynos 990 before any capture
 * (the stock kernel's log, phone-mic-v6.txt): vts.bin (root image fw/, from the phone's /vendor/firmware) copied into
 * VTS's SRAM (0x15600000), CM4 GPR dump on (gpr +0 = 1), the core powered (PMU VTS_CPU_CONFIGURATION 0x3080 bit 0).
 * Samsung then waits for "VTS boot completed" and sets mode 0 (off): the core idles during capture. mic.vts 0: powered off.
 */
#define VTS_SRAM_PHYS           0x15600000U
#define VTS_SRAM_SIZE           0x101400U
#define VTS_GPR_PHYS            0x155f0000U
#define PMU_VTS_CPU_CONF        0x3080U
#define PMU_VTS_CPU_STATUS      0x3084U
#define PMU_VTS_CPU_IN          0x30a4U

static void
dbg_mic_vts(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (!mic_debug_allowed(out, cap)) {
		return;
	}

	static volatile uint8_t *sram, *gpr;
	const uint8_t *fw;
	uint64_t size = 0;
	uint32_t st0, st1;

	if (!mic_pd_open || mic_pmu == NULL) {
		snprintf(out, cap, "mic: mic.bus, mic.pd first\n");
		return;
	}
	if (n >= 1 && v[0] == 0) {
		wr(mic_pmu, PMU_VTS_CPU_CONF, rd(mic_pmu, PMU_VTS_CPU_CONF) & ~1U);
		snprintf(out, cap, "mic: VTS core off (CONF %08x STATUS %08x)\n", rd(mic_pmu, PMU_VTS_CPU_CONF),
		    rd(mic_pmu, PMU_VTS_CPU_STATUS));
		return;
	}
	fw = redwood_image_file("fw", "vts.bin", &size, NULL);
	if (fw == NULL || size == 0 || size > VTS_SRAM_SIZE) {
		snprintf(out, cap, "mic: no fw/vts.bin in the root image (userland/build.sh, ../fw-sound/vts.bin)\n");
		return;
	}
	if (sram == NULL) {
		sram = (volatile uint8_t *)ml_io_map(VTS_SRAM_PHYS, VTS_SRAM_SIZE);
		gpr = (volatile uint8_t *)ml_io_map(VTS_GPR_PHYS, 0x1000);
	}
	wr(mic_pmu, PMU_VTS_CPU_CONF, rd(mic_pmu, PMU_VTS_CPU_CONF) & ~1U);     /* (off while its program is replaced) */
	IOSleep(2);
	for (uint64_t i = 0; i + 4 <= size; i += 4) {
		uint32_t w;

		memcpy(&w, fw + i, 4);
		*(volatile uint32_t *)(sram + i) = w;
	}
	if (size & 3) {
		uint32_t w = 0;

		memcpy(&w, fw + (size & ~3ULL), size & 3);
		*(volatile uint32_t *)(sram + (size & ~3ULL)) = w;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(gpr, 0, 1);                                  /* CM4 GPR dump on */
	st0 = rd(mic_pmu, PMU_VTS_CPU_STATUS);
	wr(mic_pmu, PMU_VTS_CPU_CONF, rd(mic_pmu, PMU_VTS_CPU_CONF) | 1U);
	IOSleep(50);
	st1 = rd(mic_pmu, PMU_VTS_CPU_STATUS);
	snprintf(out, cap, "mic: vts.bin (%llu bytes) in VTS SRAM (first words %08x %08x, read back %08x %08x); core CONF %08x STATUS "
	    "%08x -> %08x IN %08x; gpr %08x %08x %08x %08x\n", size, ((const uint32_t *)fw)[0], ((const uint32_t *)fw)[1],
	    *(volatile uint32_t *)sram, *(volatile uint32_t *)(sram + 4), rd(mic_pmu, PMU_VTS_CPU_CONF), st0, st1,
	    rd(mic_pmu, PMU_VTS_CPU_IN), rd(gpr, 0), rd(gpr, 4), rd(gpr, 8), rd(gpr, 0xc));
}

static void
dbg_snd_gate(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (n >= 1) {
		snd_gate = (uint32_t)(v[0] > 32767 ? 32767 : v[0]);
	}
	snprintf(out, cap, "gate %u of 32767 (a block from iOS peaking under it is silence); since the last look: %u blocks, "
	    "%u gated, the loudest peaked at %u; amps %s\n", snd_gate, gate_blocks, gate_closed, gate_peak_max,
	    amps_on ? "on" : "off");
	gate_peak_max = gate_blocks = gate_closed = 0;
}

static void
dbg_snd_amps(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (n >= 1 && v[0] != 0) {
		amps_on = amps_up();
		snprintf(out, cap, "amplifiers %s\n", amps_on ? "on" : "FAILED (see the log)");
	} else {
		amps_down();
		amps_on = false;
		snprintf(out, cap, "amplifiers off\n");
	}
}


/* snd.supply [1]: the amplifiers' supply, read (and switched on with 1); snd.prep: their reset and setup again */
static void
dbg_snd_supply(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	uint8_t c = 0;

	if (n >= 1 && v[0] != 0) {
		amps_supply(out, cap);
		return;
	}
	int r = redwood_spmic_read(1, SPMIC_LDO4S_CTRL, &c);

	snprintf(out, cap, "LDO4S CTRL %#04x (answer %d)\n", c, r);
}

static void
dbg_snd_prep(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	/* The playback worker owns reset, I2C and the failure latch. */
	(void)OSCompareAndSwap(0, 1, &amp_recovery_requested);
	thread_wakeup((event_t)&ring_head);
	snprintf(out, cap, "amplifier recovery queued on the audio thread (result in the log)\n");
}

/* iOS input: 48 kHz, signed 16-bit mono; hardware runs only while the device is open. */
#define MIC_IOS_RING_FRAMES 16384U
#define MIC_IOS_PRIME_FRAMES 1024U
#define MIC_DMA_FRAME_BYTES 32U
static LCK_GRP_DECLARE(exynos_mic_group, "exynos-mic");
static LCK_MTX_DECLARE(exynos_mic_lock, &exynos_mic_group);
static int16_t mic_ios_ring[MIC_IOS_RING_FRAMES];
static uint32_t mic_ios_head, mic_ios_tail;
static bool mic_ios_primed;
static uint64_t mic_ios_produced, mic_ios_dropped;
static uint32_t mic_ios_peak, mic_ios_short_reads;
static uint32_t mic_saved_cap[sizeof(samsung_abox_cap) / sizeof(samsung_abox_cap[0])];
static uint32_t mic_saved_route, mic_saved_qch, mic_saved_uaif0, mic_saved_uaif1;
static uint32_t mic_saved_mux, mic_saved_div;
static bool mic_saved_valid;

static int
exynos_mic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	int error = 0;

	lck_mtx_lock(&exynos_mic_lock);
	if (mic_ios_wants) {
		error = EBUSY;
	} else {
		mic_ios_head = mic_ios_tail = 0;
		mic_ios_produced = mic_ios_dropped = 0;
		mic_ios_peak = mic_ios_short_reads = 0;
		mic_ios_primed = false;
		mic_ios_wants = true;
	}
	lck_mtx_unlock(&exynos_mic_lock);
	if (error == 0) {
		thread_wakeup((event_t)&mic_ios_wants);
	}
	return error;
}

static int
exynos_mic_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&exynos_mic_lock);
	mic_ios_wants = false;
	lck_mtx_unlock(&exynos_mic_lock);
	thread_wakeup((event_t)&mic_ios_wants);
	return 0;
}

static int
exynos_mic_read(__unused dev_t dev, struct uio *uio, __unused int flags)
{
	int16_t samples[512];
	int error = 0;

	while (uio_resid(uio) >= 2 && error == 0) {
		uint32_t want = (uint32_t)(uio_resid(uio) / 2), have, take;

		want = want > 512 ? 512 : want;
		lck_mtx_lock(&exynos_mic_lock);
		have = mic_ios_head - mic_ios_tail;
		if (!mic_ios_primed && have >= MIC_IOS_PRIME_FRAMES) {
			mic_ios_primed = true;
		}
		if (!mic_ios_primed) {
			lck_mtx_unlock(&exynos_mic_lock);
			break;
		}
		take = want < have ? want : have;
		for (uint32_t i = 0; i < take; i++) {
			samples[i] = mic_ios_ring[(mic_ios_tail + i) % MIC_IOS_RING_FRAMES];
		}
		mic_ios_tail += take;
		if (take < want) {
			mic_ios_short_reads++;
			mic_ios_primed = false;
		}
		lck_mtx_unlock(&exynos_mic_lock);
		if (take == 0) {
			break;
		}
		error = uiomove((caddr_t)samples, (int)(take * 2), uio);
	}
	return error;
}

static const struct cdevsw exynos_mic_cdevsw = {
	.d_open = exynos_mic_open, .d_close = exynos_mic_close, .d_read = exynos_mic_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* ABOX 3.0.1: an eight-bit period index and a twenty-bit byte count, not the older sixteen-byte offset. */
static uint32_t
exynos_mic_position(void)
{
	uint32_t status = rd(mic_abox, MIC_WDMA4 + 0x30);
	uint32_t bytes = ((status >> 20) & 0xffU) * (MIC_BUF_BYTES / 2) + (status & 0xfffffU);

	return (bytes % MIC_BUF_BYTES) / MIC_DMA_FRAME_BYTES;
}

static void
exynos_mic_stop_path(void)
{
	char report[80];

	dbg_mic_stop(NULL, 0, report, sizeof(report));
	if (mic_slif != NULL) {
		wr(mic_slif, 0x300, 0);
		wr(mic_slif, 0x100, 0);
		wr(mic_slif, 0x114, 0);
	}
	for (unsigned i = 0; i < 3; i++) {
		volatile uint8_t *d = i == 0 ? mic_dmic : mic_dmic_extra[i - 1];

		if (d != NULL) {
			wr(d, 0, rd(d, 0) & ~(1U << 31));
		}
	}
	if (mic_abox != NULL && mic_saved_valid) {
		for (unsigned i = 0; i < sizeof(mic_saved_cap) / sizeof(mic_saved_cap[0]); i++) {
			wr(mic_abox, samsung_abox_cap[i].off, mic_saved_cap[i]);
		}
		wr(mic_abox, ABOX_ROUTE_CTRL1, mic_saved_route);
		wr(mic_abox, ABOX_QCHANNEL_DISABLE, mic_saved_qch);
		wr(mic_abox, MIC_UAIF6_CTRL1, mic_saved_uaif1);
		wr(mic_abox, MIC_UAIF6_CTRL0, mic_saved_uaif0 & ~2U);
	}
	if (mic_saved_valid) {
		wr(cmu, 0x1024, mic_saved_mux);
		wr(cmu, 0x1844, mic_saved_div);
	}
	mic_saved_valid = false;
	/* Touch only the mic-bias pin: the adjacent pins are the live amplifier I2C bus. */
	volatile uint8_t *bias = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, PINCTRL_PERIC0_GPP4);
	if (bias != NULL) {
		redwood_gpio_set(bias, 1, 0);
	}
	mic_front_ready = false;
}

static bool
exynos_mic_start_path(void)
{
	char report[1024];
	uint64_t ms = 1;

	mic_saved_valid = false;
	if (!mic_capture_buffers()) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(mic_saved_cap) / sizeof(mic_saved_cap[0]); i++) {
		mic_saved_cap[i] = rd(mic_abox, samsung_abox_cap[i].off);
	}
	mic_saved_route = rd(mic_abox, ABOX_ROUTE_CTRL1);
	mic_saved_qch = rd(mic_abox, ABOX_QCHANNEL_DISABLE);
	mic_saved_uaif0 = rd(mic_abox, MIC_UAIF6_CTRL0);
	mic_saved_uaif1 = rd(mic_abox, MIC_UAIF6_CTRL1);
	mic_saved_mux = rd(cmu, 0x1024);
	mic_saved_div = rd(cmu, 0x1844);
	mic_saved_valid = true;
	mic_front_ready = false;
	dbg_mic_bus(NULL, 0, report, sizeof(report));
	dbg_mic_pd(NULL, 0, report, sizeof(report));
	if (!mic_pd_open) {
		return false;
	}
	dbg_mic_cmu(NULL, 0, report, sizeof(report));
	dbg_mic_clk(NULL, 0, report, sizeof(report));
	dbg_mic_slif(NULL, 0, report, sizeof(report));
	if (!mic_front_ready) {
		return false;
	}
	wr(mic_slif, 0x300, 0); /* no diagnostic AHB dump while iOS uses WDMA4 */
	dbg_mic_cap(&ms, 1, report, sizeof(report));
	IOSleep(20); /* discard power-up/filter settling before exposing samples */
	return (rd(mic_abox, MIC_WDMA4 + 0x30) & (1U << 31)) != 0;
}

static void
exynos_mic_thread(__unused void *parameter, __unused wait_result_t result)
{
	bool active = false;
	unsigned failures = 0, still = 0;
	uint32_t last = 0;
	int major;

	mic_worker = current_thread();
	major = cdevsw_add(-1, &exynos_mic_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-mic") == NULL) {
		snd_log("mic: cannot create /dev/redwood-mic\n");
		return;
	}
	snd_log("mic: iOS input available (/dev/redwood-mic, 48 kHz signed 16-bit mono)\n");
	for (;;) {
		if (!mic_ios_wants) {
			if (active) {
				exynos_mic_stop_path();
				active = false;
				mic_hw_busy = false;
				snd_dbg("mic: iOS capture stopped (%llu frames, %llu old frames dropped, peak %u/32768, %u short reads)\n",
				    mic_ios_produced, mic_ios_dropped, mic_ios_peak, mic_ios_short_reads);
			}
			failures = 0;
			assert_wait((event_t)&mic_ios_wants, THREAD_UNINT);
			if (mic_ios_wants) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
			continue;
		}
		if (!active) {
			if (!sound_ready || failures >= 3) {
				IOSleep(10);
				continue;
			}
			mic_hw_busy = true;
			if (!exynos_mic_start_path()) {
				exynos_mic_stop_path();
				mic_hw_busy = false;
				failures++;
				snd_log("mic: capture start failed (%u/3)\n", failures);
				IOSleep(100);
				continue;
			}
			last = exynos_mic_position();
			active = true;
			still = 0;
			snd_dbg("mic: iOS capture running (all three DMIC converters, UAIF6, WDMA4)\n");
		}
		uint32_t pos = exynos_mic_position();
		uint32_t frames = (pos + MIC_BUF_BYTES / MIC_DMA_FRAME_BYTES - last) % (MIC_BUF_BYTES / MIC_DMA_FRAME_BYTES);

		/* self-check: WDMA4 stuck for 200 ms -> the path down and up again (3 tries until iOS closes the device) */
		if (frames == 0 && ++still >= 50) {
			exynos_mic_stop_path();
			active = false;
			mic_hw_busy = false;
			failures++;
			heal_mic_stall++;
			snd_log("mic: capture stalled (WDMA4 at %u for 200 ms): restarting the path (%u/3)\n", pos, failures);
			continue;
		} else if (frames != 0) {
			still = 0;
		}

		__asm__ volatile ("dsb sy" ::: "memory");
		lck_mtx_lock(&exynos_mic_lock);
		for (uint32_t i = 0; i < frames; i++) {
			uint32_t at = (last + i) % (MIC_BUF_BYTES / MIC_DMA_FRAME_BYTES);
			int16_t sample = (int16_t)((int32_t)(mic_buf[at * 8] << 8) >> 16);

			mic_ios_ring[mic_ios_head++ % MIC_IOS_RING_FRAMES] = sample;
			if (snd_debug) {
				uint32_t magnitude = sample < 0 ? (uint32_t)-(int32_t)sample : (uint32_t)sample;

				mic_ios_peak = magnitude > mic_ios_peak ? magnitude : mic_ios_peak;
			}
			mic_ios_produced++;
		}
		if (mic_ios_head - mic_ios_tail > MIC_IOS_RING_FRAMES) {
			mic_ios_dropped += mic_ios_head - mic_ios_tail - MIC_IOS_RING_FRAMES;
			mic_ios_tail = mic_ios_head - MIC_IOS_RING_FRAMES;
		}
		lck_mtx_unlock(&exynos_mic_lock);
		last = pos;
		IOSleep(4);
	}
}

/*
 * One-shot arming for the DSP route (B, 2026-11-06, after two boot loops): snd_calliope runs only if
 * /private/var/root/calliope.arm starts with '1', and this overwrites it with '0' (synced) before Calliope starts --
 * so a run that hangs or loops comes back without the DSP on the next boot by itself. Arm from the Mac:
 *   ssh s20 'echo 1 > /private/var/root/calliope.arm'   (then deploy or restart with snd_calliope=2)
 * Waits up to 150 s for /private/var (mounted by init). snd_cal_arm=0 skips the check.
 */
#define CAL_ARM_PATH            "/private/var/root/calliope.arm"
static bool
calliope_armed(void)
{
	vfs_context_t context = vfs_context_kernel();
	vnode_t vp = NULLVP;
	char c = 0, zero = '0';
	int left = 0, error = ENOENT;
	uint32_t check = 1;

	PE_parse_boot_argn("snd_cal_arm", &check, sizeof(check));
	if (check == 0) {
		return true;
	}
	for (unsigned waited = 0; waited < 300; waited++) {
		if (rootvnode != NULLVP && vnode_lookup("/private/var/root", 0, &vp, context) == 0) {
			vnode_put(vp);
			vp = NULLVP;
			error = vnode_open(CAL_ARM_PATH, FREAD | FWRITE, 0, 0, &vp, context);
			if (error != ENOENT || waited >= 120) {
				break;          /* /var is there: the file is or is not (2 s more for a late mount) */
			}
		}
		delay_for_interval(500, NSEC_PER_MSEC);
	}
	if (error != 0 || vp == NULLVP) {
		snd_log("snd: calliope: not armed (%s: %d): the AP-only path this boot\n", CAL_ARM_PATH, error);
		return false;
	}
	error = vn_rdwr(UIO_READ, vp, &c, 1, 0, UIO_SYSSPACE, 0, vfs_context_ucred(context), &left, vfs_context_proc(context));
	if (error == 0 && left == 0 && c == '1') {
		error = vn_rdwr(UIO_WRITE, vp, &zero, 1, 0, UIO_SYSSPACE, IO_SYNC, vfs_context_ucred(context), &left,
		    vfs_context_proc(context));
		(void)VNOP_FSYNC(vp, MNT_WAIT, context);
	} else {
		c = 0;
	}
	vnode_close(vp, FREAD | FWRITE, context);
	if (c != '1' || error != 0) {
		snd_log("snd: calliope: not armed (%s holds '%c', disarm %d): the AP-only path this boot\n", CAL_ARM_PATH,
		    c ? c : '?', error);
		return false;
	}
	snd_log("snd: calliope: armed by %s (now disarmed for the next boot)\n", CAL_ARM_PATH);
	return true;
}

static void
sound_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t written, consumed, base, t0, last_look = 0, last_vol = 0, checked_at = 0;
	uint64_t status_consumed = 0, quiet_since = 0;
	uint32_t last_pos, generation = 0, below = 0;
	unsigned start_recoveries = 0;
	bool muted = false, decided = false, stall_told = false;
	uint64_t stall_since = 0;

	if (!aud_power_on() || !aud_clocks()) {
		return;
	}
	if (snd_calliope != 0 && !calliope_armed()) {
		snd_calliope = 0;
		snd_rdma = 3;
	}
	if (snd_calliope != 0) {
		/*
		 * The firmware is in the system image's /fw, which is the root only once init has switched to iOS's volume (the
		 * small image is the root before): wait until the file is there (120 s at most), unless the small image has it.
		 */
		{
			uint64_t sz = 0;
			unsigned waited = 0;
			bool there = redwood_image_file("fw", "calliope_dram_2.bin", &sz, NULL) != NULL;

			while (!there && waited++ < 240) {
				vnode_t vn = NULLVP;

				if (rootvnode != NULLVP && vnode_lookup("/fw/calliope_dram_2.bin", 0, &vn, vfs_context_kernel()) == 0) {
					vnode_put(vn);
					there = true;
					break;
				}
				delay_for_interval(500, NSEC_PER_MSEC);
			}
			snd_log("snd: calliope: the firmware is %s (after %u ms)\n", there ? "there" : "NOT there after 120 s",
			    waited * 500);
		}
		calliope_boot();                /* stage 1 of Samsung's way: the DSP firmware; no bypass playback this run */
		if (snd_calliope < 2) {
			return;
		}
	}
	abox_route();
	amps_ok = amps_bring_up();      /* (their setup needs no clock; their power-up does) */
	/*
	 * At boot about half the bring-ups fail ("does not answer" / "OTP_BOOT_DONE never came") and the same code a
	 * minute later always works (B, 2026-11-06, snd.prep x3 on a running phone): try again, a little later each time.
	 */
	for (unsigned tries = 1; (!amps[0].present || !amps[1].present) && tries <= 6; tries++) {
		IOSleep(250 * tries);
		amps_ok = amps_bring_up();
		snd_log("snd: amplifiers, try %u: left %s, right %s\n", tries + 1, amps[0].present ? "ok" : "missing",
		    amps[1].present ? "ok" : "missing");
	}
	if (snd_calliope >= 2) {
		if (!calliope_play()) {
			return;
		}
	} else {
		abox_start();
	}
	/* frames are counted on from where the DMA is now, in the ring's terms: ring index = count % BUF_FRAMES */
	t0 = now_us();
	last_pos = dma_position(NULL);
	base = written = last_pos;
	redwood_guard_configure(true, snd_gain);
	redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
	redwood_audio_set_sink("the S20+'s speakers (ABOX UAIF2, CS35L41 x2)", exynos_sink);
	sound_ready = true;
	if (snd_tone != 0) {
		tone_left = snd_tone * 48;
		last_sound_us = now_us();
	}
	snd_log("snd: running (amplifiers %s, %u/8 dB under the chip's full level, guard gain %u/8 dB%s)\n",
	    amps_ok ? "ready" : "MISSING", snd_below, snd_gain, snd_tone ? ", the test tone first" : "");

	for (;;) {
		uint64_t now = now_us(), by_time = (now - t0) * REDWOOD_AUDIO_RATE / 1000000;
		uint32_t pos = dma_position(NULL), delta = (pos + BUF_FRAMES - last_pos) % BUF_FRAMES;

		if (OSCompareAndSwap(1, 0, &amp_recovery_requested)) {
			uint32_t status = 0;
			bool healthy;

			redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
			amps_down();
			amps_on = false;
			amps_ok = amps_bring_up();
			healthy = amps_ok && amps[0].present && amps[1].present;
			for (unsigned i = 0; i < 2; i++) {
				if (!amp_read(&amps[i], L41_IRQ1_STATUS1, &status) || (status & L41_FAULTS)) {
					healthy = false;
				}
			}
			faulted = !healthy;
			quiet_since = 0;
			snd_log("snd: amplifier recovery %s; left %s, right %s\n", healthy ? "ready" : "FAILED (muted)",
			    amps[0].present ? "ok" : "missing", amps[1].present ? "ok" : "missing");
		}
		last_pos = pos;
		status_consumed += delta;
		if (amps_on && use_status && delta == 0) {
			if (stall_since == 0) {
				stall_since = now;
			} else if (now - stall_since >= 200000 && !stall_told) {
				stall_told = true;
				dma_stalls++;
				snd_log("snd: playback DMA stalled at %u for 200 ms\n", pos);
			}
		} else {
			stall_since = 0;
			stall_told = false;
		}
		if (cal_dram != NULL) {
			static uint64_t cal_log_at;

			calliope_drain(cal_dram);
			calliope_trigger_word();
			calliope_timer_kick();
			if (now - cal_log_at >= 500000) {
				cal_log_at = now;
				calliope_log();
			}
		}
		/* after a quarter second: does the DMA's own count agree with the clock? (it should, within a period) */
		if (!decided && now - t0 >= 250000) {
			int64_t diff = (int64_t)status_consumed - (int64_t)by_time;

			decided = true;
			use_status = diff < 1024 && diff > -1024 && status_consumed != 0;
			snd_log("snd: after %llu us the DMA read %llu frames, the clock says %llu: following %s\n", now - t0,
			    status_consumed, by_time, use_status ? "the DMA" : "the clock");
		}
		/* the amplifiers: on with sound, off after three quiet seconds; iOS's volume; their faults */
		/*
		 * (at once, not at the next 100 ms look, when sound comes with the amplifiers off -- and before the samples are
		 * handed to the DMA: a key click is over in 30 ms; B, 2026-11-07, "single clicks don't play")
		 */
		if (amps_ok && !faulted && (now - last_vol >= 100000 ||
		    (!amps_on && last_sound_us != 0 && now - last_sound_us < QUIET_OFF_US))) {
			bool sound = last_sound_us != 0 && now - last_sound_us < QUIET_OFF_US;
			uint32_t b = 0, g = 0;
			bool m = false;

			last_vol = now;
			if (redwood_audio_volume(&b, &m, &g) && g != generation) {
				generation = g;
				below = b;
				muted = m;
			}
			if (sound && !muted && !amps_on) {
				amps_on = amps_up();
				if (!amps_on) {
					faulted = true;
					amps_down();
					redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
					/* Retry only an answered, fault-free PUP timeout, at most twice per boot. */
					if (amp_start_retryable && start_recoveries < 2) {
						start_recoveries++;
						(void)OSCompareAndSwap(0, 1, &amp_recovery_requested);
						snd_log("snd: amplifier startup recovery %u/2 queued\n", start_recoveries);
					}
				}
				quiet_since = 0;
				if (!amps_on || snd_debug) {
					snd_log("snd: amplifiers %s (iOS volume %u/8 dB under)\n", amps_on ? "on" : "FAILED", below);
				}
			} else if (amps_on && (!sound || muted)) {
				if (quiet_since == 0) {
					quiet_since = now;
				}
				if (muted || now - quiet_since >= 100000) {
					amps_down();
					amps_on = false;
					redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
					snd_dbg("snd: amplifiers off (%llu frames from iOS, %llu fed, %u underruns)\n", ios_frames, fed_frames,
					    (uint32_t)underruns);
				}
			} else if (sound) {
				quiet_since = 0;
			}
			/* (quiet_since is not touched here: clearing it on every pass kept the amplifiers on for good, 27 October) */
			if (amps_on) {
				uint32_t total = snd_below + (tone_left != 0 ? 0 : below);

				amps_volume(total, false);
				redwood_guard_attenuation(total);
				if (now - checked_at >= 500000) {
					uint32_t f = amps_faults();

					checked_at = now;
					if (f != 0) {
						amps_down();
						amps_on = false;
						faulted = true;
						redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
						/* self-check: reset and set up again, at most 3 times a boot (a real short stays off) */
						if (heal_amp_fault < 3) {
							heal_amp_fault++;
							(void)OSCompareAndSwap(0, 1, &amp_recovery_requested);
							snd_log("snd: AMPLIFIER FAULT %#x: recovery %u/3 queued\n", f, heal_amp_fault);
						} else {
							snd_log("snd: AMPLIFIER FAULT %#x: both off for the rest of the run\n", f);
						}
					} else {
						amps_answered_us = now;
					}
				}
			}
		}
		/* iOS's samples to the DMA (after the amplifiers: see above) */
		consumed = base + (use_status ? status_consumed : by_time);
		if (written < consumed) {
			underruns++;
			written = consumed + 64;
		}
		while (written < consumed + LEAD_FRAMES) {
			uint32_t n = (uint32_t)(consumed + LEAD_FRAMES - written), room = BUF_FRAMES - (uint32_t)(written % BUF_FRAMES);

			n = n < CHUNK ? n : CHUNK;
			n = n < room ? n : room;
			fill(written, n);
			written += n;
		}
		__asm__ volatile ("dsb sy" ::: "memory");

		if (now - last_look >= 60000000) {
			last_look = now;
			snd_dbg("snd: %llu frames from iOS, %llu fed, %llu silent, %u underruns; DMA at %u (%s); quiet %llu times, "
			    "%llu ms in all\n", ios_frames, fed_frames, silent_frames, (uint32_t)underruns, pos,
			    use_status ? "followed" : "by the clock", quiet_spells, quiet_us / 1000);
		}
		if (decided && !amps_on && tone_left == 0 &&
		    (last_sound_us == 0 || now - last_sound_us >= QUIET_OFF_US)) {
			uint64_t began = now_us(), heard = last_sound_us;

			/* the whole ring silence, then asleep until iOS sends sound (it goes on sending silence: dropped) */
			bzero(buf, BUF_BYTES);
			__asm__ volatile ("dsb sy" ::: "memory");
			snd_quiet = true;
			quiet_spells++;
			while (amp_recovery_requested == 0 && last_sound_us == heard && now_us() - began < 60000000) {
				ring_tail = ring_head;
				assert_wait_timeout((event_t)&ring_head, THREAD_UNINT, 1000, NSEC_PER_MSEC);
				if (amp_recovery_requested != 0 || last_sound_us != heard) {
					clear_wait(current_thread(), THREAD_AWAKENED);
					break;
				}
				thread_block(THREAD_CONTINUE_NULL);
			}
			snd_quiet = false;
			now = now_us();
			quiet_us += now - began;
			if (amp_recovery_requested == 0 && last_sound_us == heard && amps_ok && !faulted) {
				if (amps_answer()) {
					amps_answered_us = now;
				} else if (heal_amp_idle < 3) {
					heal_amp_idle++;
					(void)OSCompareAndSwap(0, 1, &amp_recovery_requested);
					snd_log("snd: self-check: an amplifier stopped answering: recovery %u/3 queued\n", heal_amp_idle);
				}
			}
			/* on from where the DMA is now: the count again such that count % BUF_FRAMES is its ring index */
			pos = dma_position(NULL);
			base = written + (pos + BUF_FRAMES - (uint32_t)(written % BUF_FRAMES)) % BUF_FRAMES;
			written = base;
			last_pos = pos;
			status_consumed = 0;
			t0 = now;
			continue;
		}
		IOSleep(4);
	}
}


/*
 * Calliope's cores off (PMU: enable 0x31a0/0x3220 bit 0, then power 0x3180/0x3200 bit 0) and ABOX's SysMMU off. A warm
 * restart (a panic, the menu's Reboot) does not power the audio block down: a DSP left running writes into memory the
 * next kernel owns (A, 2026-11-06: boot loop after a run with Calliope up).
 */
static void
calliope_halt(volatile uint8_t *p, volatile uint8_t *m)
{
	if (p == NULL) {
		return;
	}
	for (unsigned c = 0; c < 2; c++) {
		uint32_t en = 0x31a0 + 0x80 * c, pw = 0x3180 + 0x80 * c;

		*(volatile uint32_t *)(p + en) &= ~1U;
		*(volatile uint32_t *)(p + pw) &= ~1U;
	}
	if (m != NULL) {
		*(volatile uint32_t *)(m + 0x8000) = 0;     /* CTRL_VM */
		*(volatile uint32_t *)(m + 0x0) = 0;        /* CTRL */
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* snd.health: what the drivers know, from memory only (no I2C, no registers: safe while playing) */
static void
dbg_snd_health(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	uint64_t now = now_us();

	snprintf(out, cap, "snd: %s; amps %s/%s%s%s, %s, last answered %llu s ago; %llu underruns, %u DMA stalls; "
	    "repairs: %u amp faults, %u amp idle, %u mic stalls; mic %s%s; debug %s\n",
	    sound_ready ? "running" : "NOT running", amps[0].present ? "L ok" : "L MISSING", amps[1].present ? "R ok" : "R MISSING",
	    faulted ? ", FAULTED (muted)" : "", amps_ok ? "" : ", bring-up failed", amps_on ? "on" : (snd_quiet ? "asleep" : "off"),
	    amps_answered_us ? (now - amps_answered_us) / 1000000 : 0, underruns, dma_stalls,
	    heal_amp_fault, heal_amp_idle, heal_mic_stall, mic_worker != NULL ? "ready" : "NOT ready",
	    mic_hw_busy ? " (capturing)" : "", snd_debug ? "on" : "off");
}

/*
 * Why the last run ended, at every boot (B, 2026-11-07: the lock-screen restarts were the main PMIC's WTSR pulling
 * the SoC's reset pin -- RST_STAT PINRESET, PWRONSRC WTSR, OFFSRC 0 -- with nothing in the kernel log). Names from
 * Samsung's sec_debug_reset_reason.c (Exynos 9830 RST_STAT, S2MPS19 PWRONSRC 0x10 / OFFSRC 0x11).
 */
static void
reset_reason_log(void)
{
	static const char *const rst[32] = {
		"CL0_DBGRST0", "CL0_DBGRST1", "CL0_DBGRST2", "CL0_DBGRST3", "CL1_DBGRST0", "CL1_DBGRST1", "CL2_DBGRST0",
		"CL2_DBGRST1", NULL, NULL, NULL, NULL, NULL, NULL, "CL2_WARMRST0", "CL2_WARMRST1", "PINRESET", NULL, NULL, NULL,
		"APM_SDTRESET", "APM_SYSRESET", "VTS_WDTRESET", "CL2_WDTRESET", "CL0_WDTRESET", "AUD_WDTRESET", "SSS_WDTRESET",
		"DBGCORE_WDTRESET", "WRESET", "SWRESET", "PORESET", NULL
	};
	static const char *const on[8] = { "PWRON", "JIGONB", "ACOKB", "MRST", "ALARM1", "ALARM2", "SMPL", "WTSR" };
	static const char *const off[8] = { "SUB_OFF", "TSD", "TIMEOUT", "LDO_SYS_OK", "PWRHOLD", "RSVD5", "RSVD6", "UVLOB" };
	volatile uint8_t *p = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x1000);
	uint32_t st = p != NULL ? rd(p, 0x404) : 0;
	uint8_t onsrc = 0, offsrc = 0;
	int a = redwood_pmic_read(1, 0x10, &onsrc), b = redwood_pmic_read(1, 0x11, &offsrc);
	char line[256];
	size_t at = (size_t)snprintf(line, sizeof(line), "reset: last run ended by RST_STAT %#x (", st);

	for (unsigned i = 0; i < 32 && at < sizeof(line); i++) {
		if ((st >> i) & 1) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %s", rst[i] ? rst[i] : "?");
		}
	}
	at += (size_t)snprintf(line + at, sizeof(line) - at, " ); PMIC on %#x (", a == 0 ? onsrc : 0xff);
	for (unsigned i = 0; i < 8 && a == 0 && at < sizeof(line); i++) {
		if ((onsrc >> i) & 1) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %s", on[i]);
		}
	}
	at += (size_t)snprintf(line + at, sizeof(line) - at, " ) off %#x (", b == 0 ? offsrc : 0xff);
	for (unsigned i = 0; i < 8 && b == 0 && at < sizeof(line); i++) {
		if ((offsrc >> i) & 1) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %s", off[i]);
		}
	}
	if (at < sizeof(line)) {
		snprintf(line + at, sizeof(line) - at, " )\n");
	}
	snd_log("%s", line);
}

/* ---- entry points --------------------------------------------------------------------------------------------- */

void
redwood_exynos_sound_init(void)
{
	uint32_t on = 0;
	uint64_t pool_phys;
	vm_offset_t pool_virt;
	size_t pool_size;
	thread_t thread;

	reset_reason_log();
	{
		/* whatever the boot-args: a Calliope left running by the last run is stopped first */
		volatile uint8_t *p = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);

		if (p != NULL && (rd(p, 0x31a0) & 1)) {
			calliope_halt(p, (volatile uint8_t *)ml_io_map(SYSMMU_PHYS, 0x9000));
			snd_log("snd: Calliope was still running from the last run: stopped\n");
		}
	}
	if (!PE_parse_boot_argn("snd", &on, sizeof(on)) || on == 0) {
		return;
	}
	PE_parse_boot_argn("snd_below", &snd_below, sizeof(snd_below));
	/* x1slte experiments, after Scion's S9 ABOX (hakios.exynos_abox.c: RDMA0 CTRL0 0x400921 there, SIFS1 not the mixer) */
	PE_parse_boot_argn("snd_burst", &snd_burst, sizeof(snd_burst));
	PE_parse_boot_argn("snd_bufupd", &snd_bufupd, sizeof(snd_bufupd));
	PE_parse_boot_argn("snd_sifs1", &snd_sifs1, sizeof(snd_sifs1));
	PE_parse_boot_argn("snd_sifs_cnt", &snd_sifs_cnt, sizeof(snd_sifs_cnt));
	PE_parse_boot_argn("snd_sblimit", &snd_sblimit, sizeof(snd_sblimit));
	PE_parse_boot_argn("snd_gain", &snd_gain, sizeof(snd_gain));
	PE_parse_boot_argn("snd_tone", &snd_tone, sizeof(snd_tone));
	PE_parse_boot_argn("snd_calliope", &snd_calliope, sizeof(snd_calliope));
	PE_parse_boot_argn("snd_samsung", &snd_samsung, sizeof(snd_samsung));
	PE_parse_boot_argn("snd_gate", &snd_gate, sizeof(snd_gate));
	PE_parse_boot_argn("snd_debug", &snd_debug, sizeof(snd_debug));
	{
		/* the watchdog Samsung's bootloader leaves running after a cold start: off (wdt=0 keeps it; confirmed
		 * 2026-11-07: WTCON 0x18021 counting from 0x8000, the restarts at the lock screen) */
		uint32_t wdt = 1;

		PE_parse_boot_argn("wdt_off", &wdt, sizeof(wdt));
		if (wdt != 0) {
			snd_log("snd: watchdog WDT_CLUSTER0 %s\n", wdt_disable() ? "found running: off" : "clock would not come on");
		}
	}
	PE_parse_boot_argn("snd_cal_trig", &snd_cal_trig, sizeof(snd_cal_trig));
	PE_parse_boot_argn("snd_cal_kick", &snd_cal_kick, sizeof(snd_cal_kick));
	redwood_dbgport_add("snd.send", "ID TYPE [a0..a7]: one message to Calliope, 200 ms of its answer into the log", dbg_snd_send);
	redwood_dbgport_add("snd.log", ": Calliope's new log lines into the kernel log", dbg_snd_log);
	redwood_dbgport_add("snd.state", ": the DMA, UAIF2, routing, amplifiers, Calliope at a glance", dbg_snd_state);
	redwood_dbgport_add("snd.amp", "SIDE REG [VALUE]: a CS35L41 register (0 top, 1 bottom)", dbg_snd_amp);
	redwood_dbgport_add("snd.supply", "[1]: the amplifiers' 1.8 V (sub-PMIC LDO4S), read / switched on", dbg_snd_supply);
	redwood_dbgport_add("snd.prep", ": the amplifiers reset and set up again", dbg_snd_prep);
	redwood_dbgport_add("snd.cal", "0|1|2|3: Calliope halted / booted now / playback asked / its timers", dbg_snd_cal);
	redwood_dbgport_add("mic.look", ": the VTS domain and its bridges (always-on blocks only)", dbg_mic_look);
	redwood_dbgport_add("mic.bus", ": CMU_APM's VTS bus clock and BUS1's VTS bridges on", dbg_mic_bus);
	redwood_dbgport_add("mic.pd", ": the VTS power domain on, then EL3's DTZPC restore (after mic.bus)", dbg_mic_pd);
	redwood_dbgport_add("mic.clk", ": the microphone clocks for 48 kHz / 24 bits (after mic.cmu)", dbg_mic_clk);
	redwood_dbgport_add("mic.slif", ": the DMIC pins, DMIC_AUD0 and the serial LIF as Samsung sets them (after mic.clk)", dbg_mic_slif);
	redwood_dbgport_add("mic.cap", "[ms]: ABOX's capture side (UAIF6 -> NSRC4 -> WDMA4) on, then each channel's peak (after mic.slif)", dbg_mic_cap);
	redwood_dbgport_add("mic.peek", ": current capture peaks without reinitializing the path", dbg_mic_peek);
	redwood_dbgport_add("mic.stop", ": capture off", dbg_mic_stop);
	redwood_dbgport_add("wdt.look", ": WDT_CLUSTER0's clock gate, and (only with it on) its WTCON/WTDAT/WTCNT", dbg_wdt_look);
	redwood_dbgport_add("wdt.off", ": WDT_CLUSTER0 off (its gate opened first)", dbg_wdt_off);
	redwood_dbgport_add("pmic.r", "SUB TYPE REG: a PMIC register (SUB 0 main S2MPS19, 1 sub S2MPS22; TYPE 1 the PMIC block)", dbg_pmic_r);
	redwood_dbgport_add("pmic.w", "SUB TYPE REG VAL: a PMIC register written, read back", dbg_pmic_w);
	redwood_dbgport_add("mic.vts", "[0]: VTS's Cortex-M core booted with fw/vts.bin as Samsung does before capture (0: off)", dbg_mic_vts);
	redwood_dbgport_add("mic.cmu", ": CMU_VTS's microphone clocks, read (after mic.pd)", dbg_mic_cmu);
	redwood_dbgport_add("snd.gate", "[N]: the silence gate (of 32767), read / set", dbg_snd_gate);
	redwood_dbgport_add("snd.amps", "0|1: the amplifiers off / powered up now", dbg_snd_amps);
	redwood_dbgport_add("snd.health", ": the sound and mic drivers' self-check (no hardware access)", dbg_snd_health);
	if (snd_calliope >= 2) {
		snd_rdma = 0;
	}
	if (snd_calliope >= 2 && !PE_parse_boot_argn("snd_tone", &snd_tone, sizeof(snd_tone))) {
		snd_tone = 2000;         /* stage 3's own test: 2 s of 1 kHz once the amplifiers are up */
	}
	if (snd_gain > REDWOOD_GUARD_MAX_GAIN) {
		snd_gain = REDWOOD_GUARD_MAX_GAIN;
	}
	if (snd_tone > 3000) {
		snd_tone = 3000;
	}
	/* the DMA's ring: the last 64 KB of the loader's pool below 4 GB (the Wi-Fi driver only uses its first 4 MB) */
	if (redwood_wifi_dma_pool(&pool_phys, &pool_virt, &pool_size) < 0 || pool_size < (8U << 20)) {
		snd_log("snd: no DMA pool from the loader: no sound\n");
		return;
	}
	buf_phys = pool_phys + pool_size - 0x10000;
	buf = (int16_t *)ml_io_map_unmappable((vm_offset_t)buf_phys, 0x10000, VM_WIMG_WCOMB);
	pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x4000);        /* (to 0x3224: the Calliope cores' power, snd_calliope) */
	cmu = (volatile uint8_t *)ml_io_map(CMU_AUD_PHYS, 0x4000);
	mmu = (volatile uint8_t *)ml_io_map(SYSMMU_PHYS, 0x9000);
	abox = (volatile uint8_t *)ml_io_map(ABOX_PHYS, 0x2000);
	if (buf == NULL || pmu == NULL || cmu == NULL || mmu == NULL || abox == NULL) {
		snd_log("snd: cannot map\n");
		return;
	}
	snd_log("snd=1: the speakers through ABOX; DMA ring at %#llx\n", buf_phys);
	started = true;
	if (kernel_thread_start_priority(sound_thread, NULL, 90, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		uint32_t mic_enable = 1;

		PE_parse_boot_argn("mic_ios", &mic_enable, sizeof(mic_enable));
		if (mic_enable != 0 && kernel_thread_start_priority(exynos_mic_thread, NULL, 80, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	} else {
		started = false;
	}
}

/* at the run's end and on a panic's way out: the amplifiers into reset at once, the DMA stopped (its memory is reused) */
void
redwood_exynos_sound_quiesce(void)
{
	if (!started) {
		return;
	}
	/* Other cores are stopped here; no sleeps, locks or console output. */
	mic_ios_wants = false;
	if (mic_abox != NULL) {
		wr(mic_abox, MIC_WDMA4, rd(mic_abox, MIC_WDMA4) & ~1U);
		wr(mic_abox, MIC_UAIF6_CTRL0, rd(mic_abox, MIC_UAIF6_CTRL0) & ~2U);
	}
	if (mic_slif != NULL && mic_pd_open) {
		wr(mic_slif, 0x300, 0);
		wr(mic_slif, 0x100, 0);
	}
	if (gpb2 != NULL) {
		for (unsigned i = 0; i < 2; i++) {
			redwood_gpio_set(gpb2, amps[i].reset_pin, 0);
		}
	}
	if (abox != NULL && (rd(pmu, PMU_AUD_STATUS) & 1)) {
		upd(abox, RDMA(RDMA_CTRL0), 1, 0);
		upd(abox, ABOX_UAIF_CTRL0, 1, 0);
	}
	if (cal_dram != NULL) {
		calliope_halt(pmu, mmu);
	}
	started = false;
}

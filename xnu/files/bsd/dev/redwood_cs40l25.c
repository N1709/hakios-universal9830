/*
 * DRAFT, NOT BUILT, NOT RUN. A proposal for bsd/dev/redwood_cs40l25.c; see proposals/haptics/NOTES.md.
 *
 * The Galaxy S20's (x1slte, SM-G980F) vibration motor: a linear resonant actuator driven by a Cirrus Logic
 * CS40L25A (rev B1), a boosted class-D haptic amplifier with its own DSP ("Halo"), at I2C 0x40 on HSI2C_5
 * (hsi2c@105A0000; gpp1-2 SCL, gpp1-3 SDA, function 3; bit-banged here, bsd/dev/redwood_bbi2c.c). From the
 * phone's dtbo (dtbo.1.dtb, fragment@48 cs40l25@40 on __fixups__ hsi2c_5; fragment@47 hsi2c5-bus-vib and
 * motor-rst; fragment@46 motor-int) and Samsung's drivers/motor/cs40l2x.c, cs40l2x.h, cs40l2x-tables.c
 * (ExtremeXT; CONFIG_MOTOR_CS40L2X=y in extreme_x1slte_defconfig):
 *
 *   I2C        0x40, 32-bit big-endian register addresses (stride 4) and 32-bit big-endian values
 *              (cs40l2x.c:9064 regmap: reg_bits 32, val_bits 32, both REGMAP_ENDIAN_BIG). No pull-ups of
 *              the SoC's (hsi2c5-bus-vib: pin-pud 0): the board's own
 *   reset      gpp4-5 (PERIC0, bank 0x80), high = running; Samsung's probe: low (GPIOD_OUT_LOW), 2 ms, high,
 *              1 ms before the first transaction (cs40l2x.c:9198-9231)
 *   interrupt  gpm27-0 (CMGP 0x15C30000, bank 0x360; EINT, function 0xf), low = /ALERT. Not used: polled
 *   supplies   "VA" and "VP" are not in the dtbo: Samsung's driver gets dummy regulators (log of an S20 Ultra:
 *              "supply VA not found, using dummy regulator"): VA is an always-on rail, VP the battery. So the
 *              reset pin is the only power control there is
 *   boost      cirrus,boost-ind-nanohenry 1000, boost-cap-microfarad 4, boost-ipk-milliamp 3100; no
 *              boost-ctl-millivolt, so Samsung's default VBST limit of 11 V (cs40l2x.c:7666 boost_config)
 *   other dt   gpio1-mode 1 (GPIO1 trigger off), refclk-gpio2 (the B1 ROM sets the clock itself),
 *              amp-gnd-stby, auto-recovery, hiber-enable (not done here: the chip stays in standby)
 *
 * The firmware question (NOTES.md, "Firmware"): out of reset the B1 chip runs its ROM, "basic mode"
 * (registers 0x02800154..0x02800190: a state machine, a heartbeat and a ROM buzz -- frequency, level,
 * duration, trigger). Samsung's kernel never plays anything with it: it waits for the ROM's boot, shuts the
 * ROM down (cs40l2x.c:8662 basic_mode_exit) and loads /vendor/firmware/cs40l25a.wmfw (the DSP program, ID
 * 0x1400C3 "REMAP") and the wavetable cs40l20.bin (143 waveforms) into the DSP's memories, then plays by
 * mailbox writes. Samsung's bootloader DOES buzz with the ROM (LK of an S20 Ultra: "register_cs40l2x_vib
 * freq. 158Hz 130Vrms 25ms" / "cs40l2x_vib_enable", before Linux), so a vibration without firmware is
 * possible, but how the ROM's registers are encoded is in no source we have. So:
 *
 *   - the way that is Samsung's (haptic=3..5 here): firmware from fw/ of the small image (userland/build.sh,
 *     ../fw-haptic: cs40l25a.wmfw 38224 bytes, cs40l20.bin 8492 bytes, from the phone's /vendor/firmware)
 *   - the ROM's buzz only as an experiment (haptic_rom=1): the ROM's registers are read as the bootloader
 *     left them (haptic=1 prints them) and, if the chip is found still in basic mode, the trigger is written
 *     ONCE with the bootloader's own frequency, level and duration untouched; whatever happens, the chip is
 *     put into reset 200 ms later. Nothing else is ever written to the ROM's registers.
 *
 * Nothing here is done unless a boot-arg asks for it, and only with haptic_chip=cs40l25 (redwood_haptic.c's
 * redwood_haptic_init() hands over to redwood_cs40l25_init(); the AW86927 code of the POCO is never run):
 *
 *   haptic=1   a look, reading only: the pins (bus, reset, interrupt) and, if the reset pin is high, the
 *              chip's ID, revision and the ROM's registers as the bootloader left them. Nothing is written,
 *              not even a pin.
 *   haptic=2   ... and the reset given as Samsung gives it (low 2 ms, high, 1 ms), the OTP boot waited for,
 *              ID, revision and the ROM's registers read again. Nothing is written to the chip.
 *   haptic=3   ... and Samsung's configuration (the ROM shut down, the B0/B1 errata, the boost converter, the
 *              signal routing, every write read back), the firmware and the wavetable loaded, the DSP
 *              started and configured as Samsung's driver does. Then standby: nothing moves.
 *   haptic=4   ... and one effect: haptic_effect=0 (default) Samsung's own timed vibration, waveform 0 for
 *              80 ms (what Samsung's recovery plays: "cs40l2x_vibe_enable: 80ms"); N>0 the wavetable's
 *              click N, once.
 *   haptic=5   ... and the motor for iOS until the run ends: /dev/redwood-haptic, THE SAME CONTRACT as the
 *              POCO's (bsd/dev/redwood_haptic.c): what iOS plays on its "Actuator" (userland/src/
 *              audio_plugin.c, actuator_write()) comes as 16-bit samples of one channel; a waveform after a
 *              pause is a tap, one that goes on past TAP_US a vibration until it stops.
 *
 * Other boot-args: haptic_max_ms (default 1000, 100..5000) the longest vibration; haptic_max_pct (default
 * 80, 1..100) the strongest, as a share of Samsung's full scale; haptic_tap (default 0: waveform 0 for
 * TAP_MS; N>0: the wavetable's click N); haptic_rom=1 the ROM experiment above; haptic_verify=1 every block
 * of firmware read back as well (doubles the load time).
 *
 * Limits, whatever asks: nothing louder than haptic_max_pct of Samsung's 100 % (the DSP's GAIN_CONTROL,
 * Samsung's own attenuation table); no vibration longer than haptic_max_ms -- cut by this driver and, as a
 * second line, by the DSP itself (its TIMEOUT_MS set to haptic_max_ms + 50, where Samsung sets 174 s), then a
 * rest as long (no battery-temperature cutoff: the chip's own temperature faults below). Any hardware error the chip reports
 * (amplifier short, over-temperature, temperature rise, boost short, under- or over-voltage: IRQ2_STATUS1),
 * a known register that no longer reads as written (the chip reset itself: Samsung's "auto-recovery"), a
 * write that does not read back or a byte nobody acknowledges: the chip is put into reset and nothing more
 * is done for the rest of the run. Samsung releases most of those errors and goes on; this does not. At the
 * run's end and on a panic's way out the reset pin goes low (a pin write only: no I2C on the way out).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/redwood_m2m.h>
#include <stdarg.h>

/*
 * Every line into the kernel's log and into mali2metal's channel too (m2m: /m2m on rwlogd). The log keeps only the
 * first 256 KB of a boot and the newest part, and this driver's lines come when the first part is full: on the first
 * run on the phone not one of them was left (2026-11-06).
 */
static void hsay(const char *format, ...) __printflike(1, 2);

static void
hsay(const char *format, ...)
{
	char line[300];
	va_list ap;
	size_t n;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);     /* (not at the run's end: console I/O with interrupts off panics, B 2026-11-07) */
	}
	n = strlen(line);
	if (n > 0 && line[n - 1] == '\n') {
		line[n - 1] = 0;
	}
	m2m_klog('I', "DEV", "%s", line);
}
#include <sys/uio.h>

extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

/* ---- the board (dtbo.1.dtb, exynos9830.dts, pinctrl-exynos-arm64.c exynos9830_pin_banks1/4) ------------- */

#define BANK_GPP1               0x20U           /* PERIC0: gpp1-2 SCL, gpp1-3 SDA (hsi2c5-bus-vib) */
#define BANK_GPP4               0x80U           /* PERIC0: gpp4-5 reset (motor-rst, function 1 = output) */
#define PIN_RESET               5
#define REDWOOD_PINCTRL_CMGP    0x15C30000U     /* pinctrl@15C30000 (pinctrl1, CMGP) */
#define BANK_GPM27              0x360U          /* gpm27, one pin: the chip's /ALERT (motor-int) */

#define CS_ADDR                 0x40U           /* cs40l25@40: reg = <0x40> */

/* ---- registers (cs40l2x.h: line of each) ---------------------------------------------------------------- */

#define R_DEVID                 0x00000000U     /* :23 */
#define R_REVID                 0x00000004U     /* :24 */
#define R_TEST_KEY_CTL          0x00000040U     /* :29 */
#define R_PWR_CTRL1             0x00002014U     /* :42  bit 0 GLOBAL_EN */
#define R_PWR_CTRL2             0x00002018U     /* :43 */
#define R_LRCK_PAD_CONTROL      0x00002418U     /* :48 */
#define R_SDIN_PAD_CONTROL      0x00002420U     /* :49 */
#define R_PWRMGT_CTL            0x00002900U     /* :52  bit 1 MEM_RDY */
#define R_WAKESRC_CTL           0x00002904U     /* :53 */
#define R_PLL_LOOP_PARAM        0x00003008U     /* :61 */
#define R_PLL_MISC_CTRL         0x00003014U     /* :62 */
#define R_BSTCVRT_VCTRL1        0x00003800U     /* :74  bits 7:0 the VBST limit */
#define R_BSTCVRT_VCTRL2        0x00003804U     /* :75  bit 2 LIM_EN, bits 1:0 target (0 CP value, 1 class H) */
#define R_BSTCVRT_PEAK_CUR      0x00003808U     /* :76  bits 6:0 */
#define R_BSTCVRT_COEFF         0x00003810U     /* :78  bits 7:0 K1, 15:8 K2 */
#define R_BSTCVRT_SLOPE_LBST    0x00003814U     /* :79  bits 15:8 slope, 1:0 the inductor */
#define R_TEST_LBST             0x0000391CU     /* :84 */
#define R_SPKMON_RESYNC         0x00004100U     /* :86 */
#define R_OTP_TRIM_12           0x00004170U     /* :522 */
#define R_TEMP_RESYNC           0x00004310U     /* :90 */
#define R_OTP_TRIM_13           0x00004360U     /* :523 */
#define R_VPVBST_FS_SEL         0x00004400U     /* :91 */
#define R_DAC_PCM1_SRC          0x00004C00U     /* :100 */
#define R_DSP1_RX1_SRC          0x00004C40U     /* :105..108 */
#define R_DSP1_RX2_SRC          0x00004C44U
#define R_DSP1_RX3_SRC          0x00004C48U
#define R_DSP1_RX4_SRC          0x00004C4CU
#define R_AMP_DIG_VOL_CTRL      0x00006000U     /* :115 */
#define R_IRQ1_STATUS1          0x00010010U     /* :131 */
#define R_IRQ1_STATUS3          0x00010018U     /* :133 bit 31 OTP_BOOT_ERR */
#define R_IRQ1_STATUS4          0x0001001CU     /* :134 bit 1 OTP_BOOT_DONE */
#define R_IRQ2_STATUS1          0x00010810U     /* :156 the hardware errors */
#define R_IRQ2_MASK1            0x00010910U     /* :164..167 */
#define R_IRQ2_MASK2            0x00010914U
#define R_MBOX_TRIGGERINDEX     0x00013020U     /* :195 DSP_VIRT1_MBOX_1 (:831): play wavetable entry N once */
#define R_MBOX_TRIGGER_MS       0x00013024U     /* :196 DSP_VIRT1_MBOX_2 (:832): play entry N until stopped */
#define MBOX_TRIGGERRESET       0xFFFFFFFFU     /* :830 what a mailbox reads once the DSP has taken it */

#define R_DSP1_XMEM_PACK_0      0x02000000U     /* :231 */
#define R_DSP1_XMEM_UNPACK24_0  0x02800000U     /* :268 */
#define R_DSP1_RX1_RATE         0x02B80080U     /* :275, 8 apart up to RX8 */
#define R_DSP1_TX1_RATE         0x02B80280U     /* :283, 8 apart up to TX8 */
#define R_DSP1_SCRATCH1         0x02B805C0U     /* :323 */
#define R_DSP1_CCM_CORE_CTRL    0x02BC1000U     /* :327 bit 9 RESET, bit 0 EN */
#define R_DSP1_XM_ACCEL_PL0_PRI 0x02BC2020U     /* :333 */
#define R_DSP1_YM_ACCEL_PL0_PRI 0x02BC20E0U     /* :339 */
#define R_DSP1_YMEM_PACK_0      0x02C00000U     /* :501 */
#define R_DSP1_YMEM_UNPACK24_0  0x03400000U     /* :505 */
#define R_DSP1_PMEM_0           0x03800000U     /* :507 */
#define R_XM_FW_ID              (R_DSP1_XMEM_UNPACK24_0 + 12)   /* :800 the algorithm list starts here */
#define R_XM_NUM_ALGOS          (R_DSP1_XMEM_UNPACK24_0 + 36)   /* :801 */

/* the ROM ("basic mode", :548-574) */
#define R_BASIC_HALO_STATE      0x02800154U
#define R_BASIC_HALO_HEARTBEAT  0x02800158U
#define R_BASIC_STATEMACHINE    0x0280015CU     /* 0 idle, 1 play tone, 2 continue tone, 3 shutting down, 4 shut down */
#define R_BASIC_F0_STORED       0x02800174U
#define R_BASIC_GPIO_TRIG_EDGE  0x02800178U
#define R_BASIC_BUZZ_FREQ       0x0280017CU
#define R_BASIC_BUZZ_LEVEL      0x02800180U
#define R_BASIC_BUZZ_DURATION   0x02800184U
#define R_BASIC_BUZZ_TRIGGER    0x02800188U
#define R_BASIC_AMP_STATUS      0x0280018CU     /* bit 0 boot done, 1 OTP error, 2 amp error, 3 temp rise, 4 temp error */
#define R_BASIC_SHUTDOWNREQUEST 0x02800190U
#define BASIC_SHUTDOWN          4
#define BASIC_BOOT_DONE         0x01U
#define BASIC_ERRORS            0x1EU

#define DEVID_L25A              0x40A25AU       /* :762 */
#define REVID_B1                0xB1U           /* :767 */
#define FW_ID_REMAP             0x1400C3U       /* :987, cs40l2x-tables.c:1001: cs40l25a.wmfw, the default for L25A */
#define FW_MIN_REV              0x080100U       /* cs40l2x-tables.c:1003 */
#define FW_HALO_STATE_RUN       203             /* cs40l2x-tables.c:1004 */
#define ALGO_ID_VIBE            0x0000BDU       /* :1052 VIBEGEN */
#define ALGO_LIST_TERM          0xBEDEADU       /* :809 */
#define ALGO_REV_MASK           0xFF0000U       /* :810 */

/* the hardware errors (cs40l2x.h:737-742; cs40l2x-tables.c cs40l2x_hw_errs) */
#define ERR_AMP                 0x80000000U
#define ERR_TEMP                0x00020000U
#define ERR_TEMP_RISE           0x00008000U
#define ERR_BST_SHORT           0x00000100U
#define ERR_BST_UVP             0x00000080U
#define ERR_BST_OVP             0x00000040U
#define HW_ERRORS               (ERR_AMP | ERR_TEMP | ERR_TEMP_RISE | ERR_BST_SHORT | ERR_BST_UVP | ERR_BST_OVP)

/* the firmware files (cs40l2x.h:772-780, 782-797) */
#define WMFW_HEADER             40
#define WT_HEADER               16
#define T_XM_UNPACKED           0x0005U
#define T_YM_UNPACKED           0x0006U
#define T_PM_PACKED             0x10U
#define T_XM_PACKED             0x11U
#define T_YM_PACKED             0x12U
#define T_ALGO_INFO             0xF2U
#define T_WMFW_INFO             0xFFU
#define T_WMDR_NAME             0xFE00U
#define T_WMDR_INFO             0xFF00U
#define MAX_WLEN                4096            /* :980, the longest single write Samsung makes */
#define CHUNK                   256             /* ours: a bit-banged write that short rarely meets a disturbance */

#define TAP_MS                  30              /* haptic_tap=0: a tap is waveform 0 for this long */

/* ---- state -------------------------------------------------------------------------------------------- */

static struct redwood_bbi2c bus = { .name = "cs40l25 bus gpp1-2/3", .scl = 2, .sda = 3, .half_us = 4 };
static volatile uint8_t *reset_bank, *alert_bank;
static uint32_t level, max_ms = 1000, max_pct = 34, tap_index, rom_test, verify;     /* (the user, 2026-11-07: 70 % of the old 80 -> 56; 17:10: 60 % of 56 -> 34) */
static volatile bool released, broken;
static bool bus_taken;
static const char *step = "start";

/*
 * kern.redwood.haptic (x1slte, 2026-11-06): how far the bring-up got and why it stopped, readable at any time
 * (a boot's first lines are gone from the log within the hour; the user's "it stopped vibrating" had nothing to read).
 */
#include <sys/sysctl.h>
static char haptic_state[160] = "not started";
static uint32_t haptic_attempts;
SYSCTL_DECL(_kern_redwood);
SYSCTL_STRING(_kern_redwood, OID_AUTO, haptic, CTLFLAG_RD | CTLFLAG_LOCKED, haptic_state, 0,
    "the vibration motor: serving, or where its bring-up stopped");
SYSCTL_UINT(_kern_redwood, OID_AUTO, haptic_attempts, CTLFLAG_RD | CTLFLAG_LOCKED, &haptic_attempts, 0,
    "bring-up attempts this boot (3 at most)");

/* ROM values as found before anything was done (haptic=1's look) */
static bool rom_alive;
static uint32_t rom_regs[10];

/* the DSP's controls, from the firmware's algorithm blocks, given their addresses once it is loaded */
#define MAX_CONTROLS            160
#define MAX_ALGOS               16
static struct control {
	uint32_t        algo, type, offset, reg;
	char            name[32];
} controls[MAX_CONTROLS];
static unsigned ncontrols;
static struct {
	uint32_t        id, rev, xm_base, xm_size, ym_base, ym_size;
} algos[MAX_ALGOS];
static unsigned nalgos;
static uint32_t num_waves, gain_reg, endplayback_reg;

/* Samsung's write sequencer: what the DSP writes again when it wakes from hibernation (cs40l2x.c:887-978) */
#define WSEQ_MAX                48
static struct {
	uint32_t        reg, val;
} wseq[WSEQ_MAX];
static unsigned nwseq;

static uint8_t xbuf[4 + MAX_WLEN], vbuf[MAX_WLEN];
static uint32_t block_retries;

/* ---- the reset pin and the bus -------------------------------------------------------------------------- */

static void
reset_pin(bool high)
{
	redwood_gpio_set(reset_bank, PIN_RESET, high ? 1 : 0);
	released = high;
}

static bool
reset_is_high(void)
{
	uint32_t con = (*(volatile uint32_t *)(reset_bank + REDWOOD_GPIO_CON) >> (PIN_RESET * 4)) & 0xf;

	return con == REDWOOD_GPIO_OUTPUT && redwood_gpio_get(reset_bank, PIN_RESET);
}

static void
give_up(const char *why)
{
	hsay("haptic: cs40l25: %s (at: %s): the chip is put into reset\n", why, step);
	snprintf(haptic_state, sizeof(haptic_state), "stopped at %s: %s (attempt %u)", step, why, haptic_attempts);
	broken = true;
	if (reset_bank != NULL) {
		reset_pin(false);
	}
	if (bus_taken) {
		redwood_bbi2c_restore(&bus);
		bus_taken = false;
	}
}

static inline void
put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline uint32_t
le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static inline uint32_t
le24(const uint8_t *p)
{
	return le16(p) | (uint32_t)p[2] << 16;
}

static inline uint32_t
le32(const uint8_t *p)
{
	return le24(p) | (uint32_t)p[3] << 24;
}

/*
 * One I2C transfer, sent again (the bus freed first) up to twice more when it is not acknowledged (x1slte, 2026-11-06:
 * a single lost read while serving iOS put the motor in reset for the rest of the boot; the bus is bit-banged on a
 * GPIO bank the battery gauge's bus shares). The chip only acts on a transfer it acknowledged, so a retry is safe.
 */
static uint32_t xfer_retries;

static bool
xfer_retry(const uint8_t *w, uint32_t wlen, uint8_t *r, uint32_t rlen)
{
	for (int tries = 0; tries < 3; tries++) {
		if (tries > 0) {
			char why[160];

			delay_for_interval(2, NSEC_PER_MSEC);
			(void) redwood_bbi2c_setup(&bus, false, why, sizeof(why));
			xfer_retries++;
		}
		if (redwood_bbi2c_xfer(&bus, CS_ADDR, w, wlen, r, rlen)) {
			return true;
		}
	}
	return false;
}

/* one register; false (and no give_up) if nobody answers: a look may ask a chip held in reset */
static bool
rd(uint32_t reg, uint32_t *v)
{
	uint8_t a[4], b[4];

	if (broken) {
		return false;
	}
	put_be32(a, reg);
	if (!xfer_retry(a, 4, b, 4)) {
		return false;
	}
	*v = be32(b);
	return true;
}

/* `len' bytes from `reg' on (the chip's address increments by itself: Samsung's regmap_raw_write relies on it) */
static bool
rd_raw(uint32_t reg, uint8_t *data, uint32_t len)
{
	uint8_t a[4];

	put_be32(a, reg);
	return !broken && redwood_bbi2c_xfer(&bus, CS_ADDR, a, 4, data, len);
}

static bool
wr_raw(uint32_t reg, const uint8_t *data, uint32_t len)
{
	for (uint32_t done = 0; done < len && !broken; ) {
		uint32_t n = len - done > MAX_WLEN ? MAX_WLEN : len - done;

		bool sent = false;

		put_be32(xbuf, reg + done);
		memcpy(xbuf + 4, data + done, n);
		/*
		 * x1slte (2026-11-06): a 4 KB block bit-banged is a third of a second on a bank the battery gauge's bus shares;
		 * the first run lost one (not acknowledged). Shorter blocks (CHUNK, the chip's address auto-increments) and,
		 * for a block that still fails, the bus freed and the block sent again, three times at most.
		 */
		for (int tries = 0; tries < 3 && !sent; tries++) {
			if (tries > 0) {
				char why[160];

				(void) redwood_bbi2c_setup(&bus, false, why, sizeof(why));
				block_retries++;
			}
			sent = redwood_bbi2c_xfer(&bus, CS_ADDR, xbuf, 4 + n, NULL, 0);
		}
		if (!sent) {
			hsay("haptic: cs40l25: %u bytes at %#010x not acknowledged three times (%u retries so far)\n", n, reg + done,
			    block_retries);
			give_up("a block write was not acknowledged");
			return false;
		}
		if (verify && (!rd_raw(reg + done, vbuf, n) || memcmp(vbuf, data + done, n) != 0)) {
			hsay("haptic: cs40l25: %u bytes at %#010x do not read back as written\n", n, reg + done);
			give_up("a block does not read back");
			return false;
		}
		done += n;
	}
	return !broken;
}

/* a write that is not read back: test keys, triggers, mailboxes, write-1-to-clear status */
static bool
wr_once(uint32_t reg, uint32_t v)
{
	uint8_t b[8];

	if (broken) {
		return false;
	}
	put_be32(b, reg);
	put_be32(b + 4, v);
	if (!xfer_retry(b, 8, NULL, 0)) {
		give_up("a write was not acknowledged");
		return false;
	}
	return true;
}

/* every other write: read back */
static bool
wr(uint32_t reg, uint32_t v)
{
	uint32_t back = 0;

	if (!wr_once(reg, v)) {
		return false;
	}
	if (!rd(reg, &back) || back != v) {
		hsay("haptic: cs40l25: %#010x written %#010x, reads back %#010x\n", reg, v, back);
		give_up("a write does not read back");
		return false;
	}
	return true;
}

static bool
upd(uint32_t reg, uint32_t mask, uint32_t v)
{
	uint32_t before;

	if (!rd(reg, &before)) {
		give_up("a register cannot be read");
		return false;
	}
	return wr(reg, (before & ~mask) | (v & mask));
}

/* Samsung's ack_write (cs40l2x.c:6840): write, then wait (10 x 1 ms) for the DSP to set it back to `reset' */
static bool
ack(uint32_t reg, uint32_t v, uint32_t reset)
{
	uint32_t now = ~reset;

	if (!wr_once(reg, v)) {
		return false;
	}
	for (int i = 0; i < 10; i++) {
		delay_for_interval(1, NSEC_PER_MSEC);
		if (rd(reg, &now) && now == reset) {
			return true;
		}
	}
	hsay("haptic: cs40l25: %#010x = %#010x, not taken (wanted %#010x)\n", reg, now, reset);
	give_up("the chip did not take a command");
	return false;
}

static bool
wseq_add(uint32_t reg, uint32_t val)
{
	if (nwseq == WSEQ_MAX) {
		give_up("the write sequence is full");
		return false;
	}
	wseq[nwseq].reg = reg;
	wseq[nwseq++].val = val;
	return true;
}

static void
wseq_replace(uint32_t reg, uint32_t val)
{
	for (unsigned i = 0; i < nwseq; i++) {
		if (wseq[i].reg == reg) {
			wseq[i].val = val;
		}
	}
}

/* ---- haptic=1: a look ------------------------------------------------------------------------------------ */

static void
show_pins(void)
{
	volatile uint8_t *gpp1 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, BANK_GPP1);
	uint32_t c1 = *(volatile uint32_t *)(gpp1 + REDWOOD_GPIO_CON), c4 = *(volatile uint32_t *)(reset_bank + REDWOOD_GPIO_CON);
	uint32_t cm = *(volatile uint32_t *)(alert_bank + REDWOOD_GPIO_CON);

	hsay("haptic: cs40l25: gpp1-2/3 (HSI2C_5) function %u/%u (3: the I2C controller's), read %u/%u; gpp4-5 (reset) "
	    "function %u, reads %u; gpm27-0 (/ALERT) function %#x (0xf: EINT), reads %u\n", (c1 >> 8) & 0xf, (c1 >> 12) & 0xf,
	    redwood_gpio_get(gpp1, 2), redwood_gpio_get(gpp1, 3), (c4 >> (PIN_RESET * 4)) & 0xf, redwood_gpio_get(reset_bank, PIN_RESET),
	    cm & 0xf, redwood_gpio_get(alert_bank, 0));
}

static const uint32_t rom_reg_list[10] = {
	R_BASIC_HALO_STATE, R_BASIC_HALO_HEARTBEAT, R_BASIC_STATEMACHINE, R_BASIC_F0_STORED, R_BASIC_GPIO_TRIG_EDGE,
	R_BASIC_BUZZ_FREQ, R_BASIC_BUZZ_LEVEL, R_BASIC_BUZZ_DURATION, R_BASIC_BUZZ_TRIGGER, R_BASIC_AMP_STATUS,
};

/* The chip's ID and the ROM's registers; true if it answers. Reads only. */
static bool
look(const char *when, uint32_t *devid, uint32_t *revid, uint32_t regs[10])
{
	uint32_t s3 = 0, s4 = 0;

	if (!rd(R_DEVID, devid) || !rd(R_REVID, revid)) {
		hsay("haptic: cs40l25: %s: no answer at %#x\n", when, CS_ADDR);
		return false;
	}
	for (unsigned i = 0; i < 10; i++) {
		if (!rd(rom_reg_list[i], &regs[i])) {
			regs[i] = 0xdeadbeef;
		}
	}
	(void)rd(R_IRQ1_STATUS3, &s3);
	(void)rd(R_IRQ1_STATUS4, &s4);
	hsay("haptic: cs40l25: %s: DEVID %#08x (CS40L25A: %#x) REVID %#04x (B1: %#x); OTP done %u error %u; ROM: halo "
	    "state %u heartbeat %u state machine %u f0 %#x gpio-edge %#x BUZZ freq %u level %u duration %u trigger %u, "
	    "amp status %#x\n", when, *devid, DEVID_L25A, *revid, REVID_B1, (s4 >> 1) & 1, s3 >> 31, regs[0], regs[1], regs[2],
	    regs[3], regs[4], regs[5], regs[6], regs[7], regs[8], regs[9]);
	return true;
}

/*
 * haptic_rom=1: the ROM's buzz, as the bootloader set it, played once. EXPERIMENTAL: the trigger's meaning
 * (a write of 1) is a guess -- Samsung's kernel names the register and never writes it. The frequency,
 * level and duration are not touched: they are the bootloader's (LK: "158Hz 130Vrms 25ms"), which it plays
 * at every power-on. The state machine is watched; 200 ms later the chip goes into reset whatever it does.
 */
static void
rom_buzz(void)
{
	uint32_t state = 0, status = 0;
	unsigned seen_playing = 0;

	if (!rom_alive || rom_regs[2] != 0 || (rom_regs[9] & BASIC_BOOT_DONE) == 0 || (rom_regs[9] & BASIC_ERRORS) != 0 ||
	    rom_regs[5] == 0 || rom_regs[7] == 0) {
		hsay("haptic: cs40l25: ROM buzz not tried: the ROM is not idle with the bootloader's buzz in its registers\n");
		return;
	}
	step = "the ROM's buzz (experimental)";
	hsay("haptic: cs40l25: ROM BUZZ (experimental): trigger written with the bootloader's freq %u level %u duration %u\n",
	    rom_regs[5], rom_regs[6], rom_regs[7]);
	if (!wr_once(R_BASIC_BUZZ_TRIGGER, 1)) {
		return;
	}
	for (int i = 0; i < 40; i++) {
		delay_for_interval(5, NSEC_PER_MSEC);
		if (rd(R_BASIC_STATEMACHINE, &state) && (state == 1 || state == 2)) {
			seen_playing++;
		}
	}
	(void)rd(R_BASIC_AMP_STATUS, &status);
	hsay("haptic: cs40l25: ROM buzz: state machine %s (playing in %u of 40 looks, now %u), amp status %#x\n",
	    seen_playing ? "WENT TO PLAY TONE" : "never left idle", seen_playing, state, status);
	reset_pin(false);
	delay_for_interval(2, NSEC_PER_MSEC);
}

/* ---- haptic=2: out of reset, as Samsung's probe --------------------------------------------------------- */

static bool
come_out_of_reset(void)
{
	uint32_t s3 = 0, s4 = 0, devid = 0, revid = 0, regs[10];
	int i;

	step = "reset";
	reset_pin(false);                               /* devm_gpiod_get_optional(..., GPIOD_OUT_LOW) */
	delay_for_interval(2, NSEC_PER_MSEC);           /* "satisfy reset pulse width specification" */
	reset_pin(true);
	delay_for_interval(1, NSEC_PER_MSEC);           /* "satisfy control port delay specification" */
	/* cs40l2x_part_num_resolve: OTP boot done within 10 x 10 ms, no OTP error */
	for (i = 0; i < 10; i++) {
		delay_for_interval(10, NSEC_PER_MSEC);
		if (rd(R_IRQ1_STATUS4, &s4) && (s4 & 0x2)) {
			break;
		}
	}
	if (i == 10) {
		give_up("no OTP boot within 100 ms");
		return false;
	}
	if (!rd(R_IRQ1_STATUS3, &s3) || (s3 & 0x80000000U)) {
		give_up("an OTP boot error");
		return false;
	}
	if (!look("out of reset", &devid, &revid, regs)) {
		give_up("no answer out of reset");
		return false;
	}
	if (devid != DEVID_L25A || revid < REVID_B1) {
		give_up("not a CS40L25A of revision B1 or later: Samsung's sequence below is for that one");
		return false;
	}
	return true;
}

/* ---- haptic=3: Samsung's configuration ------------------------------------------------------------------ */

/* cs40l2x.c:8662 basic_mode_exit: the ROM booted, beating, then asked to shut down */
static bool
rom_exit(void)
{
	uint32_t v = 0, hb0 = 0;
	int i;

	step = "the ROM shut down";
	for (i = 0; i < 10 && !(rd(R_BASIC_AMP_STATUS, &v) && (v & BASIC_BOOT_DONE)); i++) {
		delay_for_interval(5, NSEC_PER_MSEC);
	}
	if (i == 10 || !rd(R_BASIC_HALO_HEARTBEAT, &hb0)) {
		give_up("the ROM did not boot");
		return false;
	}
	for (i = 0; i < 10; i++) {
		delay_for_interval(5, NSEC_PER_MSEC);
		if (rd(R_BASIC_HALO_HEARTBEAT, &v) && v > hb0) {
			break;
		}
	}
	if (i == 10) {
		give_up("the ROM has no heartbeat");
		return false;
	}
	if (!ack(R_BASIC_SHUTDOWNREQUEST, 1, 0)) {
		return false;
	}
	if (!rd(R_BASIC_STATEMACHINE, &v) || v != BASIC_SHUTDOWN) {
		give_up("the ROM is not shut down");
		return false;
	}
	if (!rd(R_BASIC_AMP_STATUS, &v) || (v & BASIC_ERRORS)) {
		/* Samsung releases amp and temperature errors here (hw_err_rls); this gives up */
		hsay("haptic: cs40l25: the ROM reports errors %#x\n", v);
		give_up("the ROM reports an error");
		return false;
	}
	/* cs40l2x_basic_mode_revert (cs40l2x.c:8652) */
	return wr(R_PWR_CTRL1, 0) && wr(R_PWR_CTRL2, 0x00003321) && wr(R_LRCK_PAD_CONTROL, 7) && wr(R_SDIN_PAD_CONTROL, 7) &&
	       wr(R_AMP_DIG_VOL_CTRL, 0x00008000) && wr(R_IRQ2_MASK1, 0xFFFFFFFF) && wr(R_IRQ2_MASK2, 0xFFFFFFFF);
}

/* cs40l2x_rev_b0_errata (cs40l2x.c:8782), applied to L25A B1 as well (:8901-8917), and put into the sequence */
static const struct {
	uint32_t        reg, val;
	bool            once;   /* not read back: test key, resync triggers */
} errata[] = {
	{ R_PLL_LOOP_PARAM, 0x000C1837, false },
	{ R_PLL_MISC_CTRL, 0x03008E0E, false },
	{ R_TEST_KEY_CTL, 0x55, true },
	{ R_TEST_KEY_CTL, 0xAA, true },
	{ R_TEST_LBST, 0x004DC080, false },             /* EXPL_MODE_EN */
	{ R_OTP_TRIM_12, 0x002F0065, false },
	{ R_OTP_TRIM_13, 0x00002B4F, false },
	{ R_SPKMON_RESYNC, 0, true },
	{ R_TEMP_RESYNC, 0, true },
	{ R_TEST_KEY_CTL, 0xCC, true },
	{ R_TEST_KEY_CTL, 0x33, true },
	{ R_VPVBST_FS_SEL, 0, false },
};

static bool
apply_errata(void)
{
	step = "errata";
	for (unsigned i = 0; i < sizeof(errata) / sizeof(errata[0]); i++) {
		if (!(errata[i].once ? wr_once(errata[i].reg, errata[i].val) : wr(errata[i].reg, errata[i].val)) ||
		    !wseq_add(errata[i].reg, errata[i].val)) {
			return false;
		}
	}
	return true;
}

/*
 * cs40l2x.c:7666 boost_config with the dtbo's values: 1000 nH (lbst 0), 4 uF (range 0): K1 0x24, K2 0x24,
 * slope 0x75 (cs40l2x-tables.c:16-30); 3100 mA: (3100 - 1600) / 50 + 0x10 = 0x2E; VBST limit 11000 mV
 * (Samsung's default): (11000 - 2550) / 50 + 1 = 0xAA; no OVP threshold in the dt. Then boost_short_test
 * (:7610): the target at the CP value, the device on for 10 ms, no boost short, off, class H again.
 */
static bool
configure_boost(void)
{
	uint32_t st = 0;

	step = "the boost converter";
	if (!upd(R_BSTCVRT_COEFF, 0x0000FFFF, 0x2424) || !wseq_add(R_BSTCVRT_COEFF, 0x2424) ||
	    !upd(R_BSTCVRT_SLOPE_LBST, 0x0000FF03, 0x7500) || !wseq_add(R_BSTCVRT_SLOPE_LBST, 0x7500) ||
	    !upd(R_BSTCVRT_PEAK_CUR, 0x7F, 0x2E) || !wseq_add(R_BSTCVRT_PEAK_CUR, 0x2E) ||
	    !upd(R_BSTCVRT_VCTRL1, 0xFF, 0xAA) || !wseq_add(R_BSTCVRT_VCTRL1, 0xAA) ||
	    !upd(R_BSTCVRT_VCTRL2, 0x4, 0x4) || !wseq_add(R_BSTCVRT_VCTRL2, 0x4 | 0x1)) {
		return false;
	}
	step = "the boost converter's short-circuit test";
	if (!upd(R_BSTCVRT_VCTRL2, 0x3, 0x0) || !upd(R_PWR_CTRL1, 0x1, 0x1)) {
		return false;
	}
	delay_for_interval(10, NSEC_PER_MSEC);
	if (!rd(R_IRQ1_STATUS1, &st)) {
		give_up("the boost status cannot be read");
		return false;
	}
	if (!upd(R_PWR_CTRL1, 0x1, 0x0)) {
		return false;
	}
	if (st & ERR_BST_SHORT) {
		give_up("THE BOOST CONVERTER IS SHORTED");
		return false;
	}
	if (!upd(R_BSTCVRT_VCTRL2, 0x3, 0x1)) {
		return false;
	}
	wseq_replace(R_TEST_LBST, 0x014DC080);          /* EXPL_MODE_DIS, in the sequence only (as Samsung) */
	return true;
}

/*
 * cs40l2x.c:8157 cs40l2x_init for B1 with this dt: no REFCLK (the B1 ROM's), the boost, the PCM routing
 * (:8149: DAC <- DSP1TX1 0x32; DSP RX1..4 <- ASPRX1 0x08, VMON 0x18, IMON 0x19, VPMON 0x28), the volume's
 * high-pass in the sequence, the wake sources (SDA and GPIO1, both active high: 0x98); no ASP (no
 * asp-* in the dt), no brownout (no vpbr/vbbr in the dt).
 */
static bool
configure(void)
{
	static const uint32_t routing[5][2] = {
		{ R_DAC_PCM1_SRC, 0x32 }, { R_DSP1_RX1_SRC, 0x08 }, { R_DSP1_RX2_SRC, 0x18 }, { R_DSP1_RX3_SRC, 0x19 },
		{ R_DSP1_RX4_SRC, 0x28 },
	};

	if (!rom_exit() || !apply_errata() || !configure_boost()) {
		return false;
	}
	step = "routing";
	for (unsigned i = 0; i < 5; i++) {
		if (!wr(routing[i][0], routing[i][1]) || !wseq_add(routing[i][0], routing[i][1])) {
			return false;
		}
	}
	return wseq_add(R_AMP_DIG_VOL_CTRL, 0x8000) && wr(R_WAKESRC_CTL, 0x98) && wseq_add(R_WAKESRC_CTL, 0x98);
}

/* ---- the firmware ----------------------------------------------------------------------------------------- */

static uint32_t
control(const char *name, uint32_t type, uint32_t algo)
{
	for (unsigned i = 0; i < ncontrols; i++) {
		if (controls[i].algo == algo && controls[i].type == type && strncmp(controls[i].name, name, 32) == 0) {
			return controls[i].reg;
		}
	}
	return 0;
}

/* an ALGO_INFO block (cs40l2x.c:7178 algo_parse): the algorithm's controls, by name */
static bool
parse_algo(const uint8_t *p, uint32_t len)
{
	uint32_t pos = 0, id, count;

	if (len < 12) {
		return false;
	}
	id = le32(p);
	pos = 4;
	pos += (p[pos] / 4) * 4 + 4;                    /* the name */
	if (pos + 2 > len) {
		return false;
	}
	pos += (le16(p + pos) / 4) * 4 + 4;             /* the description */
	if (pos + 4 > len) {
		return false;
	}
	count = le32(p + pos);
	pos += 4;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t off, type, length, nl;

		if (pos + 9 > len) {
			return false;
		}
		off = le16(p + pos);
		type = le16(p + pos + 2);
		length = le32(p + pos + 4);
		pos += 8;
		nl = p[pos];
		if (pos + length > len || nl + 1 > length || nl >= 32) {
			return false;
		}
		if (ncontrols < MAX_CONTROLS) {
			controls[ncontrols].algo = id;
			controls[ncontrols].type = type;
			controls[ncontrols].offset = off;
			memcpy(controls[ncontrols].name, p + pos + 1, nl);
			controls[ncontrols].name[nl] = 0;
			ncontrols++;
		}
		pos += length;
	}
	return true;
}

/* cs40l2x.c:7246 firmware_parse: the program into PM, XM and YM, and the algorithms' controls */
static bool
load_wmfw(const uint8_t *d, uint64_t size)
{
	uint32_t pos;

	step = "cs40l25a.wmfw";
	if (size < WMFW_HEADER || memcmp(d, "WMFW", 4) != 0 || (size % 4) != 0 || le32(d + 4) != WMFW_HEADER) {
		give_up("cs40l25a.wmfw is not a WMFW file");
		return false;
	}
	for (pos = WMFW_HEADER; pos + 8 <= size; ) {
		uint32_t off = le24(d + pos), type = d[pos + 3], len = le32(d + pos + 4), reg = 0;
		const uint8_t *data = d + pos + 8;

		if (pos + 8 + len > size) {
			give_up("cs40l25a.wmfw: a block runs past the end");
			return false;
		}
		switch (type) {
		case T_WMFW_INFO:
			break;
		case T_PM_PACKED:
			reg = R_DSP1_PMEM_0 + off * 5;
			break;
		case T_XM_PACKED:
			reg = R_DSP1_XMEM_PACK_0 + off * 3;
			break;
		case T_YM_PACKED:
			reg = R_DSP1_YMEM_PACK_0 + off * 3;
			break;
		case T_ALGO_INFO:
			if (!parse_algo(data, len)) {
				give_up("cs40l25a.wmfw: an algorithm block does not parse");
				return false;
			}
			break;
		default:
			hsay("haptic: cs40l25: cs40l25a.wmfw: block type %#x\n", type);
			give_up("cs40l25a.wmfw: an unknown block");
			return false;
		}
		if (reg != 0 && ((reg & 3) != 0 || (len & 3) != 0 || !wr_raw(reg, data, len))) {
			if (!broken) {
				give_up("cs40l25a.wmfw: a block not on a word");
			}
			return false;
		}
		pos += 8 + ((len + 3) & ~3U);
	}
	return true;
}

/* cs40l2x.c:6029 coeff_init: the algorithm list the program left in XM, and the controls' addresses */
static bool
read_algorithms(void)
{
	uint32_t reg = R_XM_FW_ID, n = 0, term = 0;

	step = "the algorithm list";
	if (!rd(R_XM_NUM_ALGOS, &n) || n + 1 > MAX_ALGOS) {
		give_up("the number of algorithms");
		return false;
	}
	nalgos = n + 1;
	for (unsigned i = 0; i < nalgos; i++) {
		if (!rd(reg, &algos[i].id) || !rd(reg + 4, &algos[i].rev) || !rd(reg + 8, &algos[i].xm_base) ||
		    !rd(reg + 12, &algos[i].xm_size) || !rd(reg + 16, &algos[i].ym_base) || !rd(reg + 20, &algos[i].ym_size)) {
			give_up("an algorithm's entry");
			return false;
		}
		for (unsigned c = 0; c < ncontrols; c++) {
			if (controls[c].algo != algos[i].id) {
				continue;
			}
			if (controls[c].type == T_XM_UNPACKED) {
				controls[c].reg = R_DSP1_XMEM_UNPACK24_0 + algos[i].xm_base * 4 + controls[c].offset * 4;
			} else if (controls[c].type == T_YM_UNPACKED) {
				controls[c].reg = R_DSP1_YMEM_UNPACK24_0 + algos[i].ym_base * 4 + controls[c].offset * 4;
			}
		}
		reg += 24 + (i == 0 ? 4 : 0);           /* the system algorithm has one more word (the count) */
	}
	if (!rd(reg, &term) || term != ALGO_LIST_TERM || algos[0].id != FW_ID_REMAP || algos[0].rev < FW_MIN_REV) {
		hsay("haptic: cs40l25: algorithm list: end %#x, firmware %#x revision %#x\n", term, algos[0].id, algos[0].rev);
		give_up("not the firmware Samsung's driver expects");
		return false;
	}
	hsay("haptic: cs40l25: firmware %#x revision %u.%u.%u, %u algorithms, %u controls\n", algos[0].id,
	    (algos[0].rev >> 16) & 0xff, (algos[0].rev >> 8) & 0xff, algos[0].rev & 0xff, nalgos, ncontrols);
	return true;
}

/* cs40l2x.c:6873 coeff_file_parse: the wavetable (cs40l20.bin: one XM block, one YM block of VIBEGEN) */
static bool
load_wmdr(const char *name, const uint8_t *d, uint64_t size)
{
	uint32_t pos, wt_xm = control("WAVETABLE", T_XM_UNPACKED, ALGO_ID_VIBE);
	unsigned pre = 0, a2h = 0;

	step = name;
	if (size < WT_HEADER || memcmp(d, "WMDR", 4) != 0 || (size % 4) != 0) {
		give_up("not a WMDR file");
		return false;
	}
	for (pos = le32(d + 4); pos + 20 <= size; ) {
		uint32_t off = le16(d + pos), type = le16(d + pos + 2), algo = le32(d + pos + 4), rev = le32(d + pos + 8);
		uint32_t len = le32(d + pos + 16), reg = 0;
		unsigned i;

		if (pos + 20 + len > size) {
			give_up("a block runs past the end");
			return false;
		}
		if (type != T_WMDR_NAME && type != T_WMDR_INFO) {
			for (i = 0; i < nalgos && algos[i].id != algo; i++) {
			}
			if (i == nalgos || ((rev >> 8) & ALGO_REV_MASK) != (algos[i].rev & ALGO_REV_MASK)) {
				hsay("haptic: cs40l25: %s: algorithm %#x revision %#x is not the firmware's\n", name, algo, rev);
				give_up("a block for another firmware");
				return false;
			}
			if (type == T_XM_UNPACKED) {
				reg = R_DSP1_XMEM_UNPACK24_0 + off + algos[i].xm_base * 4;      /* (off in bytes here) */
				/* Samsung's wt_limit_xm: (xm_size - WAVETABLE's word offset) x 4; the offset is 5 in this wmfw */
				if (reg == wt_xm && len > (algos[i].xm_size - 5) * 4) {
					give_up("the wavetable is too large for XM");
					return false;
				}
			} else if (type == T_YM_UNPACKED) {
				reg = R_DSP1_YMEM_UNPACK24_0 + off + algos[i].ym_base * 4;
			} else {
				give_up("an unknown block");
				return false;
			}
			/* PRE_FILTER (0x109) and A2H (0x110) blocks are levels: Samsung writes only the first of each */
			if ((algo == 0x109 && pre++ > 0) || (algo == 0x110 && a2h++ > 0)) {
				reg = 0;
			}
		} else if (len >= 16 && memcmp(d + pos + 20, "Date: ", 6) == 0) {
			hsay("haptic: cs40l25: %s: %.10s\n", name, (const char *)(d + pos + 26));
		}
		if (reg != 0 && !wr_raw(reg, d + pos + 20, len)) {
			return false;
		}
		pos += 20 + ((len + 3) & ~3U);
	}
	return true;
}

/* cs40l2x.c:6302 dsp_pre_config, for REMAP with this dt (but events off: there is no interrupt handler here) */
static bool
dsp_pre_config(void)
{
	uint32_t r, seq, base = control("POWERONSEQUENCE", T_XM_UNPACKED, FW_ID_REMAP);
	static const uint32_t irq2_masks[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF87FF, 0xFEFFFFFF };      /* :6280 */

	step = "the DSP's configuration";
	if ((r = control("GPIO_BUTTONDETECT", T_XM_UNPACKED, FW_ID_REMAP)) == 0 || !wr(r, 0x1)) {  /* GPIO1 only (L25A) */
		return broken ? false : (give_up("no GPIO_BUTTONDETECT"), false);
	}
	if ((r = control("GPIO_POL", T_XM_UNPACKED, FW_ID_REMAP)) != 0 && !wr(r, 0)) {
		return false;
	}
	if ((r = control("GPIO_ENABLE", T_XM_UNPACKED, FW_ID_REMAP)) != 0 && !wr(r, 0)) {          /* gpio1-mode 1 */
		return false;
	}
	if ((r = control("SPK_FORCE_TST_1_AUTO", T_XM_UNPACKED, FW_ID_REMAP)) != 0 && !wr(r, 0x00D02800)) {   /* amp-gnd-stby */
		return false;
	}
	/* wseq_init (:940): each entry as two words, then the end mark */
	if (base != 0) {
		for (unsigned i = 0; i < nwseq; i++) {
			if (!wr(base + i * 8, ((wseq[i].reg & 0xFFFF) << 8) | ((wseq[i].val & 0xFF000000) >> 24)) ||
			    !wr(base + i * 8 + 4, wseq[i].val & 0x00FFFFFF)) {
				return false;
			}
		}
		if (!wr(base + nwseq * 8, 0x00FFFFFF)) {
			return false;
		}
	}
	/* Samsung: HARDWARE | END events (0x800020) because it has the IRQ; with none, Samsung writes 0 too */
	if ((r = control("EVENTCONTROL", T_XM_UNPACKED, FW_ID_REMAP)) == 0 || !wr(r, 0)) {
		return broken ? false : (give_up("no EVENTCONTROL"), false);
	}
	if ((seq = control("IRQMASKSEQUENCE", T_XM_UNPACKED, FW_ID_REMAP)) != 0) {
		for (unsigned i = 0; i < 4; i++) {
			uint32_t v = i == 0 ? irq2_masks[0] & ~HW_ERRORS : irq2_masks[i];

			if (!wr(seq + i * 8, (v & 0xFF) << 16) || !wr(seq + i * 8 + 4, (v & 0xFFFFFF00) >> 8)) {
				return false;
			}
		}
		if ((r = control("IRQMASKSEQUENCE_VALID", T_XM_UNPACKED, FW_ID_REMAP)) == 0 || !wr(r, 1)) {
			return broken ? false : (give_up("no IRQMASKSEQUENCE_VALID"), false);
		}
	}
	return true;
}

/* cs40l2x.c:6472 dsp_start */
static bool
dsp_start(void)
{
	uint32_t state = 0, scratch = 1, halo = control("HALO_STATE", T_XM_UNPACKED, FW_ID_REMAP);
	int i;

	step = "the DSP's start";
	if (halo == 0 || !wr(R_DSP1_XM_ACCEL_PL0_PRI, 0) || !wr(R_DSP1_YM_ACCEL_PL0_PRI, 0)) {
		return broken ? false : (give_up("no HALO_STATE"), false);
	}
	for (i = 0; i < 8; i++) {                       /* cs40l2x_dsp_errata (:6451): every RX and TX at rate 1 */
		if (!wr(R_DSP1_RX1_RATE + i * 8, 1) || !wr(R_DSP1_TX1_RATE + i * 8, 1)) {
			return false;
		}
	}
	/*
	 * CCM_CORE_CTRL: RESET (bit 9) clears itself once the core is reset, so the write is not read back whole
	 * (2026-11-06: 0x301 written, 0x101 read, and the strict check stopped the start); EN (bit 0) must stay.
	 */
	{
		uint32_t v = 0;

		if (!upd(R_PWRMGT_CTL, 0x2, 0x2) || !rd(R_DSP1_CCM_CORE_CTRL, &v) || !wr_once(R_DSP1_CCM_CORE_CTRL, v | 0x201) ||
		    !rd(R_DSP1_CCM_CORE_CTRL, &v) || !(v & 0x1)) {
			hsay("haptic: cs40l25: the DSP core is not enabled (CCM_CORE_CTRL %#x)\n", v);
			give_up("the DSP core did not start");
			return false;
		}
	}
	for (i = 0; i < 10; i++) {
		delay_for_interval(10, NSEC_PER_MSEC);
		if (rd(halo, &state) && state == FW_HALO_STATE_RUN) {
			break;
		}
	}
	if (!rd(R_DSP1_SCRATCH1, &scratch) || i == 10 || scratch != 0) {
		hsay("haptic: cs40l25: DSP state %u (running: %u), scratch %u\n", state, FW_HALO_STATE_RUN, scratch);
		give_up("the DSP did not start");
		return false;
	}
	return true;
}

static bool hw_errors(const char *when);

/* cs40l2x.c:6551 dsp_post_config with this dt (no f0/ReDC/Q defaults, no GPIO edge indexes) */
static bool
dsp_post_config(void)
{
	uint32_t r;

	step = "the DSP's post-configuration";
	if ((r = control("TIMEOUT_MS", T_XM_UNPACKED, ALGO_ID_VIBE)) == 0 || !wr(r, max_ms + 50)) {    /* Samsung: 0x2AAAA */
		return broken ? false : (give_up("no TIMEOUT_MS"), false);
	}
	if ((r = control("NUMBEROFWAVES", T_XM_UNPACKED, ALGO_ID_VIBE)) == 0 || !rd(r, &num_waves) || num_waves == 0) {
		return broken ? false : (give_up("the wavetable is empty"), false);
	}
	/* imon_offs_sync (:4480): no CLAB algorithm in this firmware, so the IMON offset calculation off */
	if ((r = control("IMON_OFFSET_CALC_ENABLE", T_XM_UNPACKED, FW_ID_REMAP)) != 0 && !wr(r, 0)) {
		return false;
	}
	/* comp_enable, redc and f0 all on (no cirrus,comp-disable): (1 << 1) | (1 << 0) */
	if ((r = control("COMPENSATION_ENABLE", T_XM_UNPACKED, ALGO_ID_VIBE)) == 0 || !wr(r, 0x3)) {
		return broken ? false : (give_up("no COMPENSATION_ENABLE"), false);
	}
	gain_reg = control("GAIN_CONTROL", T_XM_UNPACKED, FW_ID_REMAP);
	endplayback_reg = control("ENDPLAYBACK", T_XM_UNPACKED, FW_ID_REMAP);
	if (gain_reg == 0 || endplayback_reg == 0) {
		give_up("no GAIN_CONTROL or ENDPLAYBACK");
		return false;
	}
	hsay("haptic: cs40l25: %u waveforms (Samsung's log: 143), the DSP runs; limits %u ms, %u %%\n", num_waves, max_ms, max_pct);
	return hw_errors("after the start");
}

static bool
load_firmware(void)
{
	uint64_t size = 0, wsize = 0;
	const uint8_t *fw = redwood_image_file("fw", "cs40l25a.wmfw", &size, NULL);
	const uint8_t *wt = redwood_image_file("fw", "cs40l20.bin", &wsize, NULL);
	uint64_t t0 = mach_absolute_time(), ns;

	if (fw == NULL || wt == NULL) {
		hsay("haptic: cs40l25: fw/cs40l25a.wmfw %s, fw/cs40l20.bin %s (../fw-haptic, userland/build.sh)\n",
		    fw ? "found" : "MISSING", wt ? "found" : "MISSING");
		give_up("the firmware is not in the image");
		return false;
	}
	/* cs40l25a_exc.bin (the other coefficient file of REMAP) holds only a name block: nothing to load */
	if (!load_wmfw(fw, size) || !read_algorithms() || !load_wmdr("cs40l20.bin", wt, wsize) || !dsp_pre_config() ||
	    !dsp_start() || !dsp_post_config()) {
		return false;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	hsay("haptic: cs40l25: %llu + %llu bytes of firmware loaded in %llu ms (bit-banged)\n", size, wsize, ns / NSEC_PER_MSEC);
	return true;
}

/* ---- health ------------------------------------------------------------------------------------------------- */

/* IRQ2_STATUS1's hardware errors (hw_err_chk, :6239), and the known register (check_recovery, :5447) */
static bool
hw_errors(const char *when)
{
	uint32_t st = 0, rx2 = 0;

	if (broken) {
		return false;
	}
	if (!rd(R_IRQ2_STATUS1, &st) || !rd(R_DSP1_RX2_SRC, &rx2)) {
		give_up("the chip does not answer");
		return false;
	}
	if (st & HW_ERRORS) {
		hsay("haptic: cs40l25: %s: HARDWARE ERROR %#x (amp %u, temp %u, temp rise %u, boost short %u, uv %u, ov %u)\n",
		    when, st, !!(st & ERR_AMP), !!(st & ERR_TEMP), !!(st & ERR_TEMP_RISE), !!(st & ERR_BST_SHORT),
		    !!(st & ERR_BST_UVP), !!(st & ERR_BST_OVP));
		give_up("a hardware error");
		return false;
	}
	if (rx2 != 0x18) {
		give_up("the chip lost its configuration (it reset by itself?)");
		return false;
	}
	return true;
}

/* ---- playing ------------------------------------------------------------------------------------------------ */

/*
 * Samsung's intensity (cs40l2x-tables.c:862 cs40l2x_pbq_dig_scale, used by intensity_store, cs40l2x.c:3479):
 * the attenuation for 0..100 % in 1/8 dB, 20 log10(1 / share) x 8. It goes into GAIN_CONTROL bits 13:4
 * (the control port's triggers; cp_dig_scale_set, :3335).
 */
static const uint16_t attenuation[101] = {
	0, 320, 272, 244, 224, 208, 195, 185, 176, 167, 160, 153, 147, 142, 137, 132, 127, 123, 119, 115, 112, 108, 105, 102,
	99, 96, 94, 91, 88, 86, 84, 81, 79, 77, 75, 73, 71, 69, 67, 65, 64, 62, 60, 59, 57, 55, 54, 52, 51, 50, 48, 47, 45, 44,
	43, 42, 40, 39, 38, 37, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 16, 15, 14, 13,
	12, 11, 10, 10, 9, 8, 7, 7, 6, 5, 4, 4, 3, 2, 1, 1, 0,
};
static uint32_t gain_pct;

static bool
set_strength(uint32_t pct)
{
	pct = pct < 1 ? 1 : pct > max_pct ? max_pct : pct;
	if (pct == gain_pct) {
		return true;
	}
	if (!upd(gain_reg, 0x3FF0, (uint32_t)attenuation[pct] << 4)) {
		return false;
	}
	gain_pct = pct;
	return true;
}

/* waveform 0 until stopped (Samsung's timed vibration: TRIGGER_MS, :5650) */
static bool
start_buzz(uint32_t pct)
{
	return set_strength(pct) && ack(R_MBOX_TRIGGER_MS, 0, MBOX_TRIGGERRESET);
}

/* the wavetable's entry `index' once (Samsung's clicks: TRIGGERINDEX, :5659) */
static bool
play_click(uint32_t index, uint32_t pct)
{
	if (index == 0 || index >= num_waves) {
		hsay("haptic: cs40l25: no waveform %u (1..%u)\n", index, num_waves - 1);
		return false;
	}
	return set_strength(pct) && ack(R_MBOX_TRIGGERINDEX, index, MBOX_TRIGGERRESET);
}

/* stop_playback (:4984): ENDPLAYBACK, 10 tries 10 ms apart, then reset (as Samsung: "Parking device in reset") */
static bool
stop(void)
{
	for (int i = 0; i < 10 && !broken; i++) {
		uint8_t b[8];

		put_be32(b, endplayback_reg);
		put_be32(b + 4, 1);
		if (redwood_bbi2c_xfer(&bus, CS_ADDR, b, 8, NULL, 0)) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	give_up("it does not take ENDPLAYBACK");
	return false;
}

static void
one_effect(void)
{
	uint32_t effect = 0;

	PE_parse_boot_argn("haptic_effect", &effect, sizeof(effect));
	step = "one effect";
	if (effect == 0) {
		hsay("haptic: cs40l25: waveform 0 for 80 ms at %u %% (Samsung's recovery: 80 ms at 100 %%)\n", max_pct);
		if (start_buzz(max_pct)) {
			delay_for_interval(80, NSEC_PER_MSEC);
			(void)stop();
		}
	} else {
		hsay("haptic: cs40l25: click %u at %u %%\n", effect, max_pct);
		(void)play_click(effect, max_pct);
		delay_for_interval(200, NSEC_PER_MSEC);
	}
	(void)hw_errors("after the effect");
}

/* ---- haptic=5: the motor for iOS (the contract of bsd/dev/redwood_haptic.c) --------------------------------- */

#define ACTIVE_US               30000           /* a write that was not silence counts for this long */
#define SILENT_BELOW            256             /* of 32768: samples no larger are silence */
#define TAP_US                  60000           /* longer than this, and it is a vibration, not a tap */
#define SETTLE_US               8000            /* how long iOS's waveform is listened to before a tap: its loudness */
#define FOLLOW_US               20000           /* how often a vibration's strength follows iOS's */
#define TOLD_EVENTS             24

static volatile uint64_t heard_us, quiet_us, onset_us;
static volatile uint32_t heard_peak;
static uint64_t written_frames;
static unsigned taps, loops, capped;

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

static void
heard(uint32_t peak)
{
	uint64_t now = now_us();

	if (peak <= SILENT_BELOW) {
		quiet_us = now;
		return;
	}
	bool onset = heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US;

	if (onset) {
		onset_us = now;
		heard_peak = 0;
	}
	heard_us = now;
	if (onset) {
		thread_wakeup((event_t)(uintptr_t)&heard_us);       /* the service loop sleeps while idle (cs_thread) */
	}
	if (peak > heard_peak) {
		heard_peak = peak;
	}
}

/* From the audio server's I/O thread: never waits. */
static int
cs_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		bytes = bytes > (user_ssize_t)sizeof(samples) ? (user_ssize_t)sizeof(samples) : bytes;
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		heard(peak);
	}
	return error;
}

static int
cs_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw cs_cdevsw = {
	.d_open = cs_open, .d_close = cs_open, .d_read = eno_rdwrt, .d_write = cs_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* as redwood_haptic.c's gain_for(): the cap from a third of full scale up, down to five eighths of it */
static uint32_t
pct_for(uint32_t peak)
{
	uint32_t top = max_pct, low = max_pct * 5 / 8, third = 32767 / 3;

	return peak >= third ? top : low + (top - low) * peak / third;
}

static void
serve(void)
{
	enum { IDLE, TAP, LOOP, REST } state = IDLE;
	uint64_t since = 0, rest_until = 0, last_look = 0, tap_onset = 0, followed = 0;
	int major = cdevsw_add(-1, &cs_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-haptic") == NULL) {
		hsay("haptic: cs40l25: /dev/redwood-haptic cannot be made\n");
		return;
	}
	hsay("haptic: cs40l25: THE MOTOR PLAYS WHAT IOS PLAYS ON ITS ACTUATOR (/dev/redwood-haptic): at most %u ms, %u %%\n",
	    max_ms, max_pct);
	step = "serving iOS";
	while (!broken) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;
		uint32_t peak = heard_peak, pct = pct_for(peak);
		/* No battery-temperature cutoff (user, 2026-11-21: haptics stay on when the phone is warm; torch and camera keep
		 * theirs). The chip's own over-temperature and temperature-rise faults still stop it (IRQ2_STATUS1 below). */

		switch (state) {
		case IDLE:
		case TAP:
			if (active && onset_us != tap_onset && now - onset_us >= SETTLE_US) {
				tap_onset = onset_us;
				if (state == TAP && tap_index == 0 && !stop()) {
					break;
				}
				if (!(tap_index == 0 ? start_buzz(pct) : play_click(tap_index, pct))) {
					break;
				}
				state = TAP;
				since = now;
				if (++taps + loops <= TOLD_EVENTS) {
					hsay("haptic: cs40l25: tap %u: iOS peaks at %u of 32768: %s %u at %u %%\n", taps, peak,
					    tap_index ? "click" : "waveform 0 for 30 ms,", tap_index, pct);
				}
				break;
			}
			if (state == IDLE) {
				break;
			}
			if (tap_index == 0 && !active && now - since >= TAP_MS * 1000ull) {
				state = stop() ? IDLE : state;
			} else if (active && quiet_us < tap_onset && now - since > TAP_US) {
				/* iOS goes on without a pause: a vibration */
				if ((tap_index != 0 || stop()) && start_buzz(pct)) {
					state = LOOP;
					since = followed = now;
					heard_peak = 0;
					if (taps + ++loops <= TOLD_EVENTS) {
						hsay("haptic: cs40l25: vibration %u (peak %u): waveform 0 at %u %%, %u ms at most\n", loops,
						    peak, pct, max_ms);
					}
				}
			} else if (!active && now - since > TAP_US) {
				state = (tap_index != 0 || stop()) ? IDLE : state;
			}
			break;
		case LOOP:
			if (active && now - followed >= FOLLOW_US) {
				followed = now;
				heard_peak = 0;
				if (peak != 0 && (pct > gain_pct + 5 || pct + 5 < gain_pct) && !set_strength(pct)) {
					break;
				}
			}
			if (!active || now - since > max_ms * 1000ull) {
				bool cut = now - since > max_ms * 1000ull;

				if (cut) {
					capped++;
					rest_until = now + max_ms * 1000ull;
				}
				if (stop()) {
					state = rest_until > now ? REST : IDLE;
					if (taps + loops <= TOLD_EVENTS + 1) {
						hsay("haptic: cs40l25: vibration %u: %s after %llu ms\n", loops,
						    cut ? "CUT at the limit" : "iOS stopped, and so does the motor", (now - since) / 1000);
					}
				}
			}
			break;
		case REST:
			state = now >= rest_until ? IDLE : REST;
			break;
		}
		if (state != IDLE && now - last_look > 50000) {
			last_look = now;
			if (!hw_errors("while playing")) {
				break;
			}
		}
		if (state == IDLE && !active) {
			/* (A, 2026-11-14) idle: asleep until iOS plays something (heard() wakes it), at most 100 ms --
			 * the 4 ms look was 250 wake-ups a second of a core with the motor still */
			assert_wait_timeout((event_t)(uintptr_t)&heard_us, THREAD_UNINT, 100, NSEC_PER_MSEC);
			if (heard_us != 0 && now_us() - heard_us < ACTIVE_US) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
		} else {
			delay_for_interval(4, NSEC_PER_MSEC);
		}
	}
	hsay("haptic: cs40l25: given up on (%u taps, %u vibrations, %u cut at %u ms)\n", taps, loops, capped, max_ms);
}

/* ---- the thread ------------------------------------------------------------------------------------------------ */

static void
cs_thread(__unused void *parameter, __unused wait_result_t result)
{
	char line[192];
	uint32_t devid = 0, revid = 0;

	reset_bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, BANK_GPP4);
	alert_bank = redwood_gpio_bank(REDWOOD_PINCTRL_CMGP, BANK_GPM27);
	bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, BANK_GPP1);
	hsay("haptic: cs40l25: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ?
	    "out of reset, registers read" : level == 3 ? "configured and the firmware loaded, nothing played" :
	    level == 4 ? "one effect played" : "the motor for iOS");
	show_pins();
	/* the board's pull-ups only (hsi2c5-bus-vib: pin-pud 0); see NOTES.md for why not the SoC's */
	if (!redwood_bbi2c_setup(&bus, false, line, sizeof(line))) {
		hsay("haptic: cs40l25: %s: the bus is not usable\n", line);
		redwood_bbi2c_restore(&bus);
		return;
	}
	bus_taken = true;
	hsay("haptic: cs40l25: %s\n", line);
	if (reset_is_high()) {
		rom_alive = look("as the bootloader left it", &devid, &revid, rom_regs);
	} else {
		hsay("haptic: cs40l25: the reset pin does not drive high: the chip is held in reset (nothing read)\n");
	}
	if (level >= 2 && rom_test) {
		rom_buzz();
	}
	/*
	 * Up to three bring-ups (x1slte, 2026-11-06): one run served iOS, a later boot left no /dev/redwood-haptic. A bring-up
	 * that gives up (the chip in reset) is started again from the reset, with the bus set up afresh.
	 */
	for (haptic_attempts = 1; level >= 2 && haptic_attempts <= 3; haptic_attempts++) {
		if (haptic_attempts > 1) {
			char again[192];

			delay_for_interval(500, NSEC_PER_MSEC);
			broken = false;
			nwseq = 0;
			ncontrols = 0;
			nalgos = 0;
			step = "start";
			if (!redwood_bbi2c_setup(&bus, false, again, sizeof(again))) {
				break;
			}
			bus_taken = true;
			hsay("haptic: cs40l25: attempt %u, from the reset\n", haptic_attempts);
		}
		if (come_out_of_reset() && level >= 3 && configure() && load_firmware()) {
			snprintf(haptic_state, sizeof(haptic_state), "%s (attempt %u, %u block retries, %u transfer retries)",
			    level == 5 ? "serving iOS" : "firmware loaded", haptic_attempts, block_retries, xfer_retries);
			if (level == 4) {
				one_effect();
			} else if (level == 5) {
				serve();
				if (broken && haptic_attempts < 3) {
					continue;       /* serving stopped on a fault: the bring-up again, from the reset */
				}
			}
			break;
		}
		if (!broken || level < 3) {
			break;
		}
	}
	if (!broken && level >= 3) {
		hsay("haptic: cs40l25: done; the chip stays out of reset (standby) until the run ends\n");
	} else if (!broken && bus_taken) {
		redwood_bbi2c_restore(&bus);
		bus_taken = false;
		hsay("haptic: cs40l25: done\n");
	}
}

/* from redwood_haptic_init() with haptic=N haptic_chip=cs40l25 */
void redwood_cs40l25_init(uint32_t lvl);

void
redwood_cs40l25_init(uint32_t lvl)
{
	thread_t thread;

	level = lvl > 5 ? 5 : lvl;
	PE_parse_boot_argn("haptic_max_ms", &max_ms, sizeof(max_ms));
	PE_parse_boot_argn("haptic_max_pct", &max_pct, sizeof(max_pct));
	PE_parse_boot_argn("haptic_tap", &tap_index, sizeof(tap_index));
	PE_parse_boot_argn("haptic_rom", &rom_test, sizeof(rom_test));
	PE_parse_boot_argn("haptic_verify", &verify, sizeof(verify));
	max_ms = max_ms < 100 ? 100 : max_ms > 5000 ? 5000 : max_ms;
	max_pct = max_pct < 1 ? 1 : max_pct > 100 ? 100 : max_pct;
	if (kernel_thread_start(cs_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* the run's end, a panic's way out: the reset pin low, nothing on the bus */
void redwood_cs40l25_quiesce(void);

void
redwood_cs40l25_quiesce(void)
{
	if (released && reset_bank != NULL) {
		reset_pin(false);
		hsay("haptic: cs40l25: at the run's end: %u taps, %u vibrations (%u cut), %llu frames from iOS; the chip is "
		    "held in reset\n", taps, loops, capped, written_frames);
	}
}

/* redwood_mic.c's mic_tap=1 stand-in for iOS (redwood_haptic_pretend()) */
void redwood_cs40l25_pretend(uint32_t peak);

void
redwood_cs40l25_pretend(uint32_t peak)
{
	heard(peak);
}

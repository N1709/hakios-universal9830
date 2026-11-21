/*
 * The Galaxy S20's (x1slte, SM-G980F) rear light sensor: the ams TCS3407 ("als_rear", Samsung's flicker sensor for the
 * cameras) at I2C 0x29 on HSI2C_25, bit-banged here (bsd/dev/redwood_bbi2c.c). Its lux goes into /dev/redwood-sensors
 * (sys/redwood_sensors.h, bsd/dev/redwood_sensors.c) as a LIGHT sample marked REDWOOD_LIGHT_REAR.
 *
 * NOTE: this is the REAR sensor. The light sensor Android's auto-brightness uses is the front one, the TMD4907 under the
 * screen, behind the sensor hub (bsd/dev/redwood_ssp.c, sensor 9: CONFIG_SENSORS_SSP_TMD4907). backboardd's side
 * (userland/src/inject_sensors.c) takes the front one when the hub gives it, and the rear one only until then.
 *
 * Facts (NOTES.md has every source):
 *   node      dtbo /fragment@29/__overlay__/als_rear@29, "ams,tcs3407", reg 0x29; the bus hsi2c_25 = hsi2c@109D0000,
 *             400 kHz (clock-frequency 0x61a80); exynos990-x1slte_eur_open_21.dts (ExtremeXT) is the same tree
 *   pins      hsi2c25-bus: gpp8-2 SCL, gpp8-3 SDA, function 3, no SoC pull (exynos9830.dts); gpp8 = PERIC1 +0x60
 *             (pinctrl-exynos-arm64.c exynos9830_pin_banks5). Interrupt gpa2-7 (not used: polled). led_en gpp4-4 is
 *             the flash LED's torch pin, Samsung's factory test only: never touched here
 *   supplies  "als_rear,vdd_1p8" = VDD_FLICKER_1P8 = S2MPS19 LDO25M, "als_rear,i2c_1p8" = VDD_NFC_1P8 = LDO22M (the bus's
 *             pull-ups); neither always-on (bsd/dev/redwood_acpm_pmic.c switches them, als=2)
 *   chip      drivers/optics/tcs3407.c/.h: ID 0x92 = 0x18, REVID 0x91 = 0x51 (0x01 untrimmed; TCS3408: 0x53/0x03),
 *             AUXID 0x90; ENABLE 0x80 (PON 0x01, AEN 0x02, FDEN 0x40), ATIME 0x81, STATUS 0x93, ASTATUS 0x94 ([3:0] the
 *             gain the AGC chose), ADATA0..4 0x95..0x9E (clear, red, green, blue, wideband, u16 LE), STATUS2 0xA3
 *             (AVALID 0x40, ASAT 0x08|0x10), CFG1 0xAA ([4:0] AGAIN: 0 = 0.5x, n = 2^(n-1)x), CFG6 0xAF, CFG8 0xB1
 *             (ALS_AGC_ENABLE 0x04), CFG10 0xB3 (AGC hysteresis [7:4]), PERS 0xBD, ASTEP 0xCA/0xCB (reset 999: 2.78 ms),
 *             AGC_GAIN_MAX 0xCF, FD_CFG3 0xDA, INTENAB 0xF9, SOFT_RESET 0xF3 (0x04)
 *   setup     Samsung's (ams_deviceInit, _3407_alsInit, ccb_alsInit): PON, soft reset, 1 ms; CFG6 0, AGC_GAIN_MAX 0x97,
 *             FD_CFG3 0x91; AGC on, low hysteresis 12.5 % high 87.5 %; ATIME 50000 / 2816 = 17 (18 steps of 2.78 ms:
 *             50 ms, full scale 18000 counts); PERS 1; AGAIN 64x; then ENABLE PON|AEN. Flicker (FDEN) is not used
 *   lux       Samsung's driver has no lux: it hands the camera irradiance, channel * coefficient / cpl with
 *             cpl = time_us * gain / 1000 / 2266 (STAR_D_FACTOR) and the coefficients C 118, R 112, G 172, B 180,
 *             WB 111 (amsAlg_als_processData, als_update_statics). Here lux = irradiance(green) * als_k / 1000,
 *             als_k uncalibrated (default 1000); NOTES.md says how to calibrate it against a reference
 *
 * Boot-args (nothing is touched without als=):
 *   als=1   a look: LDO22M/LDO25M as found (read), the bus, ID/REVID/AUXID (read only)
 *   als=2   ... LDO22M and LDO25M switched on if off (redwood_pmic_ldo_on(), read back), the IDs again
 *   als=3   ... Samsung's setup (every write read back), then a measurement every als_ms (default 200) into
 *           /dev/redwood-sensors; a line in the log every 10 s
 *   als_k=N  lux per 1000 units of green irradiance (default 1000)
 * Off (ENABLE 0) at the run's end and on a panic's way out (redwood_tcs3407_quiesce()).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_sensors.h>
#include <sys/systm.h>

extern int redwood_pmic_read(uint8_t type, uint8_t reg, uint8_t *value);
extern int redwood_pmic_ldo_on(unsigned ldo, char *report, size_t report_size);

#define BANK_GPP8               0x60U           /* PERIC1 */
#define ALS_ADDR                0x29U

#define REG_AUXID               0x90
#define REG_REVID               0x91
#define REG_ID                  0x92
#define REG_ENABLE              0x80
#define REG_ATIME               0x81
#define REG_STATUS              0x93
#define REG_ASTATUS             0x94
#define REG_ADATA0L             0x95
#define REG_STATUS2             0xA3
#define REG_CFG1                0xAA
#define REG_CFG6                0xAF
#define REG_CFG8                0xB1
#define REG_CFG10               0xB3
#define REG_PERS                0xBD
#define REG_AGC_GAIN_MAX        0xCF
#define REG_FD_CFG3             0xDA
#define REG_SOFT_RESET          0xF3
#define REG_INTENAB             0xF9

#define PON                     0x01
#define AEN                     0x02
#define AVALID                  0x40
#define ASAT_ANY                0x18
#define AINT_ASAT               0x88
#define ALS_AGC_ENABLE          0x04
#define ID_TCS3407              0x18
#define ATIME_50MS              17              /* 50000 / 2816 (alsTimeUsToReg) */
#define ASTEP_US_X100           278             /* (999 + 1) x 2.78 us = 2.78 ms a step */
#define AGAIN_64X               7
#define STAR_D_FACTOR           2266

#define PMIC_TYPE_PMIC          1
#define LDO22_CTRL              0x5B
#define LDO25_CTRL              0x5E

static struct redwood_bbi2c bus = { .name = "tcs3407 bus gpp8-2/3", .scl = 2, .sda = 3, .half_us = 3 };
static uint32_t level, period_ms = 200, als_k = 1000;
static volatile bool running, broken;

static bool
get(uint8_t reg, uint8_t *v, uint32_t n)
{
	return redwood_bbi2c_read_reg(&bus, ALS_ADDR, reg, v, n);
}

/* a write, read back (the self-clearing ones are not: `check` false) */
static bool
put(uint8_t reg, uint8_t value, bool check)
{
	uint8_t back = 0;

	if (broken) {
		return false;
	}
	if (!redwood_bbi2c_write_reg(&bus, ALS_ADDR, reg, value) || (check && (!get(reg, &back, 1) || back != value))) {
		redwood_dsay("als: tcs3407 %#04x <- %#04x read back %#04x: no more writes this run\n", reg, value, back);
		broken = true;
		return false;
	}
	return true;
}

static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t before = 0;

	return get(reg, &before, 1) && put(reg, (uint8_t)((before & ~mask) | (value & mask)), true);
}

static bool
ids(const char *when)
{
	uint8_t id = 0, rev = 0, aux = 0;

	if (!get(REG_ID, &id, 1) || !get(REG_REVID, &rev, 1) || !get(REG_AUXID, &aux, 1)) {
		redwood_dsay("als: %s: the TCS3407 does not answer at %#x\n", when, ALS_ADDR);
		return false;
	}
	redwood_dsay("als: %s: ID %#04x REVID %#04x AUXID %#04x (%s)\n", when, id, rev, aux,
	    id == ID_TCS3407 && (rev == 0x51 || rev == 0x01) ? "TCS3407" : id == ID_TCS3407 && (rev == 0x53 || rev == 0x03) ?
	    "TCS3408" : "not the chip expected");
	return id == ID_TCS3407;
}

/* Samsung's setup (see the head of the file), AGC on, the ALS on, no interrupt (polled) */
static bool
setup(void)
{
	if (!put(REG_ENABLE, PON, true) || !put_bits(REG_SOFT_RESET, 0x04, 0x04)) {
		return false;
	}
	delay_for_interval(2, NSEC_PER_MSEC);
	return put(REG_ENABLE, PON, true) && put(REG_CFG6, 0x00, true) && put(REG_AGC_GAIN_MAX, 0x97, true) &&
	       put(REG_FD_CFG3, 0x91, true) && put_bits(REG_CFG8, ALS_AGC_ENABLE, ALS_AGC_ENABLE) &&
	       put_bits(REG_CFG10, 0xF0, 0xC0) && put(REG_ATIME, ATIME_50MS, true) && put_bits(REG_PERS, 0x0F, 0x01) &&
	       put_bits(REG_CFG1, 0x1F, AGAIN_64X) && put(REG_INTENAB, 0x00, true) && put(REG_ENABLE, PON | AEN, true);
}

/* the gain in thousandths for ASTATUS/CFG1's code: 0 = 0.5x, n = 2^(n-1) */
static uint32_t
gain_x1000(uint32_t code)
{
	return code == 0 ? 500 : code > 11 ? 1024000 : 1000u << (code - 1);
}

static void
als_thread(__unused void *parameter, __unused wait_result_t result)
{
	char line[200];
	uint8_t l22 = 0, l25 = 0;
	uint32_t samples = 0;
	uint64_t last_line = 0;

	bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, BANK_GPP8);
	(void)redwood_pmic_read(PMIC_TYPE_PMIC, LDO22_CTRL, &l22);
	(void)redwood_pmic_read(PMIC_TYPE_PMIC, LDO25_CTRL, &l25);
	redwood_dsay("als: 1: LDO22M (VDD_NFC_1P8, the bus's pull-ups) CTRL %#04x, LDO25M (VDD_FLICKER_1P8) CTRL %#04x "
	    "(0xC0 set: on)\n", l22, l25);
	if (level >= 2) {
		(void)redwood_pmic_ldo_on(22, line, sizeof(line));
		redwood_dsay("als: 2: %s\n", line);
		(void)redwood_pmic_ldo_on(25, line, sizeof(line));
		redwood_dsay("als: 2: %s\n", line);
		delay_for_interval(5, NSEC_PER_MSEC);
	}
	if (!redwood_bbi2c_setup(&bus, false, line, sizeof(line)) && !redwood_bbi2c_setup(&bus, true, line, sizeof(line))) {
		redwood_dsay("als: %s: the bus is not usable\n", line);
		redwood_bbi2c_restore(&bus);
		return;
	}
	redwood_dsay("als: %s\n", line);
	if (!ids(level >= 2 ? "2" : "1") || level < 3) {
		redwood_bbi2c_restore(&bus);
		return;
	}
	if (!setup()) {
		redwood_dsay("als: 3: the setup did not take\n");
		(void)redwood_bbi2c_write_reg(&bus, ALS_ADDR, REG_ENABLE, 0);
		return;
	}
	running = true;
	redwood_sensors_device();
	redwood_dsay("als: 3: the rear light sensor runs: 50 ms, AGC from 64x, every %u ms into /dev/%s (lux = green "
	    "irradiance x %u / 1000, not calibrated)\n", period_ms, REDWOOD_SENSORS_NAME, als_k);
	while (!broken) {
		uint8_t st2 = 0, d[11];
		uint64_t ns;

		delay_for_interval(period_ms, NSEC_PER_MSEC);
		if (!get(REG_STATUS2, &st2, 1) || !(st2 & AVALID) || !get(REG_ASTATUS, d, sizeof(d))) {
			continue;
		}
		(void)redwood_bbi2c_write_reg(&bus, ALS_ADDR, REG_STATUS, AINT_ASAT);
		{
			uint32_t c = d[1] | (uint32_t)d[2] << 8, r = d[3] | (uint32_t)d[4] << 8;
			uint32_t g = d[5] | (uint32_t)d[6] << 8, b = d[7] | (uint32_t)d[8] << 8;
			uint32_t w = d[9] | (uint32_t)d[10] << 8, gain = gain_x1000(d[0] & 0x0f);
			uint64_t time_us = (uint64_t)(ATIME_50MS + 1) * ASTEP_US_X100 * 10;    /* 50040 us */
			uint64_t cpl = time_us * gain / 1000 / STAR_D_FACTOR;
			uint64_t irr_g = cpl ? (uint64_t)g * 172 / cpl : 0;
			uint64_t lux = irr_g * als_k / 1000;
			struct redwood_sensor_sample s = { 0 };

			if (st2 & ASAT_ANY) {
				lux = lux < 40000 ? 40000 : lux;        /* saturated at the lowest gain: very bright */
			}
			s.time = mach_absolute_time();
			s.type = REDWOOD_SENSOR_LIGHT;
			s.v[0] = (int32_t)(lux > 200000 ? 200000 : lux);
			s.v[1] = 0;
			s.v[2] = REDWOOD_LIGHT_REAR;
			s.v[3] = (int32_t)c;
			redwood_sensors_post(&s);
			samples++;
			absolutetime_to_nanoseconds(s.time, &ns);
			if (samples == 1 || ns - last_line >= 10 * NSEC_PER_SEC) {
				last_line = ns;
				redwood_dsay("als: C %u R %u G %u B %u WB %u, gain %u.%03ux%s: %llu lux (%u samples, %d readers)\n", c, r,
				    g, b, w, gain / 1000, gain % 1000, (st2 & ASAT_ANY) ? ", saturated" : "", lux, samples,
				    redwood_sensors_readers());
			}
		}
	}
	running = false;
	redwood_dsay("als: given up on after %u samples\n", samples);
}

void
redwood_tcs3407_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("als", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 3 ? 3 : level;
	PE_parse_boot_argn("als_ms", &period_ms, sizeof(period_ms));
	PE_parse_boot_argn("als_k", &als_k, sizeof(als_k));
	period_ms = period_ms < 60 ? 60 : period_ms > 5000 ? 5000 : period_ms;
	if (kernel_thread_start(als_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* the run's end, a panic's way out: the ALS off (no sleeping, no PMIC: the supplies stay as they are) */
void
redwood_tcs3407_quiesce(void)
{
	if (running) {
		broken = true;
		redwood_dsay("als: at the run's end: %s\n", redwood_bbi2c_write_reg(&bus, ALS_ADDR, REG_ENABLE, 0) ? "off" :
		    "COULD NOT BE SWITCHED OFF");
	}
}

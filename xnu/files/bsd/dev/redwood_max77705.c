/*
 * The Galaxy S20+'s (y2s, Exynos 990) battery: the MAX77705's fuel gauge and charger, read over
 * I2C bit-banged on the pins of HSI2C_7 (gpp1-6 SCL, gpp1-7 SDA: bsd/dev/redwood_bbi2c.c), and
 * handed to iOS as the power source (redwood_battery_report() -> iokit/Kernel/RedwoodBattery.cpp).
 *
 * READ ONLY. The MAX77705 charges by itself with what the bootloader set; nothing is ever written
 * to it here (a wrong charger setting could stop the charging, or worse).
 *
 * From Samsung's Exynos 990 kernel (drivers/mfd/max77705.c, drivers/battery_v2/max77705_*.c,
 * include/linux/mfd/max77705-private.h) and the phone's device tree (max77705@66 on hsi2c_7,
 * fuelgauge,fg_resistor = 2, battery,battery_full_capacity = 4500 mAh):
 *
 *   fuel gauge 0x36, 16-bit little-endian registers:
 *     0x00 STATUS     bit 3: no battery
 *     0x05 RepCap     1 mAh x fg_resistor / 2
 *     0x06 RepSOC     1/256 %
 *     0x08 Temp       1/256 C, signed (only meaningful if the gauge has a thermistor)
 *     0x09 VCell      78.125 uV
 *     0x0A Current    signed, 156.25 uA x fg_resistor (fg_resistor 2: 312.5 uA)
 *     0x0B AvgCurrent the same
 *     0x17 Cycles     (zeroed by Samsung's driver at every boot: not reported)
 *     0x35 FullCapRep as RepCap
 *     0xB3 VBYP       0.427246 mV (the input side: VBUS when a charger is in)
 *     0xD0 IIN        0.125 mA (input current)
 *   charger 0x69:
 *     0xB2 CHG_INT_OK bit 6 CHGIN_OK (wired power), bit 5 WCIN_OK (wireless), bit 2 BATP_OK
 *     0xB3 DETAILS_00 bits 6:5 CHGIN_DTLS (3: VBUS valid), bits 4:3 WCIN_DTLS (3: valid)
 *     0xB4 DETAILS_01 bits 3:0 CHG_DTLS: 0-2 charging, 3-4 done, 5-7 not charging, 8/A/B off
 *
 * Boot args: bms=1 (on), bms_secs=N (poll period, default 5), bms_trace=1 (every reading).
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_input.h>
#include <sys/redwood_m2m.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/*
 * The thermistors, on the Exynos ADC v3 (adc@15C40000, CMU_CMGP's clock: on), as Samsung's battery_v2 reads them
 * (exynos990-x1slte DTS: battery adc-temp ch 4, adc-chg-temp ch 3, adc-usb-temp ch 1; drivers/iio/adc/exynos_adc.c
 * v3: CON2 = C_TIME(6) | channel, CON1 = 2 | START, DAT bit 31 = done, 12 bits). Tables in 0.1 C, ADC falls as it
 * warms. The battery's is THE battery temperature (the gauge has no thermistor: it read 33.00 C, made up).
 */
#define ADC_PHYS                0x15c40000UL
static volatile uint32_t *adc;
static const uint16_t bat_adc[] = { 0x13a, 0x16c, 0x1a5, 0x1e9, 0x23a, 0x29a, 0x30c, 0x382, 0x414, 0x4b8, 0x574, 0x63e,
	0x719, 0x801, 0x8ee, 0x9d6, 0xac0, 0xb95, 0xc57, 0xd0d, 0xda6, 0xe16, 0xe78 };
static const uint16_t usb_adc[] = { 0x113, 0x150, 0x19f, 0x1e3, 0x227, 0x29b, 0x2fc, 0x387, 0x41e, 0x4a5, 0x568, 0x627,
	0x6fb, 0x7fa, 0x8d7, 0x9bc, 0xaa3, 0xb79, 0xc37, 0xced, 0xd87, 0xdfa, 0xe5e };   /* also the charger's */
/* 90.0 C down to -20.0 C in 5 C steps, for both tables */
#define TEMP_TOP_DECI           900
static int32_t batt_deci = INT32_MIN, chg_deci = INT32_MIN, usb_deci = INT32_MIN;      /* 0.1 C; MIN: unknown */
static uint32_t adc_raw[3];

static int
adc_read(unsigned ch)
{
	if (adc == NULL) {
		return -1;
	}
	adc[1] = 0x60U | (ch & 0xfU);           /* CON2: C_TIME(6), channel */
	adc[0] = 0x3U;                          /* CON1: non-reset, start */
	for (int i = 0; i < 200; i++) {
		uint32_t d = adc[2];            /* DAT */

		if ((d & 0x80000000U) && i > 0) {
			return (int)(d & 0xfffU);
		}
		delay(10);
	}
	return -1;
}

static int32_t
adc_to_deci(int raw, const uint16_t *t)
{
	if (raw < 0) {
		return INT32_MIN;
	}
	if (raw <= t[0]) {
		return TEMP_TOP_DECI;
	}
	for (int i = 1; i < 23; i++) {
		if (raw <= t[i]) {
			/* between t[i-1] (warmer) and t[i] (5 C cooler) */
			return TEMP_TOP_DECI - 50 * (i - 1) - (int32_t)(50 * (raw - t[i - 1]) / (t[i] - t[i - 1]));
		}
	}
	return TEMP_TOP_DECI - 50 * 22;
}

static void
thermistors_read(void)
{
	int r;

	if (adc == NULL) {
		adc = (volatile uint32_t *)ml_io_map(ADC_PHYS, 0x1000);
		if (adc == NULL) {
			return;
		}
		adc[0] = 0x4U;                  /* CON1: soft reset, then out of it (Samsung's v3 init) */
		adc[0] = 0x2U;
		adc[4] = 0;                     /* INT_EN: polled, no interrupt */
	}
	r = adc_read(4); adc_raw[0] = (uint32_t)r; batt_deci = adc_to_deci(r, bat_adc);
	r = adc_read(3); adc_raw[1] = (uint32_t)r; chg_deci = adc_to_deci(r, usb_adc);
	r = adc_read(1); adc_raw[2] = (uint32_t)r; usb_deci = adc_to_deci(r, usb_adc);
}

static int batt_c10_sysctl, chg_c10_sysctl, usb_c10_sysctl;
SYSCTL_DECL(_kern_redwood);
SYSCTL_INT(_kern_redwood, OID_AUTO, batt_temp, CTLFLAG_RD | CTLFLAG_LOCKED, &batt_c10_sysctl, 0, "battery thermistor, 0.1 C");
SYSCTL_INT(_kern_redwood, OID_AUTO, chg_temp, CTLFLAG_RD | CTLFLAG_LOCKED, &chg_c10_sysctl, 0, "charger thermistor, 0.1 C");
SYSCTL_INT(_kern_redwood, OID_AUTO, usb_temp, CTLFLAG_RD | CTLFLAG_LOCKED, &usb_c10_sysctl, 0, "USB port thermistor, 0.1 C");

void redwood_usb_gadget_vbus(bool present);
bool redwood_usb_gadget_host(void);           /* redwood_dwc3.c: a computer has configured the USB link */    /* redwood_dwc3.c: the cable, for the USB link's restarts */

#define BANK_GPP1               0x20U           /* PERIC0 */

#define FG_ADDR                 0x36U
#define CHG_ADDR                0x69U
#define MUIC_ADDR               0x25U           /* the USB-C/MUIC firmware */
#define MUIC_BC_STATUS          0x08U           /* bit 7 VBUS, bits 1:0 charger type: 0 none, 1 USB host, 2 charging host, 3 wall */

#define FG_STATUS               0x00U
#define FG_REPCAP               0x05U
#define FG_REPSOC               0x06U
#define FG_TEMP                 0x08U
#define FG_VCELL                0x09U
#define FG_CURRENT              0x0AU
#define FG_AVG_CURRENT          0x0BU
#define FG_CYCLES               0x17U
#define FG_DESIGNCAP            0x18U
#define FG_FULLCAP_REP          0x35U
#define FG_VBYP                 0xB3U
#define FG_IIN                  0xD0U

#define CHG_INT_OK              0xB2U
#define CHG_DETAILS_00          0xB3U
#define CHG_DETAILS_01          0xB4U
#define CHG_CNFG_00             0xB7U           /* bits 3:0 MODE (5: charger + buck) */
#define CHG_CNFG_02             0xB9U           /* bits 5:0 CHG_CC, fast-charge current, 50 mA steps */
#define CHG_CNFG_06             0xBDU           /* bits 3:2 CHGPROT (3: config registers unlocked) */
#define CHG_CNFG_09             0xC0U           /* bits 6:0 CHGIN_LIM, input limit, (n + 1) x 25 mA */

#define CHG_INT_OK_WCIN         (1U << 5)
#define CHG_INT_OK_CHGIN        (1U << 6)
#define CHG_CHGIN_DTLS(d00)     (((d00) >> 5) & 3U)     /* max77705_get_vbus_state(): 3 = VBUS valid */
#define CHG_WCIN_DTLS(d00)      (((d00) >> 3) & 3U)

#define FG_RESISTOR             2               /* fuelgauge,fg_resistor */
#define DESIGN_MAH              4500            /* battery,battery_full_capacity */

/* Linux's POWER_SUPPLY_STATUS_*, which struct redwood_battery_state speaks */
#define PS_STATUS_CHARGING      1
#define PS_STATUS_DISCHARGING   2
#define PS_STATUS_NOT_CHARGING  3
#define PS_STATUS_FULL          4

static struct redwood_bbi2c bus = { .name = "max77705 bus gpp1-6/7", .scl = 6, .sda = 7, .half_us = 4 };
static uint32_t trace, wdt_kick, ilim = 1;      /* ilim on by default (2026-11-20): a computer port capped, see usb_ma */
static uint32_t usb_ma = 1500;                  /* boot-arg bms_usb_ma: the cap on a computer port (USB host / charging host) */
static uint64_t wdt_kicks, wdt_expired_seen;

static bool
fg_word(uint8_t reg, uint16_t *value)
{
	uint8_t b[2];

	if (!redwood_bbi2c_read_reg(&bus, FG_ADDR, reg, b, 2)) {
		return false;
	}
	*value = (uint16_t)(b[0] | (b[1] << 8));
	return true;
}

static bool
chg_byte(uint8_t reg, uint8_t *value)
{
	return redwood_bbi2c_read_reg(&bus, CHG_ADDR, reg, value, 1);
}

/* signed 16-bit current register to uA */
static int32_t
current_ua(uint16_t raw)
{
	return (int32_t)(int16_t)raw * 15625 * FG_RESISTOR / 100;
}

static int
charge_status(bool external, uint8_t details01)
{
	if (!external) {
		return PS_STATUS_DISCHARGING;
	}
	switch (details01 & 0x0f) {
	case 0x0: case 0x1: case 0x2:
		return PS_STATUS_CHARGING;
	case 0x3: case 0x4:
		return PS_STATUS_FULL;
	case 0x8: case 0xA: case 0xB:
		return PS_STATUS_DISCHARGING;
	default:
		return PS_STATUS_NOT_CHARGING;
	}
}

static const char *
status_name(int status)
{
	switch (status) {
	case PS_STATUS_CHARGING: return "charging";
	case PS_STATUS_DISCHARGING: return "discharging";
	case PS_STATUS_FULL: return "full";
	default: return "not charging";
	}
}

/*
 * bms_ilim=1 (x1slte, 2026-11-06): the bootloader leaves the charger at 900 mA in (CNFG_09 0x23) and 900 mA
 * into the battery (CNFG_02 0x52), which is what iOS draws while awake: the battery gained nothing. As
 * Samsung's driver does once the cable is known (max77705_set_input_current / _set_charge_current, the
 * values from the phone's dtbo cable-info: TA 1650 mA in, CDP 1500 mA in), the input limit is raised
 * for a wall charger or a charging host. The charge current is kept at 1500 mA, below Samsung's 3150:
 * Samsung's software watches the battery's temperature and steps the current down, and this kernel does
 * not, so it stays at the bootloader's values outside 10..42 C. A USB host, no cable or a temperature
 * outside the range gets the bootloader's own values back. Every write is read back; on a mismatch the
 * feature switches itself off.
 */
/*
 * bms_hv=1 (x1slte, 2026-11-06): 9 V charging, as Samsung's kernel does it through the MAX77705's USB-C firmware (the
 * "MUIC" at 0x25), from drivers/muic/max77705-muic-afc.c, drivers/usb/typec/maxim/max77705/max77705_pd.c and _usbc.c,
 * include/linux/muic/max77705-muic.h, include/linux/usb/typec/maxim/max77705.h:
 *   an opcode goes to OPCODE_WRITE (0x21) with its data, then OPCODE_WRITE_END (0x41) = 0; the firmware's answer is
 *   flagged by UIC_INT (0x02) bit 7 APCmdResI and waits at OPCODE_READ (0x51), the opcode first.
 *   USB-PD (PD_STATUS1 0x0d bit 4 PSRDY: a contract): OPCODE_CURRENT_SRCCAP (0x30) gives [1] bits 5:3 the PDO in use,
 *   bits 2:0 how many, then 4 bytes each (a fixed PDO: bits 31:30 0, 19:10 volts x 50 mV, 9:0 amps x 10 mA);
 *   OPCODE_SRCCAP_REQUEST (0x32) [n] asks for PDO n; [1] 0 = sent.
 *   AFC (a wall charger without PD): COMMAND_AFC_RESULT_READ (0x21) [0x46] asks for 9 V 1.65 A ([0x08]: 5 V); the
 *   answer's [1] 0 = done, [2] the VBUS ADC.
 *   USBC_STATUS1 (0x06) bits 7:4: VBUS's ADC (2 = 4.5-5.5 V, 6 = 8.5-9.5 V, 7 = 9.5-10.5 V, 8+ = over 10.5 V).
 * Samsung's numbers for this phone (dtbo battery/cable-info): pre_afc_input_current 500 mA while VBUS changes, HV TA
 * input 1650 mA, pd_charging_charge_power 15000 mW (so 1650 mA at 9 V), max_input_voltage 9000 mV. The charge current
 * on 9 V is 2500 mA between 15 and 40 C (Samsung's goes to 3150 with its temperature steps, which this kernel lacks),
 * 1500 mA otherwise, and outside 10..42 C the phone asks for 5 V back. Over 10.5 V on VBUS: 5 V asked for, bootloader
 * limits, and no 9 V again until the cable is replugged. Two failed tries per plug-in.
 */
#define MUIC_UIC_INT            0x02U
#define  UIC_INT_APCMDRES       0x80U
#define MUIC_USBC_STATUS1       0x06U
#define MUIC_PD_STATUS1         0x0dU
#define  PD_PSRDY               0x10U
#define MUIC_OPCODE_WRITE       0x21U
#define MUIC_OPCODE_END         0x41U
#define MUIC_OPCODE_READ        0x51U
#define OP_AFC                  0x21U
#define OP_CURRENT_SRCCAP       0x30U
#define OP_SRCCAP_REQUEST       0x32U
#define VBADC(st1)              (((st1) >> 4) & 0xfU)

enum hv_state { HV_IDLE, HV_ACTIVE, HV_FAILED, HV_FAULT };
static uint32_t hv_on;
static unsigned plugged_polls;          /* polls since the cable went in (charger_limits) */
static enum hv_state hv_state;
static unsigned hv_tries, pd9_ma, pd5_ma;   /* pd5_ma: the 5 V PD contract's current, when 9 V is not had */
static bool hv_pd;

static bool
muic_opcode(uint8_t op, const uint8_t *data, unsigned len, uint8_t *answer, unsigned answer_len, unsigned wait_ms)
{
	uint8_t w[34], intr = 0;

	if (len > 32 || answer_len > 33) {
		return false;
	}
	(void)redwood_bbi2c_read_reg(&bus, MUIC_ADDR, MUIC_UIC_INT, &intr, 1);       /* (clears an old answer's flag) */
	w[0] = MUIC_OPCODE_WRITE;
	w[1] = op;
	for (unsigned i = 0; i < len; i++) {
		w[2 + i] = data[i];
	}
	if (!redwood_bbi2c_xfer(&bus, MUIC_ADDR, w, len + 2, NULL, 0) ||
	    (len < 32 && !redwood_bbi2c_write_reg(&bus, MUIC_ADDR, MUIC_OPCODE_END, 0))) {
		return false;
	}
	for (unsigned ms = 0; ms < wait_ms; ms += 20) {
		delay_for_interval(20, NSEC_PER_MSEC);
		if (redwood_bbi2c_read_reg(&bus, MUIC_ADDR, MUIC_UIC_INT, &intr, 1) && (intr & UIC_INT_APCMDRES)) {
			return redwood_bbi2c_read_reg(&bus, MUIC_ADDR, MUIC_OPCODE_READ, answer, answer_len) && answer[0] == op;
		}
	}
	return false;
}

static unsigned
vbus_adc(void)
{
	uint8_t st1 = 0;

	return redwood_bbi2c_read_reg(&bus, MUIC_ADDR, MUIC_USBC_STATUS1, &st1, 1) ? VBADC(st1) : 0;
}

static void charger_write_limits(unsigned in_ma, unsigned cc_ma);

/* VBUS between 8.5 and 10.5 V within 3 s */
static bool
hv_wait_9v(void)
{
	unsigned adc = 0;

	for (unsigned ms = 0; ms < 3000; ms += 100) {
		adc = vbus_adc();
		if (adc == 6 || adc == 7) {
			return true;
		}
		if (adc >= 8) {
			break;
		}
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	m2m_klog('W', "DEV", "bms: hv: VBUS ADC %u after the request, not 9 V", adc);
	return false;
}

/* back to 5 V: AFC 0x08, or PD's first PDO (always 5 V) */
static void
hv_back_to_5v(const char *why)
{
	uint8_t a[12] = { 0 }, b = hv_pd ? 1 : 0x08;

	charger_write_limits(500, 0);
	(void)muic_opcode(hv_pd ? OP_SRCCAP_REQUEST : OP_AFC, &b, 1, a, hv_pd ? 2 : 11, 1500);
	m2m_klog('W', "DEV", "bms: hv: back to 5 V (%s): answer %#x %#x, VBUS ADC %u", why, a[0], a[1], vbus_adc());
}

/* a try at 9 V: true when VBUS is there */
static bool
hv_try(unsigned charger_type)
{
	uint8_t pd1 = 0, a[33] = { 0 }, n;

	hv_pd = redwood_bbi2c_read_reg(&bus, MUIC_ADDR, MUIC_PD_STATUS1, &pd1, 1) && (pd1 & PD_PSRDY);
	if (hv_pd) {
		unsigned count, now, best = 0, best_ma = 0;

		if (!muic_opcode(OP_CURRENT_SRCCAP, NULL, 0, a, 30, 1000)) {
			m2m_klog('W', "DEV", "bms: hv: PD contract, but its source capabilities did not come");
			return false;
		}
		now = (a[1] >> 3) & 7U;
		count = a[1] & 7U;
		for (unsigned i = 0; i < count && 5 + i * 4 < sizeof(a); i++) {
			uint32_t pdo = a[2 + i * 4] | (a[3 + i * 4] << 8) | (a[4 + i * 4] << 16) | ((uint32_t)a[5 + i * 4] << 24);
			unsigned mv = ((pdo >> 10) & 0x3ffU) * 50, ma = (pdo & 0x3ffU) * 10;

			m2m_klog('I', "DEV", "bms: hv: PDO %u: %#x: %s %u mV %u mA%s", i + 1, pdo, (pdo >> 30) ? "not fixed," : "fixed",
			    mv, ma, i + 1 == now ? " (in use)" : "");
			if (i + 1 == now && (pdo >> 30) == 0 && mv == 5000) {
				pd5_ma = ma;
			}
			if ((pdo >> 30) == 0 && mv == 9000 && ma >= 1500) {
				best = i + 1;
				best_ma = ma;
			}
		}
		if (best == 0) {
			m2m_klog('I', "DEV", "bms: hv: this PD charger offers no fixed 9 V of 1.5 A or more: staying at 5 V");
			hv_tries = 99;
			return false;
		}
		pd9_ma = best_ma;
		charger_write_limits(500, 0);
		n = (uint8_t)best;
		if (!muic_opcode(OP_SRCCAP_REQUEST, &n, 1, a, 2, 1500) || a[1] != 0) {
			m2m_klog('W', "DEV", "bms: hv: PDO %u (9 V %u mA) request: answer %#x %#x", best, best_ma, a[0], a[1]);
			return false;
		}
		m2m_klog('I', "DEV", "bms: hv: PDO %u (9 V %u mA) asked for", best, best_ma);
		return hv_wait_9v();
	}
	if (charger_type != 3) {
		return false;
	}
	charger_write_limits(500, 0);
	n = 0x46;               /* 9 V, 1.65 A */
	if (!muic_opcode(OP_AFC, &n, 1, a, 11, 3000)) {
		m2m_klog('W', "DEV", "bms: hv: AFC: no answer from the USB-C firmware in 3 s");
		return false;
	}
	m2m_klog('I', "DEV", "bms: hv: AFC answer: result %u, VBUS ADC %u (rx %02x %02x %02x %02x)", a[1], a[2], a[3], a[4], a[5],
	    a[6]);
	return a[1] == 0 && hv_wait_9v();
}

static int16_t boot_c2 = -1, boot_c9 = -1;

/* in_ma 0: the bootloader's values; cc_ma 0 with in_ma: 1500 */
static void
charger_write_limits(unsigned in_ma, unsigned cc_ma)
{
	uint8_t c2 = 0, c9 = 0, want2, want9, r2 = 0, r9 = 0;
	static unsigned last_in = ~0U, last_cc = ~0U;

	if (!ilim || !chg_byte(CHG_CNFG_02, &c2) || !chg_byte(CHG_CNFG_09, &c9)) {
		return;
	}
	if (boot_c2 < 0) {
		boot_c2 = c2;
		boot_c9 = c9;
		m2m_klog('I', "DEV", "bms: the bootloader's limits: input %u mA (CNFG_09 %#x), charge %u mA (CNFG_02 %#x)",
		    ((c9 & 0x7fU) + 1) * 25, c9, (c2 & 0x3fU) * 50, c2);
	}
	if (in_ma) {
		cc_ma = cc_ma ? cc_ma : 1500;
		want9 = (uint8_t)((c9 & 0x80U) | ((in_ma / 25 - 1) & 0x7fU));
		want2 = (uint8_t)((c2 & ~0x3fU) | ((cc_ma / 50) & 0x3fU));
	} else {
		want9 = (uint8_t)boot_c9;
		want2 = (uint8_t)boot_c2;
	}
	if (want9 == c9 && want2 == c2) {
		return;
	}
	if (!redwood_bbi2c_write_reg(&bus, CHG_ADDR, CHG_CNFG_09, want9) ||
	    !redwood_bbi2c_write_reg(&bus, CHG_ADDR, CHG_CNFG_02, want2) ||
	    !chg_byte(CHG_CNFG_09, &r9) || !chg_byte(CHG_CNFG_02, &r2) || r9 != want9 || r2 != want2) {
		m2m_klog('W', "DEV", "bms: charger limits did not take (CNFG_09 %#x -> %#x reads %#x, CNFG_02 %#x -> %#x reads "
		    "%#x): bms_ilim and bms_hv switched off, bootloader values put back", c9, want9, r9, c2, want2, r2);
		ilim = 0;
		hv_on = 0;
		redwood_bbi2c_write_reg(&bus, CHG_ADDR, CHG_CNFG_09, (uint8_t)boot_c9);
		redwood_bbi2c_write_reg(&bus, CHG_ADDR, CHG_CNFG_02, (uint8_t)boot_c2);
		return;
	}
	if (in_ma != last_in || cc_ma != last_cc) {
		m2m_klog('I', "DEV", "bms: charger limits: input %u mA, charge %u mA (CNFG_09 %#x, CNFG_02 %#x)%s",
		    ((r9 & 0x7fU) + 1) * 25, (r2 & 0x3fU) * 50, r9, r2, in_ma ? "" : " -- the bootloader's");
		last_in = in_ma;
		last_cc = cc_ma;
	}
}

static void
charger_limits(bool external, int32_t temp_hundredths)
{
	uint8_t bc = 0;
	unsigned type, in_ma = 0, cc_ma = 0;
	bool warm_ok = temp_hundredths >= 1000 && temp_hundredths <= 4200;

	if (!ilim) {
		return;
	}
	if (!redwood_bbi2c_read_reg(&bus, MUIC_ADDR, MUIC_BC_STATUS, &bc, 1)) {
		bc = 0;                 /* unknown: treated as no charger */
	}
	type = bc & 3U;
	if (!external || !(bc & 0x80U)) {
		/* unplugged: the charger is back at 5 V by itself; a new plug-in may try again */
		if (hv_state != HV_IDLE) {
			m2m_klog('I', "DEV", "bms: hv: cable out");
		}
		hv_state = HV_IDLE;
		hv_tries = 0;
		pd9_ma = 0;
		pd5_ma = 0;
		plugged_polls = 0;
		charger_write_limits(0, 0);
		return;
	}
	if (hv_on && hv_state == HV_ACTIVE) {
		unsigned adc = vbus_adc();

		if (adc >= 8) {
			hv_back_to_5v("VBUS over 10.5 V");
			hv_state = HV_FAULT;
		} else if (!warm_ok) {
			hv_back_to_5v("battery temperature outside 10..42 C");
			hv_state = HV_FAILED;
		} else if (adc <= 3) {
			m2m_klog('W', "DEV", "bms: hv: VBUS fell back to 5 V (ADC %u): the charger ended it", adc);
			hv_state = HV_FAILED;
		}
	}
	if (plugged_polls < 1000) {
		plugged_polls++;
	}
	/*
	 * 9 V only from a charger, never a computer: AFC talks on D+/D-, the USB data lines -- on a Mac's port (detected
	 * as type 3) it broke the link and the Mac cut the port, charge and data both (2026-11-20). So only after the cable
	 * has been in for 3 polls (a computer configures the link within that) with no host on the link.
	 */
	if (hv_on && hv_state == HV_IDLE && redwood_usb_gadget_host()) {
		hv_state = HV_FAILED;
		m2m_klog('I', "DEV", "bms: hv: a computer on the USB link: 5 V only on this plug-in");
	}
	if (hv_on && hv_state == HV_IDLE && plugged_polls >= 3 && warm_ok && (type == 3 || type == 0 || type == 2)) {
		if (hv_try(type)) {
			hv_state = HV_ACTIVE;
			m2m_klog('I', "DEV", "bms: hv: 9 V by %s, VBUS ADC %u", hv_pd ? "USB PD" : "AFC", vbus_adc());
		} else if (++hv_tries >= 2) {
			hv_state = HV_FAILED;
			m2m_klog('I', "DEV", "bms: hv: no 9 V on this plug-in (%u tries): 5 V charging", hv_tries);
		}
	}
	if (warm_ok) {
		if (hv_state == HV_ACTIVE) {
			/* Respect the selected PD profile as well as Samsung's 9 V input limit. */
			in_ma = hv_pd && pd9_ma < 1650 ? pd9_ma : 1650;
			cc_ma = temp_hundredths >= 1500 && temp_hundredths <= 4000 ? 2500 : 1500;
		} else {
			/* a Mac/PC port: no more than it offers (a Mac cuts the port, charge and data both, when overdrawn:
			 * 1.7-2.0 A at 4.6 V seen, 2026-11-20); a wall charger: Samsung's 1650 mA */
			in_ma = type == 3 ? 1650 : (type == 1 || type == 2) ? usb_ma : 0;
			/* a Mac's port is detected as a wall charger (type 3, 2026-11-20): a configured USB link says computer */
			if (in_ma > usb_ma && redwood_usb_gadget_host()) {
				in_ma = usb_ma;
			}
			/* a 5 V USB PD contract (a USB-C laptop: 5 V 3 A) says what may be taken: up to Samsung's 3000 mA */
			if (pd5_ma >= 1500) {
				in_ma = pd5_ma > 3000 ? 3000 : pd5_ma;
				cc_ma = temp_hundredths >= 1500 && temp_hundredths <= 4000 ? 2500 : 1500;
			}
		}
	}
	/*
	 * Samsung's protection (exynos990-x1slte DTS, battery_v2), on the battery's own thermistor -- never the CPU's --
	 * and the charger chip's for its rule. Only ever lowers. 0.1 C, 2 C hysteresis as Samsung's:
	 *   battery >= 50 (back < 48) or <= 0 (back > 2): charging stopped (100 mA, the charger's floor)
	 *   battery >= 42 (back < 40): 1350 mA (swelling)        battery < 5 / < 15: 350 / 900 mA
	 *   charger chip >= 60 (back < 58): input 1000, charge 1900 mA
	 */
	{
		static int hot_stop, cold_stop, swell, chg_hot;
		int32_t t = batt_deci, c = chg_deci;
		unsigned cap = 0;

		if (t != INT32_MIN) {
			hot_stop = hot_stop ? t >= 480 : t >= 500;
			cold_stop = cold_stop ? t <= 20 : t <= 0;
			swell = swell ? t >= 400 : t >= 420;
			if (hot_stop || cold_stop) {
				cap = 100;
			} else if (swell) {
				cap = 1350;
			} else if (t < 50) {
				cap = 350;
			} else if (t < 150) {
				cap = 900;
			}
		}
		if (c != INT32_MIN) {
			chg_hot = chg_hot ? c >= 580 : c >= 600;
		}
		if (cap || chg_hot) {
			if (in_ma == 0) {
				in_ma = usb_ma;                 /* the bootloader's values are not a limit we know */
			}
			if (cc_ma == 0) {
				cc_ma = 1500;
			}
			if (cap && cc_ma > cap) {
				cc_ma = cap;
			}
			if (chg_hot) {
				in_ma = in_ma > 1000 ? 1000 : in_ma;
				cc_ma = cc_ma > 1900 ? 1900 : cc_ma;
			}
		}
		{
			static unsigned last_cap = ~0U;
			static int last_chg_hot = -1;

			if (cap != last_cap || chg_hot != last_chg_hot) {
				m2m_klog('I', "DEV", "bms: temperature protection: battery %d.%d C, charger %d.%d C -> %s%s",
				    t / 10, t < 0 ? -(t % 10) : t % 10, c / 10, c < 0 ? -(c % 10) : c % 10,
				    cap == 100 ? "charging stopped" : cap ? "charge current lowered" : "no battery limit",
				    chg_hot ? ", charger hot: input 1000 mA" : "");
				last_cap = cap;
				last_chg_hot = chg_hot;
			}
		}
	}
	charger_write_limits(in_ma, cc_ma);
}

static bool
poll_once(bool say)
{
	struct redwood_battery_state b;
	uint16_t status, repcap, repsoc, temp, vcell, cur, avg, cycles, fullcap, designcap, vbyp, iin;
	uint8_t int_ok, d00, d01;
	bool external;
	int32_t temp_hundredths;

	if (!fg_word(FG_REPSOC, &repsoc) || !fg_word(FG_VCELL, &vcell) || !fg_word(FG_CURRENT, &cur) ||
	    !fg_word(FG_AVG_CURRENT, &avg) || !fg_word(FG_STATUS, &status) || !fg_word(FG_REPCAP, &repcap) ||
	    !fg_word(FG_FULLCAP_REP, &fullcap) || !fg_word(FG_DESIGNCAP, &designcap) || !fg_word(FG_CYCLES, &cycles) ||
	    !fg_word(FG_TEMP, &temp) || !fg_word(FG_VBYP, &vbyp) || !fg_word(FG_IIN, &iin)) {
		return false;
	}
	if (!chg_byte(CHG_INT_OK, &int_ok) || !chg_byte(CHG_DETAILS_00, &d00) || !chg_byte(CHG_DETAILS_01, &d01)) {
		return false;
	}
	/*
	 * The charger's watchdog (x1slte, 2026-11-06): Samsung's kernel enables it (CNFG_00 WDTEN) and kicks it
	 * from its charger driver (max77705_chg_set_wdtmr_kick: CNFG_06 WDTCLR = 01). Nobody kicks it under iOS,
	 * so about a minute after boot CHG_DTLS reads 0xB, "WDT expired": the charger stops charging and the
	 * phone runs from the cable while the battery drains. bms_wdt=1 kicks it at every poll, as Samsung's
	 * kernel does: a read-modify-write of WDTCLR (bits 1:0) only, nothing about currents or voltages.
	 */
	if (wdt_kick) {
		uint8_t c6 = 0, after = 0;

		if ((d01 & 0x0fU) == 0x0bU && wdt_expired_seen++ < 5) {
			m2m_klog('W', "DEV", "bms: the charger's watchdog had expired (CHG_DTLS 0xB): kicking it");
		}
		if (chg_byte(CHG_CNFG_06, &c6) &&
		    redwood_bbi2c_write_reg(&bus, CHG_ADDR, CHG_CNFG_06, (uint8_t)((c6 & ~0x03U) | 0x01U))) {
			if (wdt_kicks++ == 0) {
				chg_byte(CHG_CNFG_06, &after);
				m2m_klog('I', "DEV", "bms: watchdog kicked: CNFG_06 %#x -> wrote %#x, reads %#x", c6,
				    (c6 & ~0x03U) | 0x01U, after);
			}
		}
	}

	/* a cable or a pad: its OK bit and its input valid, as Samsung's driver reads it (max77705_get_vbus_state) */
	external = ((int_ok & CHG_INT_OK_CHGIN) && CHG_CHGIN_DTLS(d00) == 3) ||
	    ((int_ok & CHG_INT_OK_WCIN) && CHG_WCIN_DTLS(d00) == 3);

	/* the gauge's temperature is only real with a thermistor on it (Samsung reads one on the AP's ADC) */
	temp_hundredths = (int32_t)(int16_t)temp * 100 / 256;
	if (temp_hundredths < -2000 || temp_hundredths > 7000) {
		temp_hundredths = 2500;
	}
	/* the real one: the battery's thermistor on the AP's ADC (2026-11-20) */
	thermistors_read();
	if (batt_deci != INT32_MIN) {
		temp_hundredths = batt_deci * 10;
	}
	batt_c10_sysctl = batt_deci == INT32_MIN ? -999 : batt_deci;
	chg_c10_sysctl = chg_deci == INT32_MIN ? -999 : chg_deci;
	usb_c10_sysctl = usb_deci == INT32_MIN ? -999 : usb_deci;

	{
		uint8_t bc;

		if (redwood_bbi2c_read_reg(&bus, MUIC_ADDR, MUIC_BC_STATUS, &bc, 1)) {
			redwood_usb_gadget_vbus((bc & 0x80U) != 0);
		}
	}
	charger_limits(external, temp_hundredths);

	bzero(&b, sizeof(b));
	b.size = sizeof(b);
	b.percent_hundredths = (int32_t)((uint32_t)repsoc * 100 / 256);
	if (b.percent_hundredths > 10000) {
		b.percent_hundredths = 10000;
	}
	b.voltage_uv = (int32_t)((uint64_t)vcell * 78125 / 1000);      /* (64-bit: 4.3 V overflows 32) */
	b.current_ua = current_ua(avg);
	b.temperature_hundredths = temp_hundredths;
	b.present = (status & (1U << 3)) == 0;
	b.external = external;
	b.status = charge_status(external, d01);
	b.charge_uah = (int32_t)repcap * FG_RESISTOR / 2 * 1000;
	b.full_uah = (int32_t)fullcap * FG_RESISTOR / 2 * 1000;
	b.design_uah = DESIGN_MAH * 1000;
	b.cycles = 0;                           /* Samsung's driver zeroes the gauge's count: the real one is kept in its EFS */
	b.usb_type = 0;                         /* unknown: the type is the MUIC's (0x25) to say */
	if (external) {
		b.usb_voltage_uv = (int32_t)((uint64_t)vbyp * 427246 / 1000);
		b.usb_current_ua = (int32_t)iin * 125;
	}
	redwood_battery_report(&b);
	{
		static int last_external = -1, last_status = -1;

		if (b.external != last_external || b.status != last_status) {
			m2m_klog('I', "DEV", "bms: %s, %s at %d.%02d %%: INT_OK %#x DETAILS_00 %#x DETAILS_01 %#x, input %d mV %d mA",
			    external ? "on a charger" : "no charger", status_name(b.status), b.percent_hundredths / 100,
			    b.percent_hundredths % 100, int_ok, d00, d01, (int)((uint64_t)vbyp * 427246 / 1000000),
			    (int)iin * 125 / 1000);
			last_external = b.external;
			last_status = b.status;
		}
	}

	if (say) {
		/* read only: what the bootloader left the charger at (x1slte: input 879 mA at 4.8 V does not keep up) */
		uint8_t c0 = 0, c2 = 0, c6 = 0, c9 = 0;

		if (chg_byte(CHG_CNFG_00, &c0) && chg_byte(CHG_CNFG_02, &c2) && chg_byte(CHG_CNFG_06, &c6) &&
		    chg_byte(CHG_CNFG_09, &c9)) {
			m2m_klog('I', "DEV", "bms: charger config: CNFG_00 %#x (mode %u), CNFG_02 %#x (charge %u mA), CNFG_06 %#x "
			    "(prot %u), CNFG_09 %#x (input limit %u mA); battery %d mA, input %d mV %d mA",
			    c0, c0 & 0xfU, c2, (c2 & 0x3fU) * 50, c6, (c6 >> 2) & 3U, c9, ((c9 & 0x7fU) + 1) * 25,
			    b.current_ua / 1000, b.usb_voltage_uv / 1000, b.usb_current_ua / 1000);
		} else {
			m2m_klog('W', "DEV", "bms: charger config registers did not answer");
		}
	}
	if (say || trace) {
		printf("bms: %d.%02d %% %d mV %d mA (now %d), %d.%02d C, %d/%d mAh (design %d, gauge %u), %u cycles, "
		    "%s%s, input %d mV %d mA; INT_OK %#x DTLS %#x %#x; charger %d.%d C, USB %d.%d C (ADC %u %u %u)\n",
		    b.percent_hundredths / 100, b.percent_hundredths % 100, b.voltage_uv / 1000, b.current_ua / 1000,
		    current_ua(cur) / 1000, temp_hundredths / 100, temp_hundredths % 100 < 0 ? -(temp_hundredths % 100) :
		    temp_hundredths % 100, b.charge_uah / 1000, b.full_uah / 1000, DESIGN_MAH,
		    designcap * FG_RESISTOR / 2, b.cycles, b.present ? "" : "NO BATTERY, ",
		    status_name(b.status), b.usb_voltage_uv / 1000, b.usb_current_ua / 1000, int_ok, d00, d01,
		    chg_c10_sysctl / 10, chg_c10_sysctl < 0 ? -(chg_c10_sysctl % 10) : chg_c10_sysctl % 10,
		    usb_c10_sysctl / 10, usb_c10_sysctl < 0 ? -(usb_c10_sysctl % 10) : usb_c10_sysctl % 10,
		    adc_raw[0], adc_raw[1], adc_raw[2]);
	}
	return true;
}

static void
bms_thread(__unused void *parameter, __unused wait_result_t result)
{
	char line[192];
	uint32_t secs = 5, failures = 0;
	uint64_t polls = 0;

	PE_parse_boot_argn("bms_secs", &secs, sizeof(secs));
	PE_parse_boot_argn("bms_trace", &trace, sizeof(trace));
	PE_parse_boot_argn("bms_wdt", &wdt_kick, sizeof(wdt_kick));
	PE_parse_boot_argn("bms_ilim", &ilim, sizeof(ilim));
	PE_parse_boot_argn("bms_usb_ma", &usb_ma, sizeof(usb_ma));
	if (usb_ma < 100 || usb_ma > 3000) {
		usb_ma = 1500;
	}
	PE_parse_boot_argn("bms_hv", &hv_on, sizeof(hv_on));
	if (secs < 1 || secs > 600) {
		secs = 5;
	}
	bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, BANK_GPP1);
	if (!redwood_bbi2c_setup(&bus, false, line, sizeof(line))) {
		printf("bms: %s: the bus is not usable, giving up\n", line);
		redwood_bbi2c_restore(&bus);
		return;
	}
	printf("bms: %s\n", line);

	for (;; polls++) {
		/* a line at the start, then once a minute */
		bool say = polls == 0 || (polls * secs) % 60 < secs;

		if (poll_once(say)) {
			if (failures) {
				m2m_klog('I', "DEV", "bms: the MAX77705 answers again after %u failed reads", failures);
			}
			failures = 0;
		} else {
			/*
			 * x1slte (2026-11-06): the first read at boot worked, then never again (the percentage froze at
			 * its boot value). The touch driver bit-bangs its PMIC bus on this same bank at the same time,
			 * and a transfer cut short leaves the gauge holding SDA low. Set the bus up again (DAT preset to
			 * 0, up to 9 clocks to free a stuck slave, a stop) and read again at once.
			 */
			bool ok = redwood_bbi2c_setup(&bus, false, line, sizeof(line)) && poll_once(say);

			if (failures++ < 5 || failures % 60 == 0) {
				m2m_klog(ok ? 'I' : 'W', "DEV", "bms: no answer from the MAX77705 (%u, %u stretched clocks given up on "
				    "so far); bus set up again: %s: %s", failures, bus.stretch_timeouts, line,
				    ok ? "answers again" : "still no answer");
			}
			if (ok) {
				failures = 0;
			}
		}
		delay_for_interval(secs, NSEC_PER_SEC);
	}
}

void
redwood_max77705_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("bms", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (kernel_thread_start(bms_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * The S20's main PMIC (S2MPS19) through ACPM: a register read and write, and three of its LDOs switched on, for the
 * sensors (bsd/dev/redwood_ssp.c, bsd/dev/redwood_tcs3407.c). The AP has no bus of its own to the PMIC: SPEEDY belongs to
 * the APM core, and Linux asks ACPM over its "mfd" IPC channel (drivers/soc/samsung/acpm/acpm_mfd.c).
 *
 *   channel     2: the x1slte dtbo's s2mps19mfd@00 "acpm-ipc-channel = <0x02>", speedy channel 0
 *               (drivers/mfd/s2mps19_core.c: acpm_mfd_node = that node, channel 0 in every call)
 *   command     cmd[0] = reg [7:0] | type [11:8] | speedy channel [15:12]; cmd[1] = func [7:0] | value [15:8]
 *               (include/soc/samsung/acpm_mfd.h: SHIFT_REG 0, SHIFT_TYPE 8, SHIFT_CHANNEL 12, SHIFT_FUNC 0,
 *               SHIFT_WRITE_VAL 8; FUNC_READ 0, FUNC_WRITE 1); answer cmd[1]: value [15:8], return code [31:24]
 *   type        the PMIC's block: 0 TOP, 1 PMIC (the LDOs), 2 RTC (s2mps19_core.c I2C_ADDR_*)
 *   LDOs        CTRL = enable [7:6] (0xC0 on) | vsel [5:0]; group 1 (LDO21/22/25): 700 mV + 25 mV a step
 *               (s2mps19-regulator.h S2MPS19_PMIC_REG_L21CTRL 0x5A, L22CTRL 0x5B, L25CTRL 0x5E; S2MPS19_LDO_MIN1/STEP1;
 *               s2mps19_regulator.c LDO_DESC group 1)
 *   which       LDO21M vdd_mcu_1p8 (1.8 V, the hub's "ssp-vdd-mcu-1p8", lhd's mcu_power), LDO22M VDD_NFC_1P8 (1.8 V,
 *               the TCS3407's I2C pull-ups, "als_rear,i2c_1p8"), LDO25M VDD_FLICKER_1P8 (1.6-2.2 V, the TCS3407's
 *               supply, "als_rear,vdd_1p8"); none of them always-on in exynos990-x1slte_eur_open_21.dts
 *
 * Only these three CTRL registers may be written (redwood_pmic_ldo_on()); every write is read back. The queue layout and
 * the doorbell are bsd/dev/redwood_exynos_acpm.c's (the DVFS/TMU client); this file keeps its own copy of the channel
 * lookup so as not to touch that file. Each channel has its own doorbell and status bit (1 << id), so the two never
 * see each other's answers. Better folded into redwood_exynos_acpm.c later (NOTES.md).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/systm.h>
#include <sys/redwood_sensors.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define ACPM_INTR_PHYS          0x15900000UL
#define ACPM_SRAM_PHYS          0x02039000UL
#define ACPM_SRAM_SIZE          0x38000
#define ACPM_INITDATA           0x7000
#define INTGR0                  0x08
#define INTCR1                  0x20
#define INTSR1                  0x28
#define CH_MFD                  2
#define SEQ_SHIFT               16
#define SEQ_MASK                (0x3fu << SEQ_SHIFT)
#define FUNC_READ               0
#define FUNC_WRITE              1
#define SPEEDY_CHANNEL          0

int redwood_pmic_read(uint8_t type, uint8_t reg, uint8_t *value);
int redwood_pmic_write(uint8_t type, uint8_t reg, uint8_t value);
int redwood_pmic_ldo_on(unsigned ldo, char *report, size_t report_size);

static LCK_GRP_DECLARE(pmic_lck_grp, "redwood-pmic");
static LCK_MTX_DECLARE(pmic_lock, &pmic_lck_grp);

static volatile uint8_t *intr, *sram;
static struct {
	uint32_t id, len, elem;
	volatile uint8_t *tx_front, *tx_rear, *tx_base, *rx_front, *rx_rear, *rx_base;
	uint32_t seq;
	bool ready, failed;
} ch;

static inline uint32_t
rd(volatile uint8_t *at)
{
	return *(volatile uint32_t *)at;
}

static inline void
wr(volatile uint8_t *at, uint32_t value)
{
	*(volatile uint32_t *)at = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* the descriptor at position CH_MFD (redwood_exynos_acpm.c find_channel(): Linux indexes the array by the id) */
static bool
channel(void)
{
	uint32_t channels, count;
	volatile uint8_t *d;

	if (ch.ready || ch.failed) {
		return ch.ready;
	}
	ch.failed = true;
	intr = (volatile uint8_t *)ml_io_map(ACPM_INTR_PHYS, 0x1000);
	sram = (volatile uint8_t *)ml_io_map(ACPM_SRAM_PHYS, ACPM_SRAM_SIZE);
	if (intr == NULL || sram == NULL) {
		redwood_dsay("pmic: ACPM not mapped\n");
		return false;
	}
	channels = rd(sram + ACPM_INITDATA + 8);
	count = rd(sram + ACPM_INITDATA + 24);
	if (channels == 0 || channels >= ACPM_SRAM_SIZE || count <= CH_MFD || count > 32) {
		redwood_dsay("pmic: ACPM names %u channels at %#x: no channel %u\n", count, channels, CH_MFD);
		return false;
	}
	d = sram + channels + CH_MFD * 72;
	ch.id = rd(d);
	ch.len = rd(d + 52);
	ch.elem = rd(d + 56);
	if (ch.len == 0 || ch.len > 64 || ch.elem < 16 || ch.elem > 64 || rd(d + 24) >= ACPM_SRAM_SIZE ||
	    rd(d + 48) >= ACPM_SRAM_SIZE || rd(d + 20) >= ACPM_SRAM_SIZE || rd(d + 44) >= ACPM_SRAM_SIZE) {
		redwood_dsay("pmic: ACPM channel %u: queue %u x %u, not usable\n", CH_MFD, ch.len, ch.elem);
		return false;
	}
	ch.tx_rear = sram + rd(d + 16);
	ch.tx_front = sram + rd(d + 20);
	ch.tx_base = sram + rd(d + 24);
	ch.rx_rear = sram + rd(d + 40);
	ch.rx_front = sram + rd(d + 44);
	ch.rx_base = sram + rd(d + 48);
	redwood_dsay("pmic: ACPM channel %u (id %u, type %u): %u x %u bytes\n", CH_MFD, ch.id, rd(d + 12), ch.len, ch.elem);
	ch.failed = false;
	ch.ready = true;
	return true;
}

/* one command and its answer, as redwood_exynos_acpm.c ipc() does for a queue channel */
static int
ipc(uint32_t cmd[4])
{
	uint32_t front, next, seq, i;
	uint64_t deadline;

	if (!channel()) {
		return -1;
	}
	front = rd(ch.tx_front);
	next = front + 1 == ch.len ? 0 : front + 1;
	if (front >= ch.len || next == rd(ch.tx_rear)) {
		redwood_dsay("pmic: ACPM channel %u: queue full (front %u rear %u)\n", CH_MFD, front, rd(ch.tx_rear));
		return -1;
	}
	seq = ch.seq = ch.seq >= 63 ? 1 : ch.seq + 1;
	cmd[0] = (cmd[0] & ~SEQ_MASK) | (seq << SEQ_SHIFT);
	for (i = 0; i < ch.elem / 4; i++) {
		wr(ch.tx_base + ch.elem * front + i * 4, i < 4 ? cmd[i] : 0);
	}
	wr(ch.tx_front, next);
	wr(intr + INTGR0, (1u << ch.id) << 16);
	clock_interval_to_deadline(20, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if (rd(intr + INTSR1) & (1u << ch.id)) {
			uint32_t rear = rd(ch.rx_rear), last = rd(ch.rx_front);

			for (i = rear; i != last && i < ch.len; i = i + 1 == ch.len ? 0 : i + 1) {
				volatile uint8_t *e = ch.rx_base + ch.elem * i;

				if (((rd(e) & SEQ_MASK) >> SEQ_SHIFT) != seq) {
					continue;
				}
				for (uint32_t w = 0; w < 4; w++) {
					cmd[w] = rd(e + w * 4);
				}
				if (i != rear) {
					for (uint32_t w = 0; w < ch.elem / 4; w++) {
						wr(e + w * 4, rd(ch.rx_base + ch.elem * rear + w * 4));
					}
				}
				rear = rear + 1 == ch.len ? 0 : rear + 1;
				wr(ch.rx_rear, rear);
				if (rear == rd(ch.rx_front)) {
					wr(intr + INTCR1, 1u << ch.id);
				}
				return 0;
			}
		}
		delay(10);
	}
	redwood_dsay("pmic: ACPM channel %u: no answer to %#x %#x in 20 ms\n", CH_MFD, cmd[0], cmd[1]);
	return -1;
}

/*
 * The sub-PMIC (S2MPS22, the dtbo's s2mps22mfd@00 on speedy1: acpm-ipc-channel 2, acpm-speedy-channel 1; Samsung's
 * s2mps22_core.c channel 1, I2C_ADDR_PMIC 1). Its LDO4S (CTRL 0x2D, s2mps22-regulator.h) feeds both CS35L41s' VA.
 */
int redwood_spmic_read(uint8_t type, uint8_t reg, uint8_t *value);
int redwood_spmic_write(uint8_t type, uint8_t reg, uint8_t value);
int
redwood_spmic_read(uint8_t type, uint8_t reg, uint8_t *value)
{
	uint32_t cmd[4] = { reg | (uint32_t)(type & 0xf) << 8 | 1U << 12, FUNC_READ, 0, 0 };
	int r;

	lck_mtx_lock(&pmic_lock);
	r = ipc(cmd);
	lck_mtx_unlock(&pmic_lock);
	if (r != 0) {
		return -1;
	}
	*value = (uint8_t)(cmd[1] >> 8);
	return (int)((cmd[1] >> 24) & 0xff);
}

int
redwood_spmic_write(uint8_t type, uint8_t reg, uint8_t value)
{
	uint32_t cmd[4] = { reg | (uint32_t)(type & 0xf) << 8 | 1U << 12, FUNC_WRITE | (uint32_t)value << 8, 0, 0 };
	int r;

	lck_mtx_lock(&pmic_lock);
	r = ipc(cmd);
	lck_mtx_unlock(&pmic_lock);
	return r != 0 ? -1 : (int)((cmd[1] >> 24) & 0xff);
}

int
redwood_pmic_read(uint8_t type, uint8_t reg, uint8_t *value)
{
	uint32_t cmd[4] = { reg | (uint32_t)(type & 0xf) << 8 | (uint32_t)SPEEDY_CHANNEL << 12, FUNC_READ, 0, 0 };
	int r;

	lck_mtx_lock(&pmic_lock);
	r = ipc(cmd);
	lck_mtx_unlock(&pmic_lock);
	if (r != 0) {
		return -1;
	}
	*value = (uint8_t)(cmd[1] >> 8);
	return (int)((cmd[1] >> 24) & 0xff);      /* SPEEDY's own result: 0 is good */
}

int
redwood_pmic_write(uint8_t type, uint8_t reg, uint8_t value)
{
	uint32_t cmd[4] = { reg | (uint32_t)(type & 0xf) << 8 | (uint32_t)SPEEDY_CHANNEL << 12,
		            FUNC_WRITE | (uint32_t)value << 8, 0, 0 };
	int r;

	lck_mtx_lock(&pmic_lock);
	r = ipc(cmd);
	lck_mtx_unlock(&pmic_lock);
	return r != 0 ? -1 : (int)((cmd[1] >> 24) & 0xff);
}

#define PMIC_TYPE_PMIC          1
#define LDO_ON                  0xC0

static const struct {
	unsigned ldo;
	uint8_t reg;
	uint8_t vmin, vmax, vset;       /* vsel: 700 mV + 25 mV a step */
	const char *name;
} ldos[] = {
	{ 19, 0x58, 0x2C, 0x2C, 0x2C, "VDD_BTP_1P8" },          /* 1.8 V only: the fingerprint sensor (qbt2000) */
	{ 21, 0x5A, 0x2C, 0x2C, 0x2C, "vdd_mcu_1p8" },          /* 1.8 V only */
	{ 22, 0x5B, 0x2C, 0x2C, 0x2C, "VDD_NFC_1P8" },          /* 1.8 V only */
	{ 25, 0x5E, 0x24, 0x3C, 0x2C, "VDD_FLICKER_1P8" },      /* 1.6-2.2 V; 1.8 V if outside */
};

/*
 * LDOn on at a voltage its constraints allow; 0 when it reads back on, else -1. `report` gets one line.
 * A voltage already within range is kept; only then is the enable field written.
 */
int
redwood_pmic_ldo_on(unsigned ldo, char *report, size_t report_size)
{
	uint8_t before = 0, after = 0, want;
	unsigned i;
	int r;

	for (i = 0; i < sizeof(ldos) / sizeof(ldos[0]) && ldos[i].ldo != ldo; i++) {
	}
	if (i == sizeof(ldos) / sizeof(ldos[0])) {
		snprintf(report, report_size, "LDO%uM is not one this may switch", ldo);
		return -1;
	}
	if ((r = redwood_pmic_read(PMIC_TYPE_PMIC, ldos[i].reg, &before)) != 0) {
		snprintf(report, report_size, "LDO%uM (%s) CTRL %#x not read (%d)", ldo, ldos[i].name, ldos[i].reg, r);
		return -1;
	}
	want = before;
	if ((want & 0x3f) < ldos[i].vmin || (want & 0x3f) > ldos[i].vmax) {
		want = (uint8_t)((want & ~0x3f) | ldos[i].vset);
	}
	want |= LDO_ON;
	if (want != before && (redwood_pmic_write(PMIC_TYPE_PMIC, ldos[i].reg, want) != 0 ||
	    redwood_pmic_read(PMIC_TYPE_PMIC, ldos[i].reg, &after) != 0)) {
		snprintf(report, report_size, "LDO%uM (%s) CTRL %#04x -> %#04x: the write failed", ldo, ldos[i].name, before,
		    want);
		return -1;
	}
	if (want == before) {
		after = before;
	}
	snprintf(report, report_size, "LDO%uM (%s) CTRL %#04x%s%#04x, %u mV, %s", ldo, ldos[i].name, before,
	    want == before ? " kept " : " -> ", after, 700 + 25 * (after & 0x3f), (after & LDO_ON) == LDO_ON ? "on" : "NOT on");
	return after == want ? 0 : -1;
}

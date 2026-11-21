/*
 * The Galaxy S20's (x1slte, SM-G980F) flashlight: the rear flash LED of the S2MPB02 (camera PMIC with a flash driver),
 * at I2C 0x59 on HSI2C_27 (gpp3-2 SCL, gpp3-3 SDA; bit-banged here, bsd/dev/redwood_bbi2c.c). From the phone's dtbo
 * (fragment@5, s2mpb02_pmic@59: torch table 3 5 5 6 6 8 8 8 10 10; torch1-gpio gpp4-4, flash1-gpio gpp4-0) and Samsung's
 * drivers/leds/leds-s2mpb02.c with include/linux/mfd/s2mpb02-regulator.h, include/linux/leds-s2mpb02.h (ExtremeXT):
 *
 *   0x12 FLED_CTRL1   bits 7:6 enable/mode: 00 off (or the GPIOs'), 10 flash by I2C, 11 torch by I2C; bit 3 LV_EN
 *   0x14 FLED_CUR1    bits 3:0 the torch's current, 20 mA a step; bits 7:4 the flash's (not used here)
 *   0x15 FLED_TIME1   bits 3:0 the torch's timer (left as the boot set it)
 * Samsung's "I2C turn way": CUR1's torch nibble, then CTRL1 7:6 = 11; off: CTRL1 7:6 = 00, the GPIOs low.
 *
 * iOS's side is the POCO's (bsd/dev/redwood_torch.c's contract): /dev/redwood-torch takes a level 0..1000, a read gives
 * what is lit. Control Center's button reaches it through mediaserverd (userland/src/inject_media.c); the loader tells
 * MobileGestalt there is a flash when torch=N is on the command line. Boot-args: torch=3 torch_chip=s2mpb02 (the POCO's
 * Qualcomm SPMI code is never run on this phone), torch_max_s (default 600).
 *
 * Limits: Samsung's own top step (10: 200 mA); on for torch_max_s at a time at most, then off until iOS asks anew; the
 * battery at 45 C or more (the MAX77705's gauge, bsd/dev/redwood_max77705.c): off. Every write read back; a write that
 * does not read back ends the light for the run. Off at the run's end and on a panic's way out.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_dbgport.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include "redwood_camera_s20_aux_power.h"
#include "redwood_camera_s20_gw2_power.h"

#define BANK_GPP3               0x60U           /* PERIC0 */
#define PMIC_ADDR               0x59U
#define REG_ID                  0x00U
#define REG_FLED_CTRL1          0x12U
#define REG_FLED_CUR1           0x14U
#define REG_FLED_TIME1          0x15U
#define CTRL1_MODE_MASK         0xC0U
#define CTRL1_TORCH_ON          0xC0U
#define CTRL1_LV_EN             0x08U
#define HOTTEST_BATTERY_C       45
#define REG_BUCK2_CTRL1         0x0dU
#define REG_LDO1_CTRL           0x1eU
#define REG_LDO5_CTRL           0x22U
#define REG_LDO6_CTRL           0x23U
#define REG_LDO11_CTRL          0x28U
#define REG_LDO12_CTRL          0x29U
#define REG_LDO14_CTRL          0x2bU
#define REG_LDO16_CTRL          0x2dU

static const uint8_t torch_table[10] = { 3, 5, 5, 6, 6, 8, 8, 8, 10, 10 };     /* the dtbo's, 20 mA a step */

static struct redwood_bbi2c bus = { .name = "s2mpb02 bus gpp3-2/3", .scl = 2, .sda = 3, .half_us = 4 };
static LCK_GRP_DECLARE(s2mpb02_lck_grp, "redwood-s2mpb02");
static LCK_MTX_DECLARE(s2mpb02_lock, &s2mpb02_lck_grp);
static volatile uint32_t wanted, shown;
static uint32_t max_secs = 600;
static volatile bool usable, lit, broken, bus_ready;

static bool
get_unlocked(uint8_t reg, uint8_t *v)
{
	return redwood_bbi2c_read_reg(&bus, PMIC_ADDR, reg, v, 1);
}

static bool
get(uint8_t reg, uint8_t *v)
{
	bool ok;

	lck_mtx_lock(&s2mpb02_lock);
	ok = get_unlocked(reg, v);
	lck_mtx_unlock(&s2mpb02_lock);
	return ok;
}

/* only the two registers above, and only the bits named; read back */
static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t before = 0, after = 0, want;

	if (reg != REG_FLED_CTRL1 && reg != REG_FLED_CUR1) {
		return false;
	}
	lck_mtx_lock(&s2mpb02_lock);
	if (broken || !get_unlocked(reg, &before)) {
		lck_mtx_unlock(&s2mpb02_lock);
		return false;
	}
	want = (uint8_t)((before & ~mask) | (value & mask));
	if (want == before) {
		lck_mtx_unlock(&s2mpb02_lock);
		return true;
	}
	if (!redwood_bbi2c_write_reg(&bus, PMIC_ADDR, reg, want) || !get_unlocked(reg, &after) || after != want) {
		printf("torch: s2mpb02 %#04x %#04x -> %#04x read back %#04x: no more writes this run\n", reg, before, want, after);
		broken = true;
		lck_mtx_unlock(&s2mpb02_lock);
		return false;
	}
	lck_mtx_unlock(&s2mpb02_lock);
	return true;
}

/* The S20 rear main module also uses these shared rails. Permit only their enable bits and BUCK2's
 * two enable bits; voltage selectors and every other PMIC register remain outside this interface. */
bool
redwood_s2mpb02_camera_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t before = 0, after = 0, want;
	/* 0x0b BUCK1 (1.3 V), 0x0f buck-boost (3.5 V), 0x2f LDO18 (3.3 V): always-on/boot-on in Samsung's x1slte DT,
	 * off after S-Boot; they feed the camera LDOs (B, 2026-11-11). */
	uint8_t allowed = (reg == REG_BUCK2_CTRL1 || reg == 0x0b || reg == 0x0f) ? 0xc0 : reg == 0x2f ? 0x80 :
	    (reg == REG_LDO1_CTRL || reg == REG_LDO5_CTRL || reg == REG_LDO6_CTRL ||
	    REG_LDO11_CTRL == reg || REG_LDO12_CTRL == reg || REG_LDO14_CTRL == reg || REG_LDO16_CTRL == reg) ? 0x80 : 0;
	bool ok = false;

	if (allowed == 0 || mask != allowed || (value & ~mask) != 0) return false;
	lck_mtx_lock(&s2mpb02_lock);
	if (!bus_ready || broken || !get_unlocked(reg, &before)) goto done;
	want = (uint8_t)((before & ~mask) | value);
	if (want == before) {
		ok = true;
		goto done;
	}
	if (redwood_bbi2c_write_reg(&bus, PMIC_ADDR, reg, want) && get_unlocked(reg, &after) &&
	    (after & mask) == (want & mask)) {
		ok = true;
	} else {
		printf("camera: S2MPB02 reg %#x enable update failed (was %#x, wanted %#x, read %#x)\n", reg, before, want, after);
	}
done:
	lck_mtx_unlock(&s2mpb02_lock);
	return ok;
}

/* Private 2LA adapter: fixed register allowlist lives in the portable plan.
 * Hold the torch/PMIC lock for the complete transaction, including rollback. */
static bool aux_read(void *ctx, uint8_t reg, uint8_t *value) {
    (void)ctx;return get_unlocked(reg,value);
}
static bool aux_write(void *ctx, uint8_t reg, uint8_t value) {
    (void)ctx;return redwood_bbi2c_write_reg(&bus,PMIC_ADDR,reg,value);
}
bool redwood_s2mpb02_2la_power(struct cam_aux_power *saved, bool restore, bool *clean) {
    struct cam_aux_power_io io={NULL,aux_read,aux_write};
    bool ok=false;*clean=true;
    lck_mtx_lock(&s2mpb02_lock);
    if(restore){ok=cam_aux_power_restore(saved,io);*clean=ok;}
    else if(bus_ready&&!broken){
        ok=cam_aux_power_begin(saved,io);
        if(!ok)*clean=cam_aux_power_restore(saved,io);
    }
    if(!*clean)broken=true;
    lck_mtx_unlock(&s2mpb02_lock);return ok;
}

struct gw2_adapter {
    void *ctx;
    bool (*read03)(void *,uint8_t,uint8_t *);
    bool (*write03)(void *,uint8_t,uint8_t);
};
static bool gw2_read(void *p,uint8_t pmic,uint8_t reg,uint8_t *v){
    struct gw2_adapter *a=p;return pmic==2?get_unlocked(reg,v):a->read03(a->ctx,reg,v);
}
static bool gw2_write(void *p,uint8_t pmic,uint8_t reg,uint8_t v){
    struct gw2_adapter *a=p;return pmic==2?redwood_bbi2c_write_reg(&bus,PMIC_ADDR,reg,v):a->write03(a->ctx,reg,v);
}
static void gw2_pause(void *p,unsigned us){(void)p;delay_for_interval(us,NSEC_PER_USEC);}
bool redwood_s2mpb02_gw2_power(struct cam_gw2_power *saved,bool restore,bool *clean,void *ctx,
    bool (*read03)(void *,uint8_t,uint8_t *),bool (*write03)(void *,uint8_t,uint8_t)){
    struct gw2_adapter adapter={ctx,read03,write03};
    struct cam_gw2_io io={&adapter,gw2_read,gw2_write,gw2_pause};
    bool ok=false;*clean=true;
    lck_mtx_lock(&s2mpb02_lock);
    if(restore){ok=cam_gw2_power_restore(saved,io);*clean=ok;}
    else if(bus_ready&&!broken){ok=cam_gw2_power_begin(saved,io);if(!ok)*clean=cam_gw2_power_restore(saved,io);}
    if(!*clean)broken=true;
    lck_mtx_unlock(&s2mpb02_lock);return ok;
}

#include "redwood_camera_s20_main_voltage.h"
bool redwood_s2mpb02_main_voltages(struct cam_main_voltage *saved,bool restore,bool *clean,
    void *ctx,bool(*read03)(void *,uint8_t,uint8_t *),bool(*write03)(void *,uint8_t,uint8_t)) {
    struct gw2_adapter adapter={ctx,read03,write03};
    struct cam_main_voltage_io io={&adapter,gw2_read,gw2_write};
    bool ok=false;*clean=true;
    lck_mtx_lock(&s2mpb02_lock);
    if(restore){ok=cam_main_voltage_restore(saved,io);*clean=ok;}
    else if(bus_ready&&!broken){ok=cam_main_voltage_begin(saved,io);if(!ok)*clean=cam_main_voltage_restore(saved,io);}
    if(!*clean)broken=true;
    lck_mtx_unlock(&s2mpb02_lock);return ok;
}

/* x1slte dtbo.0: VT core 1.05 V, IO 1.8 V, analog 2.95 V, AF 2.8 V.
 * LDO5 uses 12.5 mV steps from 0.6 V; the other three use 25 mV steps.
 * A cold PMIC reset leaves core at 1.2 V and analog at 2.8 V. Save/restore
 * selectors independently of enable bits; serialize with the flashlight. */
bool
redwood_s2mpb02_front_voltages(uint8_t saved[4], bool restore)
{
	static const uint8_t regs[4] = { REG_LDO5_CTRL, REG_LDO6_CTRL, REG_LDO11_CTRL, REG_LDO16_CTRL };
	static const uint8_t target[4] = { 0x24, 0x30, 0x5e, 0x58 };
	uint8_t before[4], after, want;
	bool ok = false;
	unsigned i;
	if (!saved) return false;
	lck_mtx_lock(&s2mpb02_lock);
	if (!bus_ready || broken) goto done;
	for (i = 0; i < 4; i++) if (!get_unlocked(regs[i], &before[i])) goto done;
	if (!restore) for (i = 0; i < 4; i++) saved[i] = before[i] & 0x7f;
	ok = true;
	for (i = 0; i < 4; i++) {
		want = (before[i] & 0x80) | ((restore ? saved[i] : target[i]) & 0x7f);
		if (want != before[i] && (!redwood_bbi2c_write_reg(&bus, PMIC_ADDR, regs[i], want) ||
		    !get_unlocked(regs[i], &after) || after != want)) {
			ok = false;
			if (!restore) {
				for (unsigned j = 0; j <= i; j++) {
					if (!redwood_bbi2c_write_reg(&bus, PMIC_ADDR, regs[j], before[j]) ||
					    !get_unlocked(regs[j], &after) || after != before[j]) broken = true;
				}
				break;
			}
		}
	}
done:
	lck_mtx_unlock(&s2mpb02_lock);
	return ok;
}

bool
redwood_s2mpb02_camera_read(uint8_t reg, uint8_t *value)
{
	bool ok;
	if (value == NULL || (reg != REG_BUCK2_CTRL1 && reg != 0x0b && reg != 0x0f && reg != 0x2f && reg != REG_LDO1_CTRL && reg != REG_LDO5_CTRL &&
	    reg != REG_LDO6_CTRL && reg != REG_LDO11_CTRL && reg != REG_LDO12_CTRL && reg != REG_LDO14_CTRL && reg != REG_LDO16_CTRL)) return false;
	lck_mtx_lock(&s2mpb02_lock);
	ok = bus_ready && !broken && get_unlocked(reg, value);
	lck_mtx_unlock(&s2mpb02_lock);
	return ok;
}

static void
camera_pmic_snapshot(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	static const uint8_t regs[] = { REG_ID, REG_BUCK2_CTRL1, REG_LDO5_CTRL, REG_LDO6_CTRL, REG_LDO11_CTRL, REG_LDO12_CTRL, REG_LDO16_CTRL };
	static const char *names[] = { "id", "buck2", "ldo5_vt_core", "ldo6_vt_io", "ldo11_vt_analog", "ldo12_front", "ldo16_vt_af" };
	uint8_t values[sizeof(regs)] = { 0 };
	bool ok = true;
	size_t used;

	if (cap == 0) return;
	if (!bus_ready) {
		(void)snprintf(out, cap, "camera-s2mpb02: bus-not-ready; read-only; no writes\n");
		return;
	}
	lck_mtx_lock(&s2mpb02_lock);
	for (unsigned i = 0; i < sizeof(regs); i++) {
		if (!get_unlocked(regs[i], &values[i])) ok = false;
	}
	lck_mtx_unlock(&s2mpb02_lock);
	used = (size_t)snprintf(out, cap, "{\"device\":\"S2MPB02\",\"address\":\"0x59\",\"read_only\":true,\"result\":\"%s\"", ok ? "ok" : "i2c_error");
	for (unsigned i = 0; i < sizeof(regs) && used < cap; i++) {
		if (i < 2) {
			used += (size_t)snprintf(out + used, cap - used, ",\"%s\":{\"reg\":\"0x%02x\",\"value\":\"0x%02x\"}", names[i], regs[i], values[i]);
		} else {
			used += (size_t)snprintf(out + used, cap - used, ",\"%s\":{\"reg\":\"0x%02x\",\"value\":\"0x%02x\",\"enabled\":%s}",
			    names[i], regs[i], values[i], (values[i] & 0x80) ? "true" : "false");
		}
	}
	if (used < cap) used += (size_t)snprintf(out + used, cap - used, "}\n");
	/* Second line (B, 2026-11-11): the whole map 0x00..0x2f, read-only, for comparison with Samsung's dump
	 * (BUCK1 0x0b/0x0c, BUCK2 0x0d/0x0e, buck-boost 0x0f/0x10, LDO1..18 0x1e..0x2f). */
	if (n > 0 && v[0] == 1 && used < cap) {
		used += (size_t)snprintf(out + used, cap - used, "map:");
		lck_mtx_lock(&s2mpb02_lock);
		for (unsigned r = 0; r <= 0x2f && used + 8 < cap; r++) {
			uint8_t val = 0;
			bool got = get_unlocked((uint8_t)r, &val);
			used += (size_t)snprintf(out + used, cap - used, got ? " %02x=%02x" : " %02x=ERR", r, val);
		}
		lck_mtx_unlock(&s2mpb02_lock);
		if (used < cap) (void)snprintf(out + used, cap - used, "\n");
	}
}

static bool
dark(void)
{
	bool ok = put_bits(REG_FLED_CTRL1, CTRL1_MODE_MASK, 0);

	lit = !ok && lit;
	return ok;
}

static bool
light(uint32_t permille)
{
	unsigned i = (permille * 9 + 500) / 1000;
	uint8_t step = torch_table[i > 9 ? 9 : i];

	if (!put_bits(REG_FLED_CUR1, 0x0F, step) || !put_bits(REG_FLED_CTRL1, CTRL1_MODE_MASK, CTRL1_TORCH_ON)) {
		(void)dark();
		return false;
	}
	lit = true;
	return true;
}

static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value;
	int error;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(value)) {
		if ((error = uiomove((caddr_t)&value, sizeof(value), uio)) != 0) {
			return error;
		}
		if (value != 0 && redwood_battery_temperature() >= HOTTEST_BATTERY_C) {
			return EBUSY;
		}
		wanted = value > 1000 ? 1000 : value;
		thread_wakeup((event_t)&wanted);        /* the thread sleeps while the light is off (torch_thread) */
	}
	return 0;
}

static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = shown;

	return uiomove((caddr_t)&value, (int)MIN(sizeof(value), (size_t)uio_resid(uio)), uio);
}

static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw torch_cdevsw = {
	.d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
torch_thread(__unused void *parameter, __unused wait_result_t result)
{
	char line[192];
	uint8_t id = 0, c1 = 0, cur = 0, t1 = 0;
	uint32_t applied = 0, times = 0;
	uint64_t since = 0;
	bool hold_off = false;
	int major;

	bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, BANK_GPP3);
	if (!redwood_bbi2c_setup(&bus, false, line, sizeof(line)) &&
	    !redwood_bbi2c_setup(&bus, true, line, sizeof(line))) {
		printf("torch: %s: the bus is not usable\n", line);
		redwood_bbi2c_restore(&bus);
		return;
	}
	bus_ready = true;
	(void)redwood_dbgport_add("cam.pmic", ": read-only S2MPB02 camera-rail register snapshot", camera_pmic_snapshot);
	if (!get(REG_ID, &id) || !get(REG_FLED_CTRL1, &c1) || !get(REG_FLED_CUR1, &cur) || !get(REG_FLED_TIME1, &t1)) {
		printf("torch: %s; the S2MPB02 does not answer at %#x\n", line, PMIC_ADDR);
		redwood_bbi2c_restore(&bus);
		return;
	}
	/* Samsung's x1slte DT keeps S2MPB02 BUCK1 (1.3 V), the buck-boost (3.5 V) and LDO18 (3.3 V) always-on/boot-on;
	 * S-Boot leaves them off. Without them the front SAK3J1 answers I2C but never starts its MIPI link (proven
	 * 2026-11-11 on Samsung's kernel: same sequence, supplies off -> 0 frames, on -> 15). On from boot, like Samsung,
	 * so they are settled long before a camera opens. cam_pmic_base=0 leaves them as the bootloader did. */
	{
		uint32_t base = 1;
		(void)PE_parse_boot_argn("cam_pmic_base", &base, sizeof(base));
		if (base) {
			/* The voltages first, as Samsung's regulator init does from its DT (min = max): BUCK1 and BUCK2
			 * 1.3 V (0x90: 0.4 V + 144 * 6.25 mV), buck-boost 3.5 V (0x48: 2.6 V + 72 * 12.5 mV). After a hard
			 * reset the chip has BUCK1 at 0xc0 (1.6 V) and BUCK2 at 0x40 (0.8 V): with 0.8 V in, the sensor's
			 * 1.05 V core LDO cannot regulate -- the camera worked only while Samsung's values survived warm
			 * restarts (seen 2026-11-11 after a forced power-off). */
			static const uint8_t volt[3][2] = { { 0x0c, 0x90 }, { 0x0e, 0x90 }, { 0x10, 0x48 } };
			for (unsigned k = 0; k < 3; k++) {
				uint8_t was = 0, now = 0;
				lck_mtx_lock(&s2mpb02_lock);
				bool rd = get_unlocked(volt[k][0], &was);
				bool wr = rd && (was == volt[k][1] || redwood_bbi2c_write_reg(&bus, PMIC_ADDR, volt[k][0], volt[k][1]));
				bool ok2 = wr && get_unlocked(volt[k][0], &now) && now == volt[k][1];
				lck_mtx_unlock(&s2mpb02_lock);
				printf("camera: S2MPB02 reg %#x voltage %#x -> %#x %s\n", volt[k][0], was, now, ok2 ? "ok" : "FAILED");
			}
			delay_for_interval(2, NSEC_PER_MSEC);
			bool b1 = redwood_s2mpb02_camera_bits(0x0b, 0xc0, 0xc0);
			delay_for_interval(2, NSEC_PER_MSEC);
			bool bb = redwood_s2mpb02_camera_bits(0x0f, 0xc0, 0xc0);
			delay_for_interval(2, NSEC_PER_MSEC);
			bool l18 = redwood_s2mpb02_camera_bits(0x2f, 0x80, 0x80);
			printf("camera: S2MPB02 base supplies BUCK1 %s, buck-boost %s, LDO18 %s\n",
			    b1 ? "on" : "FAILED", bb ? "on" : "FAILED", l18 ? "on" : "FAILED");
		}
	}
	printf("torch: %s; S2MPB02 id %#04x, FLED_CTRL1 %#04x CUR1 %#04x TIME1 %#04x\n", line, id, c1, cur, t1);
	/* Samsung's setup: low-voltage mode off, the light off */
	if (!put_bits(REG_FLED_CTRL1, CTRL1_LV_EN | CTRL1_MODE_MASK, 0)) {
		return;
	}
	usable = true;
	major = cdevsw_add(-1, &torch_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-torch") == NULL) {
		printf("torch: /dev/redwood-torch cannot be made\n");
		return;
	}
	printf("torch: the S20's flashlight is iOS's (/dev/redwood-torch): at most 200 mA, %u s at a time\n", max_secs);
	while (!broken) {
		uint32_t want = wanted;
		uint64_t now = now_ms();
		int temp = redwood_battery_temperature();

		if (want == 0) {
			hold_off = false;
		}
		if (want != 0 && temp >= HOTTEST_BATTERY_C) {
			hold_off = true;
		}
		if (hold_off) {
			want = 0;
		}
		if (want != applied) {
			if (!(want == 0 ? dark() : light(want))) {
				break;
			}
			if (applied == 0) {
				since = now;
				times++;
			}
			printf("torch: %s (iOS asked for %u of 1000; battery %d C)\n", want ? "on" : "off", wanted, temp);
			applied = want;
			shown = want;
		}
		if (applied != 0 && now - since >= (uint64_t)max_secs * 1000) {
			printf("torch: on for %u s, the limit: off until iOS asks again\n", max_secs);
			hold_off = true;
			continue;
		}
		if (applied == 0 && wanted == 0) {
			/* (A, 2026-11-15) off: sleep until iOS asks (torch_write wakes us), not a 30 ms poll -- it woke a big
			 * core ~33 times a second with the screen off. The 10 s timeout is only a safety net. */
			assert_wait_timeout((event_t)&wanted, THREAD_UNINT, 10000, NSEC_PER_MSEC);
			if (wanted != 0) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
			continue;
		}
		delay_for_interval(30, NSEC_PER_MSEC);
	}
	(void)dark();
	shown = 0;
	printf("torch: given up on (%u times lit)\n", times);
}

void redwood_s2mpb02_torch_init(void);

void
redwood_s2mpb02_torch_init(void)
{
	thread_t thread;

	PE_parse_boot_argn("torch_max_s", &max_secs, sizeof(max_secs));
	max_secs = max_secs < 5 ? 5 : max_secs > 3600 ? 3600 : max_secs;
	if (kernel_thread_start(torch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* the run's end, a panic's way out: dark */
void redwood_s2mpb02_torch_quiesce(void);

void
redwood_s2mpb02_torch_quiesce(void)
{
	if (usable && lit) {
		broken = false;
		printf("torch: at the run's end: the light %s\n", dark() ? "switched off" : "COULD NOT BE SWITCHED OFF");
	}
}

/*
 * The Galaxy S20+'s (y2s, Exynos 990) power and volume keys.
 *
 * All three are plain GPIOs of the ALIVE pinctrl, low while pressed (the phone's device tree,
 * gpio_keys, and the UEFI's y2sPkg KeypadDeviceLib, which reads them the same way):
 *
 *   volume up    gpa0-3        volume down  gpa0-4        power  gpa2-4
 *
 * They are polled every 16 ms and reported as redwood_touch_post(REDWOOD_BUTTON_DOWN/UP, ...),
 * which backboardd turns into the HID events of the real keys (userland inject.c).
 *
 * A key is believed only after it has been seen released once (a key held while the system
 * starts is not a press), and only a state seen twice running counts (debounce).
 *
 * Volume down + power held together for 3 s hands control back to the loader, which saves the
 * log -- a clean end of a run, a few seconds before the PMIC's own reset (about 7 s) would
 * cut the power.
 *
 * Boot args: keys=1 on (default: follows touch_ts), keys=0 off, keys_escape=0 no escape.
 *            keys_irq=0: poll only. Default 1 (B, 2026-11-06): each key's pin is an ALIVE wake-up EINT (both edges;
 *            dts gpa0/gpa2 interrupts: gpa0-3 SPI 3, gpa0-4 SPI 4, gpa2-4 SPI 20), which wakes this thread; it then
 *            polls every 16 ms while a key is down and for 100 ms after an interrupt (the debounce), and sleeps
 *            otherwise, with a 500 ms fallback look once interrupts are seen. A storm (> 500/s) masks them: polling.
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern void loader_return(void);
extern void redwood_var_save_on_return(void);  /* bsd/dev/redwood_ufs.c */
extern void redwood_sync_filesystems(void);     /* bsd/dev/redwood_ufs.c */

#define POLL_MS         16
#define ESCAPE_MS       3000
#define BANK_GPA0       0x00U
#define BANK_GPA2       0x40U

static struct key {
	const char         *name;
	uint32_t            bank_offset, pin;
	uint32_t            record;            /* REDWOOD_BUTTON_* */
	volatile uint8_t   *bank;
	bool                live;              /* seen released: believed from then on */
	bool                last;              /* the look before this one */
	bool                down;              /* as reported */
	uint64_t            since;
	uint32_t            presses;
	uint32_t            eint_off, intid;       /* wake-up EINT: CON 0x700 / MASK 0x900 / PEND 0xA00 + eint_off; GIC */
} keys[] = {
	{ .name = "power", .bank_offset = BANK_GPA2, .pin = 4, .record = REDWOOD_BUTTON_LOCK, .eint_off = 0x08, .intid = 32 + 20 },
	{ .name = "volume up", .bank_offset = BANK_GPA0, .pin = 3, .record = REDWOOD_BUTTON_VOLUME_UP, .eint_off = 0x00, .intid = 32 + 3 },
	{ .name = "volume down", .bank_offset = BANK_GPA0, .pin = 4, .record = REDWOOD_BUTTON_VOLUME_DOWN, .eint_off = 0x00, .intid = 32 + 4 },
};

#define EINT_EDGE_BOTH  4U
#define NKEYS           (sizeof(keys) / sizeof(keys[0]))
static volatile uint8_t *keys_alive;
static volatile bool keys_irq_on;
static volatile uint32_t keys_irqs, keys_storm, keys_window;
static uint64_t keys_window_at, keys_irq_at;
static int keys_wake;
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid),
    void *context);

static void
keys_mask_all(void)
{
	for (unsigned i = 0; i < NKEYS; i++) {
		*(volatile uint32_t *)(keys_alive + 0x900 + keys[i].eint_off) |= 1u << keys[i].pin;
	}
}

static void
keys_irq(void *context, __unused uint32_t intid)
{
	struct key *k = context;
	uint64_t now = mach_absolute_time(), second;

	*(volatile uint32_t *)(keys_alive + 0xA00 + k->eint_off) = 1u << k->pin;       /* write 1 to clear */
	keys_irqs++;
	keys_irq_at = now;
	nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
	if (now - keys_window_at > second) {
		keys_window_at = now;
		keys_window = 0;
	}
	if (++keys_window > 500) {
		keys_mask_all();
		keys_irq_on = false;
		keys_storm++;
	}
	thread_wakeup((event_t)&keys_wake);
}

static void
keys_irq_setup(void)
{
	unsigned attached = 0;

	keys_alive = keys[0].bank - keys[0].bank_offset;
	keys_mask_all();
	for (unsigned i = 0; i < NKEYS; i++) {
		volatile uint32_t *con = (volatile uint32_t *)(keys_alive + 0x700 + keys[i].eint_off);

		*con = (*con & ~(0x7u << (keys[i].pin * 4))) | (EINT_EDGE_BOTH << (keys[i].pin * 4));
		*(volatile uint32_t *)(keys_alive + 0xA00 + keys[i].eint_off) = 1u << keys[i].pin;
	}
	for (unsigned i = 0; i < NKEYS; i++) {
		redwood_gpio_func(keys[i].bank, keys[i].pin, 0xf);     /* EXT_INT (the level still reads in DAT) */
		if (gic_v3_spi_attach(keys[i].intid, false, keys_irq, &keys[i]) == KERN_SUCCESS) {
			attached++;
		} else {
			redwood_gpio_func(keys[i].bank, keys[i].pin, REDWOOD_GPIO_INPUT);
		}
	}
	if (attached != NKEYS) {
		printf("keys: %u of %lu interrupts attached: polling only\n", attached, NKEYS);
		return;
	}
	keys_irq_on = true;
	for (unsigned i = 0; i < NKEYS; i++) {
		*(volatile uint32_t *)(keys_alive + 0x900 + keys[i].eint_off) &= ~(1u << keys[i].pin);
	}
	printf("keys: by interrupt (both edges; INTID %u, %u, %u)\n", keys[0].intid, keys[1].intid, keys[2].intid);
}

#define KEY_POWER       0
#define KEY_VOLUME_DOWN 2


static void
key_look(struct key *k, bool pressed)
{
	uint64_t now = mach_absolute_time(), held;
	bool steady = pressed == k->last;

	k->last = pressed;
	if (!steady) {
		return;
	}
	if (!k->live) {
		if (!pressed) {
			k->live = true;
			printf("keys: %s is released: believed from here on\n", k->name);
		}
		return;
	}
	if (pressed == k->down) {
		return;
	}
	k->down = pressed;
	if (pressed) {
		k->since = now;
		k->presses++;
		printf("keys: %s pressed\n", k->name);
	} else {
		absolutetime_to_nanoseconds(now - k->since, &held);
		printf("keys: %s released after %llu ms\n", k->name, held / NSEC_PER_MSEC);
	}
	/*
	 * (no clock boost here: on a lock press it changed every cluster's and the GPU's clock just as the lock screen's
	 * frames were drawn -- 33-54 ms frames seen between press and release; a wake is boosted by the display coming on,
	 * 6 ms after the press, in MiniIOMobileFramebuffer::displayPower)
	 */
	redwood_touch_post(pressed ? REDWOOD_BUTTON_DOWN : REDWOOD_BUTTON_UP, k->record, 0, 0);
}

static void
keys_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t escape = 1, i;
	uint64_t both_since = 0, ms;

	PE_parse_boot_argn("keys_escape", &escape, sizeof(escape));
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		keys[i].bank = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, keys[i].bank_offset);
		keys[i].last = redwood_gpio_get(keys[i].bank, keys[i].pin) == 0;
	}
	printf("keys: power %s, volume up %s, volume down %s (polled every %u ms)\n",
	    keys[0].last ? "down" : "up", keys[1].last ? "down" : "up", keys[2].last ? "down" : "up", POLL_MS);
	{
		uint32_t use_irq = 1;

		PE_parse_boot_argn("keys_irq", &use_irq, sizeof(use_irq));
		if (use_irq) {
			keys_irq_setup();
		}
	}

	for (;;) {
		for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			key_look(&keys[i], redwood_gpio_get(keys[i].bank, keys[i].pin) == 0);
		}
		if (escape && keys[KEY_POWER].down && keys[KEY_VOLUME_DOWN].down) {
			uint64_t now = mach_absolute_time();

			if (both_since == 0) {
				both_since = now;
			}
			absolutetime_to_nanoseconds(now - both_since, &ms);
			if (ms / NSEC_PER_MSEC >= ESCAPE_MS) {
				printf("keys: volume down + power held %u ms: back to the loader\n", ESCAPE_MS);
				redwood_var_save_on_return();   /* the data volume into the flash first (var_store=1) */
				redwood_sync_filesystems();     /* and what HFS+ on userdata holds (bsd/dev/redwood_ufs.c) */
				loader_return();
			}
		} else {
			both_since = 0;
		}
		{
			/* busy: a key down, a state not yet settled, or an interrupt in the last 100 ms -- the 16 ms look */
			bool busy = !keys_irq_on;
			uint64_t now = mach_absolute_time(), settle;

			nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &settle);
			busy = busy || now - keys_irq_at < settle;
			for (i = 0; !busy && i < NKEYS; i++) {
				busy = keys[i].down || keys[i].last || !keys[i].live;
			}
			if (busy) {
				delay_for_interval(POLL_MS, NSEC_PER_MSEC);
			} else {
				assert_wait_timeout((event_t)&keys_wake, THREAD_UNINT, keys_irqs != 0 ? 500 : POLL_MS, NSEC_PER_MSEC);
				for (i = 0; i < NKEYS && redwood_gpio_get(keys[i].bank, keys[i].pin) != 0; i++) {
				}
				if (i < NKEYS) {
					clear_wait(current_thread(), THREAD_AWAKENED);  /* pressed meanwhile */
				} else {
					thread_block(THREAD_CONTINUE_NULL);
				}
			}
		}
	}
}

void
redwood_exynos_keys_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("keys", &on, sizeof(on))) {
		PE_parse_boot_argn("touch_ts", &on, sizeof(on));
	}
	if (on == 0) {
		return;
	}
	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

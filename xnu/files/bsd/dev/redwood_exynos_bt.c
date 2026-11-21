/*
 * The S20+'s Bluetooth: the BT side of the Broadcom BCM4375 (its Wi-Fi side is on PCIe, redwood_bcm4375.c), an
 * HCI controller on UART1, and iOS's bluetoothd talking to it (PLANS/06-wireless.md, B1-B3). Boot-arg bt=1.
 *
 * The hardware (the phone's device tree, exynos990-y2s_eur_open_21.dts "bluetooth" + exynos9830.dts uart@10840000;
 * the z3s Linux port's BLUETOOTH.md for what made it work on the same SoC):
 *   UART1           0x10840000, SPI 416, a USI v2 block (USI_CON +0xc4, USI_OPTION +0xc8), FIFOs of 256
 *   its USI mode    SYSREG_PERIC1 0x10721000 ("usi_uart_bt"): 1 = UART
 *   its clocks      CMU_PERIC1 0x10700000: MUX_CLKCMU_PERIC1_UART_BT_USER +0x610 (bit 4: 0 OSC 26 MHz, 1 the 400 MHz
 *                   PERIC1 IP clock), DIV_CLK_PERIC1_UART_BT +0x1800 (bits 3:0 + 1), gates +0x2008/+0x203c/+0x2058
 *   its pins        gpq0-0 RXD, -1 TXD, -2 RTS, -3 CTS, function 2 (ALIVE pinctrl 0x15850000, bank +0xa0). RTS is
 *                   held low as a GPIO, as z3s does (this UART's automatic flow control does not drive it); CTS is
 *                   honoured by the UART (UMCON AFC)
 *   BT_EN           gpm16-0 (CMGP pinctrl 0x15c30000, bank +0x200)
 *   BT device wake  gpg1-1 (PERIC1 pinctrl 0x10730000, bank +0xc0): held high, the chip never sleeps
 *   BT host wake    gpa2-3: not used
 *
 * Bring-up, in the kernel (bluetoothd only talks HCI; Apple's BlueTool, which would boot the chip, must not run on a
 * chip it does not know -- userland/src/bluetool_stub.c answers for it):
 *   BT_EN cycled; HCI_Reset at 115200; Broadcom 0xfc18 to BT_FAST baud; 0xfc2e (download minidriver); every record
 *   of the .hcd (0xfc4c write RAM ...); 0xfc4e (launch RAM); 250 ms; back at 115200; HCI_Reset; 0xfc18 again;
 *   0xfc27 (no sleep); 0xfc01 (the BD address); Read Local Version, for the log.
 *
 * Then iOS's side (PLANS/06-wireless.md, "B3 findings"):
 *   - an IOService named "bluetooth" with local-mac-address, transport-encoding 1 (H4) and transport-speed:
 *     bluetoothd's "is there Bluetooth" gate, its address, its transport (iokit/Kernel/RedwoodWLAN.cpp);
 *   - a kernel control named com.apple.uart.bluetooth: bluetoothd's "POSIX" UART driver is not a tty but a
 *     PF_SYSTEM socket to a kernel control of that name (BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth), with
 *     termios through getsockopt/setsockopt(SYSPROTO_CONTROL, TIOCGETA/TIOCSETA). What it writes goes out of the
 *     UART; what comes in goes to it. One client at a time.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <stdarg.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_dbgport.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);       /* (osfmk/kern/thread.h, MACH_KERNEL_PRIVATE) */
extern void IOSleep(unsigned int milliseconds);
extern void loader_log_line(const char *line);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid),
    void *context);
extern void RedwoodBluetoothStart(const uint8_t *address, uint32_t speed);       /* iokit/Kernel/RedwoodWLAN.cpp */

#define UART_PHYS       0x10840000U
#define UART_INTID      (416 + 32)
#define SW_CONF_PHYS    0x10721000U
#define CMU_PERIC1      0x10700000U
#define PINCTRL_CMGP    0x15c30000U

/* the UART (Samsung's serial_s3c.h; exynos: UFSTAT counts of 8 bits, FRACVAL) */
#define ULCON           0x00
#define UCON            0x04
#define UFCON           0x08
#define UMCON           0x0c
#define UTRSTAT         0x10
#define UERSTAT         0x14
#define UFSTAT          0x18
#define UTXH            0x20
#define URXH            0x24
#define UBRDIV          0x28
#define UFRACVAL        0x2c
#define UINTP           0x30
#define UINTS           0x34
#define UINTM           0x38
#define USI_CON         0xc4
#define USI_OPTION      0xc8
#define UFSTAT_RXCOUNT(v)       ((v) & 0xff)
#define UFSTAT_RXFULL           (1u << 8)
#define UFSTAT_TXCOUNT(v)       (((v) >> 16) & 0xff)
#define UFSTAT_TXFULL           (1u << 24)
#define UTRSTAT_TXE             (1u << 2)
#define UINT_RXD                (1u << 0)
#define UINT_ERROR              (1u << 1)
#define FIFO                    256

#define BT_SLOW         115200
/*
 * The working speed (boot-arg bt_baud): 1 Mbaud (B, 2026-11-07; was 3 Mbaud, whose 256-byte FIFO fills in 0.85 ms --
 * the interrupt was late now and then and overran it, and bluetoothd quit on the broken packet as audio started).
 * Headphones' audio needs some 40 KB/s of the 100 KB/s; the FIFO now lasts 2.6 ms.
 */
static uint32_t bt_fast = 3000000;     /* (back from 1 Mbaud: the chip sends ~100 KB/s of vendor events with audio) */
#define BT_FAST         bt_fast
#define CTL_NAME        "com.apple.uart.bluetooth"
#define RING            (64u << 10)

static volatile uint8_t *uart, *sw_conf, *cmu;
static volatile uint8_t *bank_q0, *bank_m16, *bank_g1;
static uint32_t uart_clock_hz;
static uint32_t bt_speed;
static bool bt_ready, bt_had_client;
static uint8_t bt_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };

/* what came in, for the client (and for the bring-up's own reads) */
static LCK_GRP_DECLARE(bt_lck_grp, "redwood-bt");
static LCK_MTX_DECLARE(bt_tx_lock, &bt_lck_grp);
static LCK_SPIN_DECLARE(bt_rx_spin, &bt_lck_grp);  /* the FIFO's reader: the interrupt, or the reader thread */
static uint8_t *ring;
static volatile uint32_t ring_head, ring_tail;  /* head: next byte written by the reader; tail: next one taken */
static uint64_t rx_bytes, tx_bytes, rx_overruns, rx_dropped, irqs;
static kern_ctl_ref bt_ctl;
static volatile uint32_t bt_unit;               /* the connected client's unit, 0: none */
static int bt_event;

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(uart + off);
}

static inline void
wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(uart + off) = v;
}

static void
bt_log(const char *fmt, ...) __printflike(1, 2);

static unsigned bt_vendor_said;        /* vendor command lines: the first 40 only (B, 2026-11-14) */

static void
bt_log(const char *fmt, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s", line);
	loader_log_line(line);
}

static void
pin_out(volatile uint8_t *bank, uint32_t pin, uint32_t level)
{
	uint32_t dat = *(volatile uint32_t *)(bank + REDWOOD_GPIO_DAT);

	*(volatile uint32_t *)(bank + REDWOOD_GPIO_DAT) = (dat & ~(1u << pin)) | ((level & 1) << pin);
	redwood_gpio_func(bank, pin, REDWOOD_GPIO_OUTPUT);
}

/* ---- the UART ---- */

static void
uart_baud(uint32_t baud)
{
	uint32_t div = uart_clock_hz / baud;

	/* (Samsung's s3c24xx_serial_set_termios: the nearer of div and div + 1, UBRDIV = div / 16 - 1, FRACVAL = div % 16) */
	if ((div & 15) != 15) {
		uint32_t rd_baud = uart_clock_hz / div, ru_baud = uart_clock_hz / (div + 1);

		if ((rd_baud > baud ? rd_baud - baud : baud - rd_baud) > (ru_baud > baud ? ru_baud - baud : baud - ru_baud)) {
			div++;
		}
	}
	/* what is in the transmitter goes out at the old speed first */
	for (int i = 0; i < 10000 && !(rd(UTRSTAT) & UTRSTAT_TXE); i++) {
		delay(10);
	}
	wr(UBRDIV, div / 16 - 1);
	wr(UFRACVAL, div & 15);
	bt_speed = baud;
}

static void
uart_reset_fifos(void)
{
	wr(UFCON, rd(UFCON) | (3u << 1));
	for (int i = 0; i < 1000 && (rd(UFCON) & (3u << 1)); i++) {
		delay(1);
	}
}

/* clocks, the USI's mode, pins, and the UART itself: 8N1, FIFOs, CTS flow control, interrupts for what comes in */
static bool
uart_setup(void)
{
	uint32_t mux, div, g1, g2, g3;

	cmu = (volatile uint8_t *)ml_io_map(CMU_PERIC1, 0x4000);
	sw_conf = (volatile uint8_t *)ml_io_map(SW_CONF_PHYS & ~0xfffU, 0x1000) + (SW_CONF_PHYS & 0xfffU);
	uart = (volatile uint8_t *)ml_io_map(UART_PHYS, 0x1000);
	/* the gates on (CG_VAL bit 21 with MANUAL bit 20: forced on, whatever automatic gating would do) */
	{
		static const uint32_t gates[] = { 0x2008, 0x203c, 0x2058 };

		for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
			volatile uint32_t *g = (volatile uint32_t *)(cmu + gates[i]);

			*g = *g | (1u << 21) | (1u << 20);
		}
	}
	mux = *(volatile uint32_t *)(cmu + 0x610);
	div = *(volatile uint32_t *)(cmu + 0x1800);
	g1 = *(volatile uint32_t *)(cmu + 0x2008);
	g2 = *(volatile uint32_t *)(cmu + 0x203c);
	g3 = *(volatile uint32_t *)(cmu + 0x2058);
	uart_clock_hz = ((mux >> 4) & 1 ? 400000000U : 26000000U) / ((div & 0xf) + 1);
	bt_log("bt: UART1 clock: mux %08x (%s), div %08x -> %u Hz; gates %08x %08x %08x; QCH %08x\n", mux,
	    (mux >> 4) & 1 ? "PERIC1 IP 400 MHz" : "OSC 26 MHz", div, uart_clock_hz, g1, g2, g3,
	    *(volatile uint32_t *)(cmu + 0x302c));

	/* the USI as a UART, out of reset, its clock requested continuously (z3s: "clkreq-on"; without it Rx is dead) */
	*(volatile uint32_t *)sw_conf = 1;
	wr(USI_CON, 1);
	delay(1);
	wr(USI_CON, 0);
	delay(1);
	wr(USI_OPTION, 1u << 1);

	/* pins: RXD, TXD, CTS to the UART; RTS a GPIO held low (we are always ready to take bytes) */
	bank_q0 = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, 0xa0);
	redwood_gpio_func(bank_q0, 0, 2);
	redwood_gpio_func(bank_q0, 1, 2);
	redwood_gpio_func(bank_q0, 3, 2);
	pin_out(bank_q0, 2, 0);
	redwood_gpio_pull(bank_q0, 0, REDWOOD_GPIO_PULL_UP);
	redwood_gpio_pull(bank_q0, 3, REDWOOD_GPIO_PULL_UP);

	wr(UINTM, 0xf);                         /* all masked while it is set up */
	wr(UINTP, 0xf);
	wr(ULCON, 3);                           /* 8N1 */
	/* UCON: Rx and Tx by the CPU (interrupt/polling), Rx timeout interrupt (8 frames), Rx error interrupt, level-triggered */
	wr(UCON, (1u << 0) | (1u << 2) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (3u << 12));
	/* UFCON: FIFOs on, Rx trigger 32 bytes, Tx trigger 4 */
	wr(UFCON, (1u << 0) | (4u << 4) | (1u << 8));
	uart_reset_fifos();
	wr(UMCON, 1u << 4);                     /* AFC: Tx waits for CTS */
	uart_baud(BT_SLOW);
	return true;
}

/* ---- what comes in ---- */

static uint32_t
ring_count(void)
{
	return ring_head - ring_tail;
}

/*
 * The FIFO into the ring: from the interrupt itself (B, 2026-11-07: the reader thread alone was late under load --
 * at 3 Mbaud the 256-byte FIFO fills in 0.85 ms -- 110 overruns once Bluetooth audio streamed, and bluetoothd quit
 * on the broken packets), and from the reader thread's looks; under bt_rx_spin with interrupts off.
 */
static void
uart_drain(void)
{
	uint32_t st, n, head;
	boolean_t was = ml_set_interrupts_enabled(FALSE);

	lck_spin_lock(&bt_rx_spin);
	head = ring_head;
	while ((n = UFSTAT_RXCOUNT(st = rd(UFSTAT))) != 0 || (st & UFSTAT_RXFULL)) {
		if (n == 0) {
			n = FIFO;
		}
		while (n-- > 0) {
			uint8_t c = (uint8_t)rd(URXH);

			if (head - ring_tail < RING) {
				ring[head % RING] = c;
				head++;
			} else {
				rx_dropped++;
			}
			rx_bytes++;
		}
	}
	__asm__ volatile ("dmb ish" ::: "memory");
	ring_head = head;                       /* the bytes before the count that says they are there */
	if (rd(UERSTAT) & 1) {
		rx_overruns++;
	}
	lck_spin_unlock(&bt_rx_spin);
	ml_set_interrupts_enabled(was);
}

static void
bt_irq(__unused void *context, __unused uint32_t intid)
{
	uart_drain();                           /* at once: the thread may be a while */
	wr(UINTP, UINT_RXD | UINT_ERROR);
	irqs++;
	thread_wakeup((event_t)&bt_event);
}

/*
 * bluetoothd's vendor commands and the chip's answers (B, 2026-11-07: bluetoothd quit 3 s after Apple's audio set-up
 * commands -- one went unanswered, it seems). Each vendor command is remembered; the answer's status is logged when it
 * is not 0; one with no answer after 500 ms is answered here (Command Complete, status 0: bt_fake=1, `bt.fake 0|1`)
 * and logged, so that bluetoothd goes on.
 */
/*
 * The last HCI packets both ways (B, 2026-11-07: the chip falls silent ~4 s into headphone audio, with no vendor
 * command near): direction, type, the first 12 bytes, length, time -- `bt.trace` prints them (newest last).
 */
#define TRACE 48
static struct { uint64_t at; uint16_t len; uint8_t dir, b[13]; } trace[TRACE];
static uint32_t trace_n, trace_frozen, parse_pos;  /* frozen at the chip's first crash-dump event (vendor event 0x1b) */

static void
trace_add(uint8_t dir, const uint8_t *p, uint32_t n, uint32_t len)
{
	uint32_t i;

	if (trace_frozen) {
		return;
	}
	i = trace_n++ % TRACE;

	trace[i].at = mach_absolute_time();
	trace[i].dir = dir;
	trace[i].len = (uint16_t)(len > 0xffff ? 0xffff : len);
	bzero(trace[i].b, sizeof(trace[i].b));
	memcpy(trace[i].b, p, n < 13 ? n : 13);
}

/*
 * ACL flow control kept here (B, 2026-11-07): the chip's firmware crashed once bluetoothd had 8 headphone-audio
 * packets of ~670 bytes out with none completed -- more than the chip's buffers. The chip's count comes from its
 * Read_Buffer_Size answer; each ACL packet to it takes one, each Number Of Completed Packets gives them back; a packet
 * with none free waits in bt_send (up to 1 s). bt_acl_max=N overrides, `bt.acl` shows.
 */
static volatile uint32_t acl_max, acl_out, acl_waits, acl_forced;
static uint32_t acl_len_max, bt_acl_max;

#define PENDING 8
/*
 * Vendor commands kept from the chip and answered here (Command Complete, success): the chip (Samsung's patchram)
 * fell silent for good right after Apple's 0xfdb9 / 0xfd81 as audio started (B, 2026-11-07). `bt.block OP ...` live.
 */
static uint16_t blocked[8] = { 0xfdb9, 0xfd81, 0xfc57 };   /* 0xfc57: the firmware crashed 0.5 s into A2DP after it */
static struct { uint16_t op; uint64_t at; } pending[PENDING];
static uint32_t bt_fake = 1, faked, bt_reup = 1;    /* bt_reup=1: bring-up again per client (it failed: 2026-11-07) */
static struct { uint8_t state, type, hdr[4]; uint32_t got, len; } rxp;   /* the H4 stream to the client, parsed */

static void
pending_answered(uint16_t op, uint8_t status, const char *how)
{
	for (unsigned i = 0; i < PENDING; i++) {
		if (pending[i].op == op && pending[i].at != 0) {
			pending[i].at = 0;
			if (status != 0) {
				if (bt_vendor_said++ < 40) bt_log("bt: the chip answers vendor command %#06x with status %#x (%s)\n", op, status, how);
			}
			return;
		}
	}
}

/* one byte of what goes to the client: an H4 packet's type, header and body */
/*
 * ACL fragments (B, 2026-11-07: the chip's firmware crashed on bluetoothd's ~630-byte audio packets, never on its
 * 63-byte ones): with bt_frag=N (`bt.frag N`) an ACL packet of more than N bytes goes to the chip as fragments of at
 * most N (HCI's own continuation, PB 01); each takes a buffer; the chip's completions are given back to bluetoothd per
 * its own packets (the fragments of each remembered here).
 */
/* The UART FIFO snapshot fix makes whole HCI packets reliable. Splitting
 * consumes extra controller credits and can stall the first audio burst. */
static uint32_t bt_frag;         /* 0: whole packets; bt.frag N for diagnostics */
#define FRAGQ 64
static uint8_t fragq[FRAGQ];
static volatile uint32_t fragq_head, fragq_tail, frag_done;

/* k fragments completed: how many of bluetoothd's packets that completes */
static uint32_t
frags_completed(uint32_t k)
{
	uint32_t whole = 0;

	if (fragq_head == fragq_tail) {
		return k;                       /* nothing remembered: as they are */
	}
	frag_done += k;
	while (fragq_head != fragq_tail && frag_done >= fragq[fragq_tail % FRAGQ]) {
		frag_done -= fragq[fragq_tail % FRAGQ];
		fragq_tail++;
		whole++;
	}
	if (fragq_head == fragq_tail) {
		frag_done = 0;
	}
	return whole;
}

static void
rx_parse(uint8_t *cp)
{
	uint8_t c = *cp;

	switch (rxp.state) {
	case 0:                                 /* the type */
		rxp.type = c;
		rxp.got = 0;
		rxp.state = (c == 0x04 || c == 0x02 || c == 0x03) ? 1 : 0;
		break;
	case 1: {                               /* the header: event 2 bytes, ACL 4, SCO 3 */
		uint32_t hl = rxp.type == 0x04 ? 2 : rxp.type == 0x02 ? 4 : 3;

		rxp.hdr[rxp.got++] = c;
		if (rxp.got == hl) {
			rxp.len = rxp.type == 0x04 ? rxp.hdr[1] : rxp.type == 0x02 ? (uint32_t)(rxp.hdr[2] | (rxp.hdr[3] << 8)) :
			    rxp.hdr[2];
			{
				uint8_t t[5] = { rxp.type, rxp.hdr[0], rxp.hdr[1], rxp.hdr[2], rxp.hdr[3] };

				trace_add('<', t, 1 + hl, rxp.len);
			}
			rxp.got = 0;
			rxp.state = rxp.len != 0 ? 2 : 0;
		}
		break;
	}
	default:                                /* the body: of events, the first 4 bytes kept */
		if (rxp.type == 0x04 && rxp.hdr[0] == 0xff) {   /* vendor events: the first's 16 bytes, then a rate */
			static uint8_t vb[16];
			static uint32_t vn, vsec;
			static uint64_t vlast;

			if (rxp.got < 16) {
				vb[rxp.got] = c;
			}
			/* the crash dump's readable text (the firmware's assert, file names): runs of 8+ printable bytes, logged */
			if (trace_frozen) {
				static char run[64];
				static uint32_t rl, shown;

				if (c >= 0x20 && c < 0x7f) {
					if (rl < sizeof(run) - 1) {
						run[rl++] = (char)c;
					}
				} else {
					if (rl >= 8 && shown < 40) {
						run[rl] = 0;
						shown++;
						bt_log("bt: crash dump text: %s\n", run);
					}
					rl = 0;
				}
			}
			if (rxp.got == 0 && c == 0x1b && !trace_frozen) {
				trace_frozen = 1;       /* the chip's firmware crashed and dumps its memory: keep what led to it */
				bt_log("bt: the chip's firmware crashed (memory dump, vendor event 0x1b): bt.trace has the 48 packets before\n");
			}
			if (rxp.got + 1 == rxp.len || rxp.got == 15) {
				if (rxp.got + 1 == rxp.len || rxp.got == 15) {
					uint64_t now = mach_absolute_time(), sec;

					nanoseconds_to_absolutetime(NSEC_PER_SEC, &sec);
					if (rxp.got == 15 || rxp.len < 16) {
						vn++;
						if (vn == 1 || now - vlast > sec) {
							bt_log("bt: vendor event %u (%u in the last second), %u bytes: %02x %02x %02x %02x %02x %02x %02x "
							    "%02x %02x %02x %02x %02x %02x %02x %02x %02x\n", vn, vsec, rxp.len, vb[0], vb[1], vb[2], vb[3],
							    vb[4], vb[5], vb[6], vb[7], vb[8], vb[9], vb[10], vb[11], vb[12], vb[13], vb[14], vb[15]);
							vlast = now;
							vsec = 0;
						}
						vsec++;
					}
				}
			}
		}
		if (rxp.type == 0x04 && rxp.hdr[0] == 0x13) {          /* Number Of Completed Packets: n, (handle, count)... */
			static uint8_t *lop;

			if (rxp.got >= 1 && ((rxp.got - 1) % 4) == 2) {
				lop = cp;
			} else if (rxp.got >= 1 && ((rxp.got - 1) % 4) == 3 && lop != NULL) {
				uint32_t done = (uint32_t)(*lop | (c << 8)), whole = frags_completed(done);

				acl_out = acl_out > done ? acl_out - done : 0;
				*lop = (uint8_t)whole;  /* (bluetoothd's packets, not the fragments) */
				*cp = (uint8_t)(whole >> 8);
				thread_wakeup((event_t)&acl_out);
			}
		}
		if (rxp.type == 0x04 && rxp.hdr[0] == 0x0e && rxp.got < 11) {
			static uint8_t cc[11];

			cc[rxp.got] = c;
			if (rxp.got == 10 && cc[1] == 0x05 && cc[2] == 0x10 && cc[3] == 0) {     /* Read_Buffer_Size */
				acl_len_max = (uint32_t)(cc[4] | (cc[5] << 8));
				acl_max = bt_acl_max != 0 ? bt_acl_max : (uint32_t)(cc[7] | (cc[8] << 8));
				acl_out = 0;
				bt_log("bt: the chip's ACL buffers: %u of %u bytes (flow control kept here)\n", acl_max, acl_len_max);
			}
		}
		if (rxp.type == 0x04 && rxp.got < 4) {
			static uint8_t body[4];

			body[rxp.got] = c;
			if (rxp.got == 3) {
				if (rxp.hdr[0] == 0x0e) {               /* Command Complete: n, opcode, status */
					pending_answered((uint16_t)(body[1] | (body[2] << 8)), body[3], "complete");
				} else if (rxp.hdr[0] == 0x0f) {        /* Command Status: status, n, opcode */
					pending_answered((uint16_t)(body[2] | (body[3] << 8)), body[0], "status");
				}
			}
		}
		if (++rxp.got >= rxp.len) {
			rxp.state = 0;
		}
		break;
	}
}

/* (the reader thread) a vendor command with no answer after 500 ms: answered here, between packets */
static void
pending_check(uint32_t unit)
{
	uint64_t now = mach_absolute_time(), limit;

	nanoseconds_to_absolutetime(500 * NSEC_PER_MSEC, &limit);
	for (unsigned i = 0; i < PENDING; i++) {
		if (pending[i].at != 0 && now - pending[i].at > limit) {
			uint16_t op = pending[i].op;

			pending[i].at = 0;
			if (bt_fake && unit != 0 && rxp.state == 0) {
				uint8_t ev[7] = { 0x04, 0x0e, 0x04, 0x01, (uint8_t)op, (uint8_t)(op >> 8), 0x00 };

				(void)ctl_enqueuedata(bt_ctl, unit, ev, sizeof(ev), 0);
				faked++;
				bt_log("bt: vendor command %#06x got no answer in 500 ms: answered for the chip (success; %u so far)\n",
				    op, faked);
			} else {
				bt_log("bt: vendor command %#06x got no answer in 500 ms\n", op);
			}
		}
	}
}

/* to the client, as much as it will take */
static void
bt_deliver(void)
{
	uint32_t unit = bt_unit;

	if (unit != 0) {
		uint32_t head = ring_head;

		__asm__ volatile ("dmb ish" ::: "memory");
		while (parse_pos != head) {             /* parsed (and completions rewritten) before they go */
			rx_parse(&ring[parse_pos % RING]);
			parse_pos++;
		}
	}
	while (unit != 0 && ring_count() > 0 && ring_tail != parse_pos) {
		uint32_t at = ring_tail % RING, n = parse_pos - ring_tail;
		errno_t e;

		if (n > RING - at) {
			n = RING - at;
		}
		if (n > 4096) {
			n = 4096;
		}
		e = ctl_enqueuedata(bt_ctl, unit, ring + at, n, 0);
		if (e != 0) {
			break;                          /* (full: the rest the next time round) */
		}
		ring_tail += n;
	}
	pending_check(unit);
	if (unit == 0 && !bt_ready) {
		return;                                 /* the bring-up takes its own bytes */
	}
	if (unit == 0 && ring_count() > 0) {
		rx_dropped += ring_count();
		ring_tail = ring_head;                  /* nobody to give them to */
	}
}

/*
 * The interrupt wakes it; every 100 ms besides, so that nothing is left behind a missed one -- counted:
 * bt_rescued, the looks that found bytes the interrupt had not brought (it was every 2 ms: some 500
 * wake-ups a second with nothing to do, the cpuidle wake record, 27 October).
 */
static uint64_t bt_rescued, bt_safety_looks;

static void
bt_reader(__unused void *arg, __unused wait_result_t wr_)
{
	wait_result_t why = THREAD_AWAKENED;

	for (;;) {
		uint32_t before = ring_count();

		uart_drain();
		if (why == THREAD_TIMED_OUT) {
			bt_safety_looks++;
			if (ring_count() != before) {
				bt_rescued++;
				printf("bt: the 100 ms look found %u bytes no interrupt had brought (%llu times of %llu looks)\n",
				    ring_count() - before, bt_rescued, bt_safety_looks);
			}
		}
		bt_deliver();
		wr(UINTM, rd(UINTM) & ~(UINT_RXD | UINT_ERROR));
		assert_wait_timeout((event_t)&bt_event, THREAD_UNINT, 100, NSEC_PER_MSEC);
		why = thread_block(THREAD_CONTINUE_NULL);
	}
}

/* ---- what goes out ---- */

static void
uart_write(const uint8_t *p, uint32_t n)
{
	lck_mtx_lock(&bt_tx_lock);
	while (n > 0) {
		/* A full FIFO's count wraps to zero. Use the same snapshot for
		 * count and full: if it drains between separate reads, a full
		 * FIFO can otherwise appear to have all 256 slots free. */
		uint32_t status = rd(UFSTAT);
		uint32_t room = (status & UFSTAT_TXFULL) ? 0 : FIFO - UFSTAT_TXCOUNT(status);
		if (room == 0) {
			delay(20);
			continue;
		}
		while (room-- > 0 && n > 0) {
			wr(UTXH, *p++);
			n--;
			tx_bytes++;
		}
	}
	lck_mtx_unlock(&bt_tx_lock);
}

/* ---- HCI for the bring-up ---- */

/* bytes from the ring, waiting up to ms for them */
static bool
ring_take(uint8_t *to, uint32_t n, uint32_t ms)
{
	uint64_t deadline = mach_absolute_time(), now;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &now);
	deadline += now;
	while (ring_count() < n) {
		if (mach_absolute_time() > deadline) {
			return false;
		}
		IOSleep(1);
	}
	for (uint32_t i = 0; i < n; i++) {
		to[i] = ring[ring_tail % RING];
		ring_tail++;
	}
	return true;
}

/* one command, and its Command Complete (or Status); returns the status byte, or -1. reply: its parameters after the status */
static int
hci_cmd(uint16_t opcode, const uint8_t *params, uint8_t len, uint8_t *reply, uint32_t reply_max, uint32_t ms)
{
	uint8_t pkt[3 + 255 + 1], hdr[3], ev[260];

	pkt[0] = 0x01;
	pkt[1] = (uint8_t)opcode;
	pkt[2] = (uint8_t)(opcode >> 8);
	pkt[3] = len;
	memcpy(pkt + 4, params, len);
	uart_write(pkt, 4u + len);
	for (int tries = 0; tries < 8; tries++) {
		uint8_t type;

		if (!ring_take(&type, 1, ms)) {
			return -1;
		}
		if (type != 0x04) {
			continue;                       /* (not an event: should not happen during bring-up) */
		}
		if (!ring_take(hdr, 2, ms) || !ring_take(ev, hdr[1], ms)) {
			return -1;
		}
		if (hdr[0] == 0x0e && hdr[1] >= 4 && (ev[1] | ev[2] << 8) == opcode) {
			if (reply != NULL) {
				uint32_t n = hdr[1] - 4u;

				memcpy(reply, ev + 4, n < reply_max ? n : reply_max);
			}
			return ev[3];
		}
		if (hdr[0] == 0x0f && hdr[1] >= 4 && (ev[2] | ev[3] << 8) == opcode) {
			return ev[0];
		}
	}
	return -1;
}

static bool
bt_set_speed(uint32_t baud)
{
	uint8_t p[6] = { 0, 0, (uint8_t)baud, (uint8_t)(baud >> 8), (uint8_t)(baud >> 16), (uint8_t)(baud >> 24) };
	int st = hci_cmd(0xfc18, p, sizeof(p), NULL, 0, 500);

	if (st != 0) {
		bt_log("bt: 0xfc18 (UART speed %u): status %d\n", baud, st);
		return false;
	}
	uart_baud(baud);
	IOSleep(10);
	return true;
}

/* the chip powered, patched and configured; true: ready for bluetoothd */
static bool
bt_bringup(void)
{
	/*
	 * The BCM4375 is one Wi-Fi/Bluetooth module: its maker decides both the NVRAM and this patchram. The S20+ (y2s)
	 * is SEMCO's; the European S20 (x1slte) ships Murata's "X1_EUR" NVRAM too. wifi_nvram=1 (Murata) selects both.
	 */
	int which = 0;
	(void)PE_parse_boot_argn("wifi_nvram", &which, sizeof(which));
	/* bt_addr=AA:BB:CC:DD:EE:FF: the phone's own address (its EFS, bluetooth/bt_addr) instead of the placeholder */
	{
		char a[32] = { 0 };
		if (PE_parse_boot_arg_str("bt_addr", a, sizeof(a))) {
			uint8_t v[6];
			int n = 0;
			for (const char *c = a; *c && n < 6; ) {
				char *end;
				unsigned long b = strtoul(c, &end, 16);
				if (end == c || b > 0xff) {
					break;
				}
				v[n++] = (uint8_t)b;
				c = (*end == ':') ? end + 1 : end;
			}
			if (n == 6) {
				memcpy(bt_address, v, 6);
			} else {
				bt_log("bt: bt_addr=%s is not six bytes: keeping the placeholder\n", a);
			}
		}
	}
	const char *hcd_name = which == 1 ? "bcm4375B1-murata.hcd" : "bcm4375B1-semco.hcd";
	const uint8_t *hcd;
	uint64_t hcd_size = 0, at;
	uint8_t reply[64];
	uint32_t records = 0;
	int st;
	uint64_t t0 = mach_absolute_time(), ns;

	hcd = redwood_image_file("fw", hcd_name, &hcd_size, NULL);
	if (hcd == NULL || hcd_size < 3) {
		bt_log("bt: no fw/%s in the image: the chip is not patched (userland/build.sh)\n", hcd_name);
	}
	/* BT_EN cycled, the chip awake */
	bank_m16 = redwood_gpio_bank(PINCTRL_CMGP, 0x200);
	bank_g1 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, 0xc0);
	pin_out(bank_g1, 1, 1);
	pin_out(bank_m16, 0, 0);
	IOSleep(100);                           /* (20 ms was enough from cold; a crashed chip gets longer) */
	uart_baud(BT_SLOW);                     /* the chip starts at 115200 (again: a second bring-up came at 3 Mbaud) */
	pin_out(bank_m16, 0, 1);
	IOSleep(150);
	uart_reset_fifos();
	ring_head = ring_tail = 0;

	st = hci_cmd(0x0c03, NULL, 0, NULL, 0, 1000);
	if (st != 0) {
		bt_log("bt: no answer to HCI_Reset at %u baud (status %d; %llu bytes in, %llu irqs, UFSTAT %08x, UERSTAT %08x, "
		    "UMSTAT %08x, UTRSTAT %08x)\n", BT_SLOW, st, rx_bytes, irqs, rd(UFSTAT), rd(UERSTAT), rd(0x1c), rd(UTRSTAT));
		return false;
	}
	if (hci_cmd(0x1001, NULL, 0, reply, sizeof(reply), 500) == 0) {
		bt_log("bt: the chip answers at %u baud: HCI %u rev %04x, LMP %u, manufacturer %u, LMP subversion %04x\n",
		    BT_SLOW, reply[0], reply[1] | reply[2] << 8, reply[3], reply[4] | reply[5] << 8, reply[6] | reply[7] << 8);
	}
	if (hcd != NULL && hcd_size >= 3) {
		if (!bt_set_speed(BT_FAST)) {
			bt_log("bt: downloading the patchram at %u baud\n", BT_SLOW);
		}
		if ((st = hci_cmd(0xfc2e, NULL, 0, NULL, 0, 1000)) != 0) {
			bt_log("bt: 0xfc2e (download minidriver): status %d\n", st);
			return false;
		}
		IOSleep(50);
		for (at = 0; at + 3 <= hcd_size;) {
			uint16_t op = hcd[at] | hcd[at + 1] << 8;
			uint8_t len = hcd[at + 2];

			if (at + 3 + len > hcd_size) {
				break;
			}
			st = hci_cmd(op, hcd + at + 3, len, NULL, 0, 1000);
			if (op == 0xfc4e) {
				break;                  /* launch RAM: its answer may not come; the chip restarts */
			}
			if (st != 0) {
				bt_log("bt: patchram record %u (%04x) at %llu: status %d\n", records, op, at, st);
				return false;
			}
			records++;
			at += 3u + len;
		}
		IOSleep(250);
		uart_baud(BT_SLOW);
		uart_reset_fifos();
		ring_head = ring_tail = 0;
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		bt_log("bt: patchram %s: %u records written, launched (%llu ms)\n", hcd_name, records, ns / NSEC_PER_MSEC);
		if ((st = hci_cmd(0x0c03, NULL, 0, NULL, 0, 1000)) != 0) {
			bt_log("bt: no answer to HCI_Reset after the patchram (status %d)\n", st);
			return false;
		}
	}
	if (!bt_set_speed(BT_FAST)) {
		uart_baud(BT_SLOW);
	}
	{
		uint8_t sleep[12] = { 0 };      /* 0xfc27 Set Sleep Mode: mode 0, no sleep */

		(void)hci_cmd(0xfc27, sleep, sizeof(sleep), NULL, 0, 500);
	}
	{
		uint8_t a[6];

		for (int i = 0; i < 6; i++) {
			a[i] = bt_address[5 - i];
		}
		if ((st = hci_cmd(0xfc01, a, 6, NULL, 0, 500)) != 0) {
			bt_log("bt: 0xfc01 (BD address): status %d\n", st);
		}
	}
	if (hci_cmd(0x1001, NULL, 0, reply, sizeof(reply), 500) == 0) {
		bt_log("bt: patched: HCI %u rev %04x, LMP %u, manufacturer %u, LMP subversion %04x; at %u baud\n", reply[0],
		    reply[1] | reply[2] << 8, reply[3], reply[4] | reply[5] << 8, reply[6] | reply[7] << 8, bt_speed);
	}
	if (hci_cmd(0x1009, NULL, 0, reply, sizeof(reply), 500) == 0) {
		bt_log("bt: BD address %02x:%02x:%02x:%02x:%02x:%02x\n", reply[5], reply[4], reply[3], reply[2], reply[1], reply[0]);
	}
	return true;
}

/* ---- the kernel control for bluetoothd ---- */

#define TIOCGETA_64     0x40487413u     /* _IOR('t', 19, struct termios), a 64-bit process's termios: 72 bytes */
#define TIOCSETA_64     0x80487414u
#define TIOCSETAW_64    0x80487415u
#define TIOCSETAF_64    0x80487416u

static void
dbg_bt_trace(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	size_t at = 0;
	uint64_t now = mach_absolute_time();
	uint32_t first = trace_n > TRACE ? trace_n - TRACE : 0;

	for (uint32_t k = first; k < trace_n && at + 80 < cap; k++) {
		uint32_t i = k % TRACE;
		uint64_t ns;

		absolutetime_to_nanoseconds(now - trace[i].at, &ns);
		at += (size_t)snprintf(out + at, cap - at, "%6llu ms %c %4u:", ns / 1000000, trace[i].dir, trace[i].len);
		for (unsigned j = 0; j < 13 && at + 4 < cap; j++) {
			at += (size_t)snprintf(out + at, cap - at, " %02x", trace[i].b[j]);
		}
		if (at + 2 < cap) {
			out[at++] = '\n';
			out[at] = 0;
		}
	}
	if (at == 0) {
		snprintf(out, cap, "bt: no packets yet\n");
	} else if (trace_frozen && at + 40 < cap) {
		snprintf(out + at, cap - at, "(frozen at the chip's crash)\n");
	}
}

static void
dbg_bt_frag(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (n >= 1) {
		bt_frag = (uint32_t)(v[0] > 1021 ? 1021 : v[0]);
	}
	snprintf(out, cap, "bt: ACL packets to the chip in fragments of at most %u bytes (0: whole); %u remembered\n",
	    bt_frag, fragq_head - fragq_tail);
}

static void
dbg_bt_acl(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (n >= 1) {
		bt_acl_max = (uint32_t)v[0];
		if (bt_acl_max != 0) {
			acl_max = bt_acl_max;
		}
	}
	snprintf(out, cap, "bt: ACL buffers %u (of %u bytes), %u out now; %u waits, %u recounts; trace %s\n", acl_max,
	    acl_len_max, acl_out, acl_waits, acl_forced, trace_frozen ? "frozen at a crash (bt.trace; bt.acl unfreezes with any N)" : "live");
	if (n >= 1) {
		trace_frozen = 0;
	}
}

static void
dbg_bt_block(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	size_t at;

	if (n >= 1) {
		bzero(blocked, sizeof(blocked));
		for (unsigned i = 0; i < n && i < 8; i++) {
			blocked[i] = (uint16_t)v[i];
		}
	}
	at = (size_t)snprintf(out, cap, "bt: kept from the chip:");
	for (unsigned i = 0; i < 8 && at < cap; i++) {
		if (blocked[i] != 0) {
			at += (size_t)snprintf(out + at, cap - at, " %#06x", blocked[i]);
		}
	}
	if (at < cap) {
		snprintf(out + at, cap - at, " (bt.block 0 for none)\n");
	}
}

static void
dbg_bt_fake(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (n >= 1) {
		bt_fake = v[0] != 0;
	}
	snprintf(out, cap, "bt: unanswered vendor commands answered here: %s (%u so far)\n", bt_fake ? "yes" : "no", faked);
}

/* bt.state, bt.baud [N] (used at the next bring-up: when bluetoothd starts again -- `killall bluetoothd`) */
static void
dbg_bt_baud(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (n >= 1 && v[0] >= 115200 && v[0] <= 4000000) {
		bt_fast = (uint32_t)v[0];
	}
	snprintf(out, cap, "bt: UART at %u baud now, %u at the next bring-up; %llu bytes in, %llu out, %llu overruns, "
	    "%llu dropped, %llu irqs\n", bt_speed, bt_fast, rx_bytes, tx_bytes, rx_overruns, rx_dropped, irqs);
}

static errno_t
bt_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, void **unitinfo)
{
	if (bt_unit != 0) {
		return EBUSY;
	}
	*unitinfo = NULL;
	ring_tail = ring_head;                  /* nothing old for a new client */
	/*
	 * A client after the first: bluetoothd started again, and the chip may be wedged by what the last one left (it
	 * fell silent when audio started): powered off and brought up again, patchram and all (B, 2026-11-07)
	 */
	if (bt_had_client && bt_reup) {
		bool ok;

		bt_ready = false;
		ok = bt_bringup();
		bt_ready = true;
		ring_tail = ring_head;
		bt_log("bt: brought up again for the new client: %s\n", ok ? "ok" : "FAILED");
	}
	bt_had_client = true;
	bzero(&rxp, sizeof(rxp));
	bzero(pending, sizeof(pending));
	acl_max = acl_out = 0;
	fragq_head = fragq_tail = frag_done = 0;
	parse_pos = ring_tail;
	bt_unit = sac->sc_unit;
	bt_log("bt: %s: a client connected (unit %u)\n", CTL_NAME, sac->sc_unit);
	return 0;
}

static errno_t
bt_disconnect(__unused kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	if (bt_unit == unit) {
		bt_unit = 0;
	}
	bt_log("bt: %s: the client went (unit %u; %llu bytes in, %llu out, %llu dropped, %llu overruns)\n", CTL_NAME, unit,
	    rx_bytes, tx_bytes, rx_dropped, rx_overruns);
	return 0;
}

/* one of the chip's ACL buffers, waiting for one to be freed (up to 1 s) */
static void
acl_take(void)
{
	uint64_t deadline, lim;

	if (acl_max == 0) {
		return;
	}
	nanoseconds_to_absolutetime(NSEC_PER_SEC, &lim);
	deadline = mach_absolute_time() + lim;
	if (acl_out >= acl_max) {
		acl_waits++;
	}
	while (acl_out >= acl_max) {
		if (mach_absolute_time() > deadline) {
			acl_forced++;
			acl_out = 0;            /* (completions lost: start counting again) */
			bt_log("bt: no ACL buffer freed in 1 s: counting again (%u times)\n", acl_forced);
			break;
		}
		assert_wait_timeout((event_t)&acl_out, THREAD_UNINT, 5, NSEC_PER_MSEC);
		if (acl_out < acl_max) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			break;
		}
		thread_block(THREAD_CONTINUE_NULL);
	}
	acl_out++;
}

static errno_t
bt_send(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	/*
	 * bluetoothd's vendor commands (OGF 0x3f) to the log (B, 2026-11-07: the chip fell silent when audio was routed
	 * to headphones); its UART speed change (0xfc18) kept at our speed, since the UART's speed is ours (bt_setopt)
	 */
	trace_add('>', (const uint8_t *)mbuf_data(m), (uint32_t)mbuf_len(m), (uint32_t)mbuf_pkthdr_len(m));
	if (mbuf_len(m) >= 4 && ((uint8_t *)mbuf_data(m))[0] == 0x01) {
		uint8_t *h = (uint8_t *)mbuf_data(m);
		uint16_t op = (uint16_t)(h[1] | (h[2] << 8));

		if ((op >> 10) == 0x3f) {
			uint32_t plen = h[3], have = (uint32_t)mbuf_len(m) - 4;

			for (unsigned i = 0; i < 8; i++) {
				if (blocked[i] == op && op != 0 && bt_unit != 0) {
					uint8_t ev[7] = { 0x04, 0x0e, 0x04, 0x01, (uint8_t)op, (uint8_t)(op >> 8), 0x00 };

					bt_log("bt: vendor command %#06x kept from the chip, answered here\n", op);
					mbuf_freem(m);
					while (rxp.state != 0) {
						IOSleep(1);     /* (between the chip's packets) */
					}
					(void)ctl_enqueuedata(bt_ctl, bt_unit, ev, sizeof(ev), 0);
					return 0;
				}
			}

			for (unsigned i = 0; i < PENDING; i++) {
				if (pending[i].at == 0) {
					pending[i].op = op;
					pending[i].at = mach_absolute_time();
					break;
				}
			}

			if (op == 0xfc18 && plen >= 6 && have >= 6) {
				uint32_t want = (uint32_t)h[6] | ((uint32_t)h[7] << 8) | ((uint32_t)h[8] << 16) | ((uint32_t)h[9] << 24);

				h[6] = (uint8_t)bt_speed; h[7] = (uint8_t)(bt_speed >> 8);
				h[8] = (uint8_t)(bt_speed >> 16); h[9] = (uint8_t)(bt_speed >> 24);
				bt_log("bt: bluetoothd asks for UART speed %u: kept at %u\n", want, bt_speed);
			} else {
				if (bt_vendor_said++ < 40) bt_log("bt: bluetoothd sends vendor command %#06x, %u bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
				    op, plen, have > 0 ? h[4] : 0, have > 1 ? h[5] : 0, have > 2 ? h[6] : 0, have > 3 ? h[7] : 0,
				    have > 4 ? h[8] : 0, have > 5 ? h[9] : 0, have > 6 ? h[10] : 0, have > 7 ? h[11] : 0);
				/* Apple's advertising commands, whole (B, 2026-11-19: the chip refuses 0xfce9/0xfce2, so nothing is
				 * advertised -- Continuity, Instant Hotspot; their layout is read from these) */
				static uint32_t adv_said;
				if ((op == 0xfce9 || op == 0xfce2) && adv_said++ < 400) {
					char hex[3 * 96 + 1];
					uint32_t n = have < 96 ? have : 96, i;

					for (i = 0; i < n; i++) {
						snprintf(hex + 3 * i, 4, "%02x ", h[4 + i]);
					}
					hex[3 * n] = 0;
					bt_log("bt: adv %#06x %u: %s\n", op, plen, hex);
				}
			}
		}
	}
	if (mbuf_len(m) >= 5 && ((uint8_t *)mbuf_data(m))[0] == 0x02) {
		static uint8_t pkt[4 + 1100];
		uint32_t total = (uint32_t)mbuf_pkthdr_len(m), len, at, nfrag = 0;
		uint16_t hh;

		if (total > sizeof(pkt)) {
			total = sizeof(pkt);
		}
		(void)mbuf_copydata(m, 0, total, pkt);
		mbuf_freem(m);
		hh = (uint16_t)(pkt[1] | (pkt[2] << 8));
		len = (uint32_t)(pkt[3] | (pkt[4] << 8));
		if (len + 5 > total) {
			len = total - 5;
		}
		/* the fragments: the first keeps its PB flags, the rest are continuations (PB 01) */
		at = 0;
		do {
			uint32_t n = (bt_frag != 0 && len - at > bt_frag) ? bt_frag : len - at;
			uint16_t h2 = at == 0 ? hh : (uint16_t)((hh & 0xcfff) | 0x1000);
			uint8_t hdr[5] = { 0x02, (uint8_t)h2, (uint8_t)(h2 >> 8), (uint8_t)n, (uint8_t)(n >> 8) };

			acl_take();
			uart_write(hdr, 5);
			uart_write(pkt + 5 + at, n);
			at += n;
			nfrag++;
		} while (at < len);
		if (bt_frag != 0 || fragq_head != fragq_tail) {
			if (fragq_head - fragq_tail < FRAGQ) {
				fragq[fragq_head % FRAGQ] = (uint8_t)nfrag;
				fragq_head++;
			}
		}
		return 0;
	}
	for (mbuf_t n = m; n != NULL; n = mbuf_next(n)) {
		if (mbuf_len(n) > 0) {
			uart_write((const uint8_t *)mbuf_data(n), (uint32_t)mbuf_len(n));
		}
	}
	mbuf_freem(m);
	return 0;
}

static errno_t
bt_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, __unused void *data,
    __unused size_t len)
{
	uint32_t o = (uint32_t)opt;

	/* the speed is the kernel's: the chip's is changed with 0xfc18 only, and was at bring-up */
	return o == TIOCSETA_64 || o == TIOCSETAW_64 || o == TIOCSETAF_64 ? 0 : EINVAL;
}

static errno_t
bt_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	if ((uint32_t)opt == TIOCGETA_64 && data != NULL && *len >= 72) {
		uint64_t *t = (uint64_t *)data;

		bzero(data, 72);
		t[0] = 0x4;                             /* c_iflag: IGNPAR */
		t[1] = 0;                               /* c_oflag */
		t[2] = 0x300 | 0x800 | 0x8000 | 0x10000 | 0x20000;     /* c_cflag: CS8 CREAD CLOCAL CCTS_OFLOW CRTS_IFLOW */
		t[3] = 0;                               /* c_lflag */
		t[7] = bt_speed;                        /* c_ispeed (after c_cc[20] and padding: offset 56) */
		t[8] = bt_speed;                        /* c_ospeed */
		*len = 72;
		return 0;
	}
	return EINVAL;
}

static void
bt_start(__unused void *arg, __unused wait_result_t w)
{
	struct kern_ctl_reg reg;
	thread_t reader;
	errno_t e;

	if (kernel_thread_start_priority(bt_reader, NULL, 93, &reader) != KERN_SUCCESS) {
		bt_log("bt: no reader thread\n");
		return;
	}
	thread_deallocate(reader);
	if (gic_v3_spi_attach(UART_INTID, false, bt_irq, NULL) != KERN_SUCCESS) {
		bt_log("bt: UART1's interrupt (GIC %u) not attached: polled every 2 ms\n", UART_INTID);
	}
	wr(UINTM, rd(UINTM) & ~(UINT_RXD | UINT_ERROR));
	if (!bt_bringup()) {
		bt_log("bt: the controller did not come up: no Bluetooth\n");
		return;
	}
	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, CTL_NAME, sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_sendsize = 64 << 10;
	reg.ctl_recvsize = 256 << 10;
	reg.ctl_connect = bt_connect;
	reg.ctl_disconnect = bt_disconnect;
	reg.ctl_send = bt_send;
	reg.ctl_setopt = bt_setopt;
	reg.ctl_getopt = bt_getopt;
	e = ctl_register(&reg, &bt_ctl);
	if (e != 0) {
		bt_log("bt: kernel control %s not registered (%d)\n", CTL_NAME, e);
		return;
	}
	bt_ready = true;
	RedwoodBluetoothStart(bt_address, bt_speed);
	bt_log("bt: ready: kernel control %s (H4 at %u baud), IOService \"bluetooth\" for bluetoothd\n", CTL_NAME, bt_speed);
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_QUAD(_kern_redwood, OID_AUTO, bt_rx_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &rx_bytes, "bytes from the BT controller");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, bt_tx_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &tx_bytes, "bytes to the BT controller");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, bt_rx_dropped, CTLFLAG_RD | CTLFLAG_LOCKED, &rx_dropped, "bytes nobody took");

void redwood_exynos_bt_init(void);

void
redwood_exynos_bt_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	(void)PE_parse_boot_argn("bt_baud", &bt_fast, sizeof(bt_fast));
	(void)PE_parse_boot_argn("bt_fake", &bt_fake, sizeof(bt_fake));
	(void)PE_parse_boot_argn("bt_acl_max", &bt_acl_max, sizeof(bt_acl_max));
	(void)PE_parse_boot_argn("bt_frag", &bt_frag, sizeof(bt_frag));
	redwood_dbgport_add("bt.frag", "[N]: ACL packets to the chip in fragments of at most N bytes (0: whole)", dbg_bt_frag);
	redwood_dbgport_add("bt.acl", "[N]: ACL flow control kept for the chip: its buffers (N overrides; any N unfreezes the trace)", dbg_bt_acl);
	redwood_dbgport_add("bt.trace", ": the last 48 HCI packets both ways (ms ago, > to the chip, < from it, length, bytes)", dbg_bt_trace);
	redwood_dbgport_add("bt.block", "[OP ...]: vendor commands kept from the chip and answered here (0: none)", dbg_bt_block);
	redwood_dbgport_add("bt.fake", "0|1: answer vendor commands the chip leaves unanswered (500 ms)", dbg_bt_fake);
	redwood_dbgport_add("bt.baud", "[N]: the BT UART's speed now and at the next bring-up (killall bluetoothd), counters", dbg_bt_baud);
	if (!PE_parse_boot_argn("bt", &on, sizeof(on)) || on == 0) {
		return;
	}
	ring = kalloc_data(RING, Z_WAITOK | Z_ZERO);
	if (ring == NULL || !uart_setup()) {
		return;
	}
	if (kernel_thread_start(bt_start, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* Device-side USB mux: USB framing -> real kernel TCP sockets on loopback.
 * Stock lockdownd checks TCP_PEERPID and accepts these as USB connections.
 * This layer neither answers lockdown requests nor modifies native trust.
 * All socket operations occur in a worker, never in the DWC3 polling path.
 */
#include "redwood_usbmux_probe.h"
#include "redwood_usbmux_wire.h"
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <netinet/in.h>
#include <sys/errno.h>
#include <sys/kpi_socket.h>
#include <sys/systm.h>
#include <machine/machine_routines.h>

#define MQ 16u
#define MC 16u
#define MW 65536u
struct mux_packet {
	size_t len;
	uint8_t bytes[RWMUX_MAX];
};
struct mux_connection {
	socket_t so;
	uint16_t host, port;
	uint32_t seq, ack, una, window;
	size_t pending;
	bool connecting, synack, established, ack_due;
	uint64_t deadline;
	uint8_t *input;
};
static LCK_GRP_DECLARE(mux_grp, "redwood-usbmux");
static LCK_SPIN_DECLARE(mux_lock, &mux_grp);
static struct mux_packet *mq_rx, *mq_tx;
static struct mux_connection *mc;
static uint32_t rxhead, rxtail, txhead, txtail, generation, worker_generation;
/* the worker's wake-up (A, 2026-11-14): it polled every millisecond for ever, 880 wake-ups a second of the big
 * cores with nothing on the cable -- the hottest kernel thread at idle. Now 1 ms only while data moves; quiet,
 * it sleeps 20 ms (100 ms with the link down) unless a packet from the computer or a link change wakes it. */
static int mux_wake_event;
static uint32_t mux_activity;
static bool mux_started, mux_link;
extern void redwood_usbmux_notify(bool);
static unsigned mux_version;
static bool lockdown_refused;      /* (see mux_lockdown_probe) */
static uint16_t txseq, rxseq;
static uint64_t mux_packets, mux_connections, mux_bad;
static uint8_t assembly[RWMUX_MAX], output[RWMUX_MAX];
static size_t assembled;

void redwood_usbmux_link(bool up);
bool redwood_usbmux_rx(const uint8_t *bytes, size_t length);
size_t redwood_usbmux_tx(uint8_t *bytes, size_t cap);

static bool mux_enqueue(const uint8_t *bytes, size_t length) {
	bool ok = false;
	lck_spin_lock(&mux_lock);
	if (mux_link && worker_generation == generation && mq_tx && txhead - txtail < MQ && length <= RWMUX_MAX) {
		struct mux_packet *p = &mq_tx[txhead % MQ];
		p->len = length;
		memcpy(p->bytes, bytes, length);
		txhead++;
		ok = true;
	}
	lck_spin_unlock(&mux_lock);
	return ok;
}
size_t redwood_usbmux_tx(uint8_t *bytes, size_t cap) {
	size_t n = 0;
	lck_spin_lock(&mux_lock);
	if (mq_tx && txtail != txhead && mq_tx[txtail % MQ].len <= cap) {
		n = mq_tx[txtail % MQ].len;
		memcpy(bytes, mq_tx[txtail % MQ].bytes, n);
		txtail++;
	}
	lck_spin_unlock(&mux_lock);
	return n;
}
bool redwood_usbmux_rx(const uint8_t *bytes, size_t length) {
	bool ok = false;
	lck_spin_lock(&mux_lock);
	if (mux_link && mq_rx && rxhead - rxtail < MQ && length <= RWMUX_MAX) {
		struct mux_packet *p = &mq_rx[rxhead % MQ];
		p->len = length;
		memcpy(p->bytes, bytes, length);
		rxhead++;
		ok = true;
	}
	lck_spin_unlock(&mux_lock);
	if (ok)
		thread_wakeup(&mux_wake_event);
	return ok;
}
static size_t mux_header(uint32_t proto, size_t payload) {
	size_t h = mux_version >= 2 ? 16 : 8;
	memset(output, 0, h);
	rwmux_put32(output, proto);
	rwmux_put32(output + 4, (uint32_t)(h + payload));
	if (mux_version >= 2) {
		rwmux_put32(output + 8, RWMUX_MAGIC);
		rwmux_put16(output + 12, txseq);
		rwmux_put16(output + 14, rxseq);
	}
	return h;
}
static bool mux_tcp(struct mux_connection *c, uint8_t flags, const uint8_t *data, size_t n) {
	size_t h = mux_header(6, 20 + n);
	uint8_t *t = output + h;
	memset(t, 0, 20);
	rwmux_put16(t, c->port);
	rwmux_put16(t + 2, c->host);
	rwmux_put32(t + 4, c->seq);
	rwmux_put32(t + 8, c->ack);
	t[12] = 0x50;
	t[13] = flags;
	rwmux_put16(t + 14, (uint16_t)((MW - c->pending) >> 8));
	if (n)
		memcpy(t + 20, data, n);
	bool ok = mux_enqueue(output, h + 20 + n);
	if (ok)
		txseq++;
	return ok;
}
static void mux_close(struct mux_connection *c, bool reset) {
	if (reset)
		(void)mux_tcp(c, 4, NULL, 0);
	if (c->so)
		sock_close(c->so);
	uint8_t *input = c->input;
	memset(c, 0, sizeof(*c));
	c->input = input;
}
static void mux_reset(void) {
	for (unsigned i = 0; i < MC; i++)
		mux_close(&mc[i], false);
	mux_version = 0;
	txseq = 0;
	rxseq = 0xffff;
	assembled = 0;
}
static void mux_input(const uint8_t *p, size_t length) {
	uint32_t protocol = rwmux_get32(p);
	size_t h = mux_version >= 2 ? 16 : 8;
	if (mux_version >= 2)
		rxseq = rwmux_get16(p + 12);
	if (protocol == 0) {
		if (length != 20 || mux_version || rwmux_get32(p + 8) < 1) {
			mux_bad++;
			return;
		}
		size_t n = mux_header(0, 12);
		rwmux_put32(output + n, 2);
		rwmux_put32(output + n + 4, 0);
		rwmux_put32(output + n + 8, 0);
		if (mux_enqueue(output, n + 12)) {
			mux_version = 2;
			txseq = 0;
			rxseq = 0xffff;
			printf("usbmux: version 2 negotiated\n");
		}
		return;
	}
	if (protocol == 2) {
		if (mux_version == 2 && length == 17 && p[16] == 7) {
			txseq = 0;
			rxseq = 0;
		} else
			mux_bad++;
		return;
	}
	if (protocol != 6 || !mux_version || length < h + 20) {
		mux_bad++;
		return;
	}
	const uint8_t *t = p + h;
	size_t th = (t[12] >> 4) * 4u;
	if (th < 20 || th > length - h) {
		mux_bad++;
		return;
	}
	size_t n = length - h - th;
	uint16_t host = rwmux_get16(t), port = rwmux_get16(t + 2);
	uint32_t seq = rwmux_get32(t + 4), ack = rwmux_get32(t + 8), window = (uint32_t)rwmux_get16(t + 14) << 8;
	uint8_t flags = t[13];
	struct mux_connection *c = NULL;
	for (unsigned i = 0; i < MC; i++)
		if (mc[i].so && mc[i].host == host && mc[i].port == port) {
			c = &mc[i];
			break;
		}
	if (flags & 4) {
		if (c)
			mux_close(c, false);
		return;
	}
	if ((flags & 2) && !c) {
		for (unsigned i = 0; i < MC; i++)
			if (!mc[i].so) {
				c = &mc[i];
				break;
			}
		if (!c) {
			struct mux_connection rejected = {.host = host, .port = port, .ack = seq + 1};
			(void)mux_tcp(&rejected, 4, NULL, 0);
			return;
		}
		uint8_t *input = c->input;
		memset(c, 0, sizeof(*c));
		c->input = input;
		c->host = host;
		c->port = port;
		c->ack = seq + 1;
		c->window = window;
		int error = sock_socket(PF_INET, SOCK_STREAM, 0, NULL, NULL, &c->so);
		struct sockaddr_in a = {.sin_len = sizeof(a),
					.sin_family = AF_INET,
					.sin_port = htons(port),
					.sin_addr.s_addr = htonl(0x7f000001)};
		if (!error)
			error = sock_connect(c->so, (struct sockaddr *)&a, MSG_DONTWAIT);
		if (error && error != EINPROGRESS) {
			if (port == 62078)
				lockdown_refused = true;
			mux_close(c, true);
			return;
		}
		c->connecting = true;
		uint64_t timeout;
		nanoseconds_to_absolutetime(3ULL * NSEC_PER_SEC, &timeout);
		c->deadline = mach_absolute_time() + timeout;
		mux_connections++;
		printf("usbmux: host port %u -> native port %u\n", host, port);
		return;
	}
	if (!c)
		return;
	if (flags != 0x10) {
		mux_close(c, true);
		return;
	}
	if ((int32_t)(ack - c->una) >= 0 && (int32_t)(c->seq - ack) >= 0) {
		c->una = ack;
		if (c->synack && ack == 1)
			c->established = true;
	}
	c->window = window;
	if (seq != c->ack) {
		c->ack_due = true;
		return;
	}
	if (n > MW - c->pending) {
		mux_close(c, true);
		return;
	}
	if (n) {
		memcpy(c->input + c->pending, t + th, n);
		c->pending += n;
		c->ack += (uint32_t)n;
		c->ack_due = true;
	}
}
static void mux_stream(const uint8_t *p, size_t n) {
	while (n) {
		size_t take = RWMUX_MAX - assembled;
		if (take > n)
			take = n;
		if (!take) {
			mux_bad++;
			mux_reset();
			return;
		}
		memcpy(assembly + assembled, p, take);
		assembled += take;
		p += take;
		n -= take;
		for (;;) {
			int packet = rwmux_frame(assembly, assembled, (int)mux_version);
			if (packet < 0) {
				mux_bad++;
				mux_reset();
				return;
			}
			if (!packet)
				break;
			mux_input(assembly, (size_t)packet);
			mux_packets++;
			assembled -= (size_t)packet;
			memmove(assembly, assembly + packet, assembled);
		}
	}
}
static void mux_poll(struct mux_connection *c) {
	if (!c->so)
		return;
	if (c->connecting) {
		if (!sock_isconnected(c->so)) {
			int error = 0;
			int size = sizeof(error);
			(void)sock_getsockopt(c->so, SOL_SOCKET, SO_ERROR, &error, &size);
			if (error || mach_absolute_time() > c->deadline) {
				if (c->port == 62078)
					lockdown_refused = true;
				mux_close(c, true);
			}
			return;
		}
		if (mux_tcp(c, 0x12, NULL, 0)) {
			c->seq = 1;
			c->connecting = false;
			c->synack = true;
		}
		return;
	}
	if (c->pending) {
		struct iovec iov = {.iov_base = c->input, .iov_len = c->pending};
		struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
		size_t sent = 0;
		int error = sock_send(c->so, &msg, MSG_DONTWAIT, &sent);
		if (error && error != EWOULDBLOCK) {
			mux_close(c, true);
			return;
		}
		if (sent) {
			c->pending -= sent;
			memmove(c->input, c->input + sent, c->pending);
			c->ack_due = true;
		}
	}
	if (c->ack_due && mux_tcp(c, 0x10, NULL, 0))
		c->ack_due = false;
	uint32_t flight = c->seq - c->una;
	if (!c->established || flight >= c->window)
		return;
	bool room;
	lck_spin_lock(&mux_lock);
	room = txhead - txtail < MQ;
	lck_spin_unlock(&mux_lock);
	if (!room)
		return;
	uint8_t data[4096];
	size_t cap = sizeof(data);
	if (cap > c->window - flight)
		cap = c->window - flight;
	struct iovec iov = {.iov_base = data, .iov_len = cap};
	struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
	size_t received = 0;
	int error = sock_receive(c->so, &msg, MSG_DONTWAIT, &received);
	if (error == EWOULDBLOCK)
		return;
	if (error || !received) {
		mux_close(c, true);
		return;
	}
	mux_activity++;
	if (mux_tcp(c, 0x10, data, received))
		c->seq += (uint32_t)received;
	else
		mux_close(c, true);
}
/* A Mac asks for lockdownd (62078) the moment the device is on its bus; at start-up that is before launchd
 * has the socket, the Mac is refused, and its discovery agent gives the device up: no Finder entry until the
 * agent is restarted or the cable replugged (every restart of the phone, 2026-11-13: "USBMuxConnectByPort
 * failed ... Connection refused", AMDeviceConnect 0xE8000065). So: once refused, look every second whether
 * the port answers now, and then let the link drop and come back once -- the Mac asks again and is answered. */
extern void redwood_usb_gadget_replug(void);
static uint64_t lockdown_probe_at;
static void mux_lockdown_probe(void) {
	if (!lockdown_refused)
		return;
	uint64_t now = mach_absolute_time(), second;
	if (now < lockdown_probe_at)
		return;
	nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
	lockdown_probe_at = now + second;
	socket_t so = NULL;
	struct sockaddr_in a = {.sin_len = sizeof(a),
				.sin_family = AF_INET,
				.sin_port = htons(62078),
				.sin_addr.s_addr = htonl(0x7f000001)};
	if (sock_socket(PF_INET, SOCK_STREAM, 0, NULL, NULL, &so))
		return;
	int error = sock_connect(so, (struct sockaddr *)&a, 0);
	sock_close(so);
	if (error)
		return;
	lockdown_refused = false;
	printf("usbmux: lockdownd answers now and the computer was refused before: the link restarts once\n");
	redwood_usb_gadget_replug();
}
static void mux_worker(__unused void *arg, __unused wait_result_t result) {
	uint32_t seen = 0, activity = 0, quiet = 0;
	struct mux_packet *packet = kalloc_data(sizeof(struct mux_packet), Z_WAITOK | Z_ZERO);
	for (;;) {
		bool have = false, up;
		uint32_t gen;
		lck_spin_lock(&mux_lock);
		gen = generation;
		up = mux_link;
		if (mq_rx && rxtail != rxhead) {
			memcpy(packet, &mq_rx[rxtail % MQ], sizeof(*packet));
			rxtail++;
			have = true;
		}
		lck_spin_unlock(&mux_lock);
		if (gen != seen) {
			mux_reset();
			seen = gen;
			worker_generation = gen;
			redwood_usbmux_notify(up);
		}
		if (up) {
			if (have)
				mux_stream(packet->bytes, packet->len);
			for (unsigned i = 0; i < MC; i++)
				mux_poll(&mc[i]);
			mux_lockdown_probe();
		}
		if (have || activity != mux_activity) {
			activity = mux_activity;
			quiet = 0;
		} else if (quiet < 1000) {
			quiet++;
		}
		if (up && quiet < 50) {
			delay_for_interval(1, NSEC_PER_MSEC);   /* data moved in the last 50 ms */
		} else {
			assert_wait_timeout((event_t)&mux_wake_event, THREAD_UNINT, up ? 20 : 100, NSEC_PER_MSEC);
			thread_block(THREAD_CONTINUE_NULL);
		}
	}
}
void redwood_usbmux_link(bool up) {
	if (!mux_started && up) {
		mq_rx = kalloc_data(sizeof(struct mux_packet) * MQ, Z_WAITOK | Z_ZERO);
		mq_tx = kalloc_data(sizeof(struct mux_packet) * MQ, Z_WAITOK | Z_ZERO);
		mc = kalloc_type(struct mux_connection, MC, Z_WAITOK | Z_ZERO);
		if (mc)
			for (unsigned i = 0; i < MC; i++) {
				mc[i].input = kalloc_data(MW, Z_WAITOK | Z_ZERO);
				if (!mc[i].input)
					return;
			}
		if (!mq_rx || !mq_tx || !mc) {
			printf("usbmux: allocation failed\n");
			return;
		}
		thread_t thread;
		if (kernel_thread_start(mux_worker, NULL, &thread) != KERN_SUCCESS)
			return;
		thread_deallocate(thread);
		mux_started = true;
	}
	lck_spin_lock(&mux_lock);
	mux_link = up;
	generation++;
	rxtail = rxhead;
	txtail = txhead;
	lck_spin_unlock(&mux_lock);
	thread_wakeup(&mux_wake_event);
	/* (not with interrupts off: loader_return() quiesces the gadget that way, and the console then panics
	 * "Console I/O from interrupt-disabled context" -- kern.redwood_menu never reached Log access, 2026-11-13) */
	if (ml_get_interrupts_enabled()) {
		printf("usbmux: cable %s (packets %llu, connections %llu, invalid %llu)\n", up ? "configured" : "disconnected",
		       mux_packets, mux_connections, mux_bad);
	}
}

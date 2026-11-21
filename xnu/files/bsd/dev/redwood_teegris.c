/*
 * redwood_teegris.c -- Samsung TEEGRIS (the S20's secure OS) from XNU (A, 2026-11-14).
 *
 * A port of the normal-world half of Samsung's tzdev driver (drivers/misc/tzdev of the Exynos990 kernel,
 * GPL-2.0, reference commit 69515fbb7): the order and the wire formats are Samsung's, kept close to the
 * original so the secure world sees what it saw from Linux. What is here:
 *
 *   - SMCs: TEEGRIS fast SMC32 calls 0xb200xxxx (CHECK_VERSION, CONNECT_AUX, CONNECT, SCHEDULE, SYSCONF,
 *     SHMEM_LIST_REG/RLS), seven argument registers.
 *   - IW channels: per-CPU 4 KiB AUX buffers, the service channel (cpu_mask: where the secure OS wants time),
 *     per-CPU log rings (printed as "SW> ..."), socket and socket-event channels. Shared pages are 4 KiB on
 *     the wire (Linux's page); XNU's 16 KiB pages are handed over as four 4 KiB frames each (kvtophys).
 *   - The scheduler: a worker per CPU that enters the secure world (SCHEDULE) while the service channel's
 *     cpu_mask asks for that CPU or a command is pending; the event interrupt (GIC SPI 222, INTID 254) and
 *     every SMC's return wake them, as Samsung's kthread pool.
 *   - Inter-world sockets (iwsock): connect/listen/accept/read/write over the shared rings, the events bitmaps,
 *     and the notification thread.
 *   - Shared memory registration (mem): kernel buffers by 4 KiB frame lists, with the client credential.
 *   - REE time: the secure world's clock source ("ree_time_socket").
 *   - A TEE client (teec): context, shared memory, open session with the TA image handed over
 *     (TEECS_OpenSession, so no loader daemon), invoke, close.
 *
 * Started on demand: sysctl kern.redwood.tee "start". Status and tests: see tee_sysctl() at the end.
 * Fingerprint TA research commands are manually armed; no native biometric unlock is exposed.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/vnode_internal.h>
#include <sys/proc.h>
#include <sys/time.h>
#include <sys/redwood_teegris.h>
#include <sys/redwood_fp_wire.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/kalloc.h>
#include <kern/clock.h>
#include <vm/vm_kern.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>

extern uint64_t kvtophys(vm_offset_t);
extern processor_t cpu_to_processor(int);
extern processor_t thread_bind(processor_t);
extern unsigned int real_ncpus;
extern kern_return_t gic_v3_spi_attach_to(uint32_t, bool, void (*)(void *, uint32_t), void *, int);
extern void gic_v3_spi_mask(uint32_t);
extern void _disable_preemption(void);
extern void _enable_preemption(void);
#define disable_preemption()    _disable_preemption()
#define enable_preemption()     _enable_preemption()

/* ---- small Linux compatibility --------------------------------------------------------------------- */
#define TZ_PAGE                 4096u
#define TZ_ROUND_UP(x, a)       ((((x) + (a) - 1) / (a)) * (a))
#define TZ_ROUND_DOWN(x, a)     (((x) / (a)) * (a))
#define BITS_PER_INT            32u
#define IS_ERR_VALUE(x)         ((long)(x) < 0 && (long)(x) >= -4095)
#define tz_rmb()                __asm__ volatile ("dmb ishld" ::: "memory")
#define tz_wmb()                __asm__ volatile ("dmb ishst" ::: "memory")
#define tz_mb()                 __asm__ volatile ("dmb ish" ::: "memory")
#ifndef ECOMM
#define ECOMM                   ENOTCONN
#endif

LCK_GRP_DECLARE(tee_lck_grp, "redwood-tee");

/*
 * Waiting: the condition is evaluated outside the wait (it may take a mutex), then the thread waits for a
 * wake-up on `ev` with a short timeout, so a wake-up that came between the check and the wait costs at most
 * that timeout. All waits here are for the secure world's answers (milliseconds).
 */
#define TZ_WAIT_MS              10
static void
tz_sleep_on(void *ev)
{
	assert_wait_timeout((event_t)ev, THREAD_UNINT, TZ_WAIT_MS, 1000 * NSEC_PER_USEC);
	thread_block(THREAD_CONTINUE_NULL);
}
#define tz_wake(ev)             thread_wakeup((event_t)(ev))

static void
tz_msleep(unsigned ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* wired, zeroed kernel memory whose 4 KiB frames are handed to the secure world */
static void *
tz_alloc(size_t size)
{
	vm_offset_t va = 0;

	size = round_page(size ? size : 1);
	if (kmem_alloc(kernel_map, &va, size, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	return (void *)va;
}

static void
tz_free(void *p, size_t size)
{
	if (p != NULL) {
		kmem_free(kernel_map, (vm_offset_t)p, round_page(size ? size : 1));
	}
}

static uint32_t
tz_pfn(const void *p, unsigned index4k)
{
	uint64_t pa = kvtophys((vm_offset_t)p + (vm_offset_t)index4k * TZ_PAGE);

	return (uint32_t)(pa >> 12);
}

/* ---- SMC ------------------------------------------------------------------------------------------------ */
#define TZ_SMC(fid)             (0xb2000000u | (fid))
#define TZ_SMC_CONNECT_AUX      TZ_SMC(0)
#define TZ_SMC_CONNECT          TZ_SMC(1)
#define TZ_SMC_SCHEDULE         TZ_SMC(3)
#define TZ_SMC_SHMEM_LIST_REG   TZ_SMC(6)
#define TZ_SMC_SHMEM_LIST_RLS   TZ_SMC(7)
#define TZ_SMC_SYSCONF          TZ_SMC(9)
#define TZ_SMC_CHECK_VERSION    TZ_SMC(13)
#define TZ_LINUX_VERSION_CODE   0x41357u        /* 4.19.87: the reference client's, an ABI field */
#define TZ_DRIVER_CODE          0x30000u        /* tzdev driver ABI 3.0.0 */

enum { TZ_IWIO_CONNECT_LOG, TZ_IWIO_CONNECT_SERVICE, TZ_IWIO_CONNECT_TRANSPORT, TZ_IWIO_CONNECT_PROFILER,
	   TZ_IWIO_CONNECT_PANIC_DUMP, TZ_IWIO_CONNECT_SOCKET_EVENTS, TZ_IWIO_CONNECT_SOCKET };

static uint64_t tee_smcs, tee_schedules;
static void tee_post_smc(void);

static int
tz_smc_raw(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t *out1)
{
	register uint64_t x0 __asm__("x0") = a0;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	register uint64_t x4 __asm__("x4") = 0;
	register uint64_t x5 __asm__("x5") = 0;
	register uint64_t x6 __asm__("x6") = 0;

	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6)
	    : : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	if (out1 != NULL) {
		*out1 = x1;
	}
	__atomic_fetch_add(&tee_smcs, 1, __ATOMIC_RELAXED);
	return (int)(int32_t)x0;
}

static int
tz_smc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int ret = tz_smc_raw(a0, a1, a2, a3, NULL);

	tee_post_smc();
	return ret;
}

/* ---- state ---------------------------------------------------------------------------------------------- */
#define TZ_MAX_CPUS             8u
enum { TEE_DOWN, TEE_STARTING, TEE_UP, TEE_FAILED };
static int tee_state = TEE_DOWN;
static char tee_status[512] = "TEEGRIS: not started (sysctl kern.redwood.tee=start)";
static unsigned tee_ncpus;

#define tee_say(...)            printf("redwood-tee: " __VA_ARGS__)

/* ---- IW channels ------------------------------------------------------------------------------------------ */
static void *aux_buf[TZ_MAX_CPUS];              /* each one XNU page; only its first 4 KiB is the secure world's */

/* the AUX buffer of the CPU this runs on; preemption stays off until tz_aux_put() */
static uint8_t *
tz_aux_get(void)
{
	disable_preemption();
	return aux_buf[cpu_number() % tee_ncpus];
}

static void
tz_aux_put(void)
{
	enable_preemption();
}

static void
tz_on_cpu(unsigned cpu, bool on)
{
	thread_bind(on ? cpu_to_processor((int)cpu) : PROCESSOR_NULL);
	thread_block(THREAD_CONTINUE_NULL);
}

typedef int (*tz_iwio_callback_t)(void *buf, unsigned long num_pages, void *ext);

/* a channel of `num_pages` 4 KiB pages, its frame list pushed through this CPU's AUX buffer */
static void *
tz_iwio_alloc_iw_channel(unsigned mode, unsigned num_pages, tz_iwio_callback_t pre, void *ext)
{
	const unsigned per_aux = TZ_PAGE / sizeof(uint32_t);
	void *buf = tz_alloc((size_t)num_pages * TZ_PAGE);
	uint32_t *pfns;
	int ret = 0;

	if (buf == NULL) {
		return NULL;
	}
	pfns = kalloc_data(num_pages * sizeof(uint32_t), Z_WAITOK);
	if (pfns == NULL) {
		return NULL;                    /* (the buffer stays: never handed over) */
	}
	for (unsigned i = 0; i < num_pages; i++) {
		pfns[i] = tz_pfn(buf, i);
	}
	if (pre != NULL && (ret = pre(buf, num_pages, ext)) != 0) {
		kfree_data(pfns, num_pages * sizeof(uint32_t));
		tz_free(buf, (size_t)num_pages * TZ_PAGE);
		return NULL;
	}
	for (unsigned off = 0; off < num_pages; off += per_aux) {
		unsigned n = MIN(per_aux, num_pages - off);
		uint8_t *aux = tz_aux_get();

		memcpy(aux, pfns + off, n * sizeof(uint32_t));
		ret = tz_smc(TZ_SMC_CONNECT, mode, n, 0);
		tz_aux_put();
		if (ret != 0) {
			tee_say("channel mode %u (%u pages): CONNECT answered %d\n", mode, num_pages, ret);
			kfree_data(pfns, num_pages * sizeof(uint32_t));
			return NULL;            /* (kept: the secure world may have taken part of it) */
		}
	}
	kfree_data(pfns, num_pages * sizeof(uint32_t));
	return buf;
}

/* service channel: where the secure OS wants to be scheduled */
struct iw_service_channel {
	uint32_t cpu_mask;
	uint32_t user_cpu_mask;
} __attribute__((packed));
static volatile struct iw_service_channel *iw_service;

static uint32_t
tz_cpu_mask(void)
{
	return iw_service ? iw_service->cpu_mask : 0;
}

static uint32_t
tz_user_cpu_mask(void)
{
	return iw_service ? iw_service->user_cpu_mask : 0;
}

/* ---- circular buffers (lib/circ_buf.c, circ_buf_packet.c) -------------------------------------------- */
struct circ_buf {
	uint32_t write_count;
	uint32_t read_count;
	char buffer[];
} __attribute__((packed));
#define CIRC_BUF_META_SIZE      (sizeof(struct circ_buf) + 1)

struct circ_buf_desc {
	struct circ_buf *circ_buf;
	unsigned int size;
	unsigned int write_count;
	unsigned int read_count;
};

enum { CIRC_BUF_MODE_KERNEL, CIRC_BUF_MODE_DROP = 2 };

static unsigned long
circ_buf_total_size(unsigned long size)
{
	return TZ_ROUND_UP(size + CIRC_BUF_META_SIZE, sizeof(uint32_t));
}

static struct circ_buf *
circ_buf_set(void *buf)
{
	struct circ_buf *c = buf;

	c->write_count = 0;
	c->read_count = 0;
	return c;
}

static void
circ_buf_connect(struct circ_buf_desc *d, struct circ_buf *c, unsigned long size)
{
	d->circ_buf = c;
	d->read_count = c->read_count;
	d->write_count = c->write_count;
	d->size = (unsigned)size;
}

static ssize_t
circ_buf_write_local(struct circ_buf_desc *d, const char *buf, size_t length)
{
	size_t avail, cur = 0, cur_length = length, bytes_used;
	unsigned int size = d->size, end = d->size + 1, new_count;
	unsigned int write_count_orig = d->write_count;
	unsigned int write_count_shared = d->circ_buf->write_count;
	unsigned int read_count_shared = d->circ_buf->read_count;

	if (write_count_shared > d->size || read_count_shared > d->size) {
		return -EIO;
	}
	d->read_count = read_count_shared;
	if (d->write_count >= d->read_count) {
		avail = size - (d->write_count - d->read_count);
		bytes_used = write_count_shared - d->read_count;
	} else {
		avail = d->read_count - d->write_count - 1;
		bytes_used = end - d->read_count + write_count_shared;
	}
	if (avail + bytes_used < cur_length) {
		d->write_count = write_count_orig;
		return -EMSGSIZE;
	}
	if (avail < cur_length) {
		d->write_count = write_count_orig;
		return -EAGAIN;
	}
	if (d->write_count >= d->read_count) {
		avail = end - d->write_count - (d->read_count ? 0 : 1);
		cur = MIN(cur_length, avail);
		memcpy(d->circ_buf->buffer + d->write_count, buf, cur);
		new_count = d->write_count + (unsigned)cur;
		d->write_count = (new_count == end ? 0 : new_count);
		buf += cur;
		cur_length -= cur;
	}
	memcpy(d->circ_buf->buffer + d->write_count, buf, cur_length);
	d->write_count += (unsigned)cur_length;
	return (ssize_t)length;
}

static ssize_t
__circ_buf_read_local(struct circ_buf_desc *d, char *buf, size_t length, size_t offset, int mode)
{
	size_t avail, cur = 0, cur_length = length, bytes_free;
	unsigned int end = d->size + 1, new_count;
	unsigned int read_count_orig = d->read_count;
	unsigned int write_count_shared = d->circ_buf->write_count;
	unsigned int read_count_shared = d->circ_buf->read_count;

	if (write_count_shared > d->size || read_count_shared > d->size) {
		return -EIO;
	}
	d->write_count = write_count_shared;
	d->read_count += (unsigned)offset;
	if (d->write_count >= d->read_count) {
		avail = d->write_count - d->read_count;
		bytes_free = d->size - d->write_count + read_count_shared;
	} else {
		avail = end - (d->read_count - d->write_count);
		bytes_free = read_count_shared - d->write_count - 1;
	}
	if (avail + bytes_free < cur_length) {
		d->read_count = read_count_orig;
		return -EMSGSIZE;
	}
	if (avail < cur_length) {
		d->read_count = read_count_orig;
		return -EAGAIN;
	}
	tz_rmb();
	if (d->write_count < d->read_count) {
		avail = end - d->read_count;
		cur = MIN(cur_length, avail);
		if (mode == CIRC_BUF_MODE_KERNEL) {
			memcpy(buf, d->circ_buf->buffer + d->read_count, cur);
		}
		new_count = d->read_count + (unsigned)cur;
		d->read_count = (new_count == end ? 0 : new_count);
		if (buf != NULL) {
			buf += cur;
		}
		cur_length -= cur;
	}
	if (mode == CIRC_BUF_MODE_KERNEL) {
		memcpy(buf, d->circ_buf->buffer + d->read_count, cur_length);
	}
	d->read_count += (unsigned)cur_length;
	return (ssize_t)length;
}

static void
circ_buf_flush_write(struct circ_buf_desc *d)
{
	tz_wmb();
	d->circ_buf->write_count = d->write_count;
}

static void
circ_buf_flush_read(struct circ_buf_desc *d)
{
	d->circ_buf->read_count = d->read_count;
}

static void
circ_buf_rollback_write(struct circ_buf_desc *d)
{
	d->write_count = d->circ_buf->write_count;
}

static size_t
circ_buf_bytes_used(struct circ_buf_desc *d)
{
	unsigned int w = d->circ_buf->write_count, r = d->circ_buf->read_count;

	return w >= r ? w - r : d->size + 1 - r + w;
}

static bool
circ_buf_is_empty(struct circ_buf_desc *d)
{
	return circ_buf_bytes_used(d) == 0;
}

static ssize_t
circ_buf_read(struct circ_buf_desc *d, char *buf, size_t length)
{
	ssize_t ret = __circ_buf_read_local(d, buf, length, 0, CIRC_BUF_MODE_KERNEL);

	if (!IS_ERR_VALUE(ret)) {
		circ_buf_flush_read(d);
	}
	return ret;
}

struct circ_buf_packet_header {
	uint32_t packet_size;
} __attribute__((packed));

static size_t
circ_buf_size_for_packet(size_t size)
{
	return size + sizeof(struct circ_buf_packet_header);
}

static ssize_t
circ_buf_write_packet_local(struct circ_buf_desc *d, const char *buf, size_t length)
{
	struct circ_buf_packet_header header = { .packet_size = (uint32_t)length };
	unsigned int orig = d->write_count;
	ssize_t ret = circ_buf_write_local(d, (const char *)&header, sizeof(header));

	if (IS_ERR_VALUE(ret)) {
		return ret;
	}
	ret = circ_buf_write_local(d, buf, length);
	if (IS_ERR_VALUE(ret)) {
		d->write_count = orig;
	}
	return ret;
}

static ssize_t
circ_buf_write_packet(struct circ_buf_desc *d, const char *buf, size_t length)
{
	ssize_t ret = circ_buf_write_packet_local(d, buf, length);

	if (!IS_ERR_VALUE(ret)) {
		circ_buf_flush_write(d);
	}
	return ret;
}

static ssize_t
circ_buf_read_packet_local(struct circ_buf_desc *d, char *buf, size_t length)
{
	struct circ_buf_packet_header header;
	unsigned int orig = d->read_count;
	ssize_t ret = __circ_buf_read_local(d, (char *)&header, sizeof(header), 0, CIRC_BUF_MODE_KERNEL);

	if (IS_ERR_VALUE(ret)) {
		return ret;
	}
	if (header.packet_size > length) {
		d->read_count = orig;
		return -EMSGSIZE;
	}
	ret = __circ_buf_read_local(d, buf, header.packet_size, 0, CIRC_BUF_MODE_KERNEL);
	if (IS_ERR_VALUE(ret)) {
		d->read_count = orig;
	}
	return ret;
}

static ssize_t
circ_buf_read_packet(struct circ_buf_desc *d, char *buf, size_t length)
{
	ssize_t ret = circ_buf_read_packet_local(d, buf, length);

	if (!IS_ERR_VALUE(ret)) {
		circ_buf_flush_read(d);
	}
	return ret;
}

static ssize_t
circ_buf_drop_packet(struct circ_buf_desc *d)
{
	struct circ_buf_packet_header header;
	unsigned int orig = d->read_count;
	ssize_t ret = __circ_buf_read_local(d, (char *)&header, sizeof(header), 0, CIRC_BUF_MODE_KERNEL);

	if (IS_ERR_VALUE(ret)) {
		return ret;
	}
	ret = __circ_buf_read_local(d, NULL, header.packet_size, 0, CIRC_BUF_MODE_DROP);
	if (IS_ERR_VALUE(ret)) {
		d->read_count = orig;
	} else {
		circ_buf_flush_read(d);
	}
	return ret;
}

/* ---- the secure world's log (iwlog.c) ------------------------------------------------------------------- */
#define TZLOG_PG_CNT            8u
#define TZ_IWLOG_BUF_SIZE       (TZLOG_PG_CNT * TZ_PAGE - sizeof(struct circ_buf))
#define TZ_IWLOG_LINE_MAX_LEN   256
static struct circ_buf *iwlog_buf[TZ_MAX_CPUS];
static char iwlog_line[TZ_MAX_CPUS][TZ_IWLOG_LINE_MAX_LEN + 1];
static unsigned iwlog_len[TZ_MAX_CPUS];
static int iwlog_wq, iwlog_request;
static uint64_t iwlog_lines;

static void
tz_iwlog_print(unsigned cpu, const char *buf, unsigned count)
{
	while (count) {
		unsigned avail = TZ_IWLOG_LINE_MAX_LEN - iwlog_len[cpu], in, out;
		const char *p = memchr(buf, '\n', count);
		bool wait_more = false;

		if (p != NULL) {
			if ((unsigned)(p - buf) > avail) {
				in = out = avail;
			} else {
				in = (unsigned)(p - buf) + 1;
				out = (unsigned)(p - buf);
			}
		} else if (count >= avail) {
			in = out = avail;
		} else {
			in = out = count;
			wait_more = true;
		}
		memcpy(&iwlog_line[cpu][iwlog_len[cpu]], buf, out);
		iwlog_len[cpu] += out;
		if (wait_more) {
			break;
		}
		iwlog_line[cpu][iwlog_len[cpu]] = 0;
		printf("SW> %s\n", iwlog_line[cpu]);
		iwlog_lines++;
		iwlog_len[cpu] = 0;
		count -= in;
		buf += in;
	}
}

static void
tz_iwlog_read_buffers(void)
{
	for (unsigned i = 0; i < tee_ncpus; i++) {
		struct circ_buf *b = iwlog_buf[i];
		uint32_t write_count, count;

		if (b == NULL) {
			continue;
		}
		write_count = b->write_count;
		if (write_count > TZ_IWLOG_BUF_SIZE || b->read_count > TZ_IWLOG_BUF_SIZE) {
			continue;
		}
		if (write_count < b->read_count) {
			count = (uint32_t)TZ_IWLOG_BUF_SIZE - b->read_count;
			if (count) {
				tz_iwlog_print(i, b->buffer + b->read_count, count);
				b->read_count = 0;
			}
		}
		count = write_count - b->read_count;
		if (count) {
			tz_iwlog_print(i, b->buffer + b->read_count, count);
			b->read_count += count;
		}
	}
}

static void
tz_iwlog_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		if (!__atomic_exchange_n(&iwlog_request, 0, __ATOMIC_ACQ_REL)) {
			assert_wait_timeout((event_t)&iwlog_wq, THREAD_UNINT, 200, 1000 * NSEC_PER_USEC);
			thread_block(THREAD_CONTINUE_NULL);
		}
		tz_iwlog_read_buffers();
	}
}

/* ---- events (event.c) ----------------------------------------------------------------------------------- */
struct iwd_events_buf {
	unsigned int size;
	int32_t num_events;
	uint32_t events[];
};

static void
tz_event_init(struct iwd_events_buf *b, unsigned size)
{
	b->size = size;
	b->num_events = 0;
	memset(b->events, 0, TZ_ROUND_UP(size, BITS_PER_INT) / BITS_PER_INT * sizeof(uint32_t));
}

static int
tz_event_get_num_pending(struct iwd_events_buf *b)
{
	return __atomic_load_n(&b->num_events, __ATOMIC_ACQUIRE);
}

static int
tz_event_add(struct iwd_events_buf *b, unsigned id)
{
	uint32_t bit = 1u << (id % BITS_PER_INT), prev;

	if (id >= b->size) {
		return 0;
	}
	prev = __atomic_fetch_or(&b->events[id / BITS_PER_INT], bit, __ATOMIC_SEQ_CST);
	if (!(prev & bit)) {
		tz_wmb();
		__atomic_fetch_add(&b->num_events, 1, __ATOMIC_SEQ_CST);
		return 1;
	}
	return 0;
}

static void
tz_event_process(struct iwd_events_buf *b, void (*callback)(unsigned id))
{
	for (unsigned i = 0; i < TZ_ROUND_UP(b->size, BITS_PER_INT) / BITS_PER_INT; i++) {
		uint32_t tmp;

		if (!__atomic_load_n(&b->num_events, __ATOMIC_ACQUIRE)) {
			return;
		}
		tz_rmb();
		tmp = __atomic_exchange_n(&b->events[i], 0, __ATOMIC_SEQ_CST);
		if (!tmp) {
			continue;
		}
		tz_wmb();
		__atomic_fetch_sub(&b->num_events, __builtin_popcount(tmp), __ATOMIC_SEQ_CST);
		for (unsigned j = 0; j < BITS_PER_INT; j++) {
			if (tmp & (1u << j)) {
				callback(i * BITS_PER_INT + j);
			}
		}
	}
}

/* ---- the scheduler (kthread_pool_smpboot.c) ---------------------------------------------------------- */
static int32_t tz_nr_cmds;
static int tz_cmd_wq;
static uint64_t tz_events_irq, tz_panic_irq;

static void
tz_kthread_pool_wake_up_all(void)
{
	tz_wake(&tz_cmd_wq);
}

static void
tz_kthread_pool_cmd_send(void)
{
	__atomic_fetch_add(&tz_nr_cmds, 1, __ATOMIC_SEQ_CST);
	tz_kthread_pool_wake_up_all();
}

static bool
tz_kthread_pool_cmd_get(void)
{
	int32_t v = __atomic_load_n(&tz_nr_cmds, __ATOMIC_ACQUIRE);

	while (v > 0) {
		if (__atomic_compare_exchange_n(&tz_nr_cmds, &v, v - 1, false, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE)) {
			return true;
		}
	}
	return false;
}

static uint32_t tz_worker_mask;         /* CPUs whose worker runs */

static bool
tz_worker_should_run(unsigned cpu)
{
	uint32_t requested = tz_cpu_mask();
	uint32_t effective = requested & tz_worker_mask;
	uint32_t outstanding = requested & ~effective;

	if (effective & (1u << cpu)) {
		tz_kthread_pool_cmd_get();
		return true;
	}
	if (outstanding) {
		tz_kthread_pool_cmd_get();
		return true;
	}
	return tz_kthread_pool_cmd_get();
}

static void
tz_worker(void *arg, __unused wait_result_t wr)
{
	unsigned cpu = (unsigned)(uintptr_t)arg;

	tz_on_cpu(cpu, true);
	__atomic_fetch_or(&tz_worker_mask, 1u << cpu, __ATOMIC_SEQ_CST);
	for (;;) {
		if (!tz_worker_should_run(cpu)) {
			tz_sleep_on(&tz_cmd_wq);
			continue;
		}
		tz_smc(TZ_SMC_SCHEDULE, 0, 0, 0);
		__atomic_fetch_add(&tee_schedules, 1, __ATOMIC_RELAXED);
	}
}

/* from a thread that just queued work for the secure world (a socket write): enter it here when allowed */
static void
tz_kthread_pool_enter_swd(void)
{
	uint32_t mask, user;
	unsigned cpu;

	disable_preemption();
	mask = tz_cpu_mask();
	tz_rmb();
	user = tz_user_cpu_mask();
	cpu = cpu_number();
	if (!user || (user & (1u << cpu)) || (mask & (1u << cpu))) {
		tz_kthread_pool_wake_up_all();
		tz_smc(TZ_SMC_SCHEDULE, 0, 0, 0);
		__atomic_fetch_add(&tee_schedules, 1, __ATOMIC_RELAXED);
	} else {
		tz_kthread_pool_cmd_send();
	}
	enable_preemption();
}

static void
tz_event_irq(__unused void *ctx, uint32_t intid)
{
	if (intid == 254) {
		tz_events_irq++;
		tz_kthread_pool_cmd_send();
	} else {
		tz_panic_irq++;
		gic_v3_spi_mask(intid);
	}
}

/* ---- inter-world sockets (iwsock.c) ------------------------------------------------------------------- */
#define MAX_NUM_SOCKETS                 (BITS_PER_INT * 16)
#define IWD_NUM_SWD_CONNECT_REQUESTS    256
#define DEFAULT_MAX_MSG_SIZE            0
#define DEFAULT_OOB_BUFFER_SIZE         128
#define DEFAULT_TRANSMIT_BUFFER_SIZE    \
	TZ_ROUND_DOWN((TZ_PAGE - 3 * TZ_ROUND_UP(CIRC_BUF_META_SIZE, sizeof(int32_t)) - 2 * sizeof(int) - \
	    DEFAULT_OOB_BUFFER_SIZE) / 2, sizeof(int32_t))
#define TZ_FILE_NAME_LEN                80
#define TZ_SCM_CREDENTIALS              2
#define SOL_UNSPEC_TZ                   0xFFFE

enum buf_endpoint_state { BUF_SK_CLOSED, BUF_SK_CONNECTED, BUF_SK_NEW };
enum tz_endpoint_state { TZ_SK_NEW, TZ_SK_LISTENING, TZ_SK_CONNECT_IN_PROGRESS, TZ_SK_CONNECTED, TZ_SK_RELEASED };
enum operation { CONNECT_FROM_NWD, NEW_NWD_LISTEN, ACCEPT_FROM_NWD };

struct iwd_sock_buf_head {
	uint32_t nwd_state;
	uint32_t swd_state;
} __attribute__((packed));

struct tz_cmsghdr {
	uint32_t cmsg_len;
	uint32_t cmsg_level;
	uint32_t cmsg_type;
};

struct tz_cmsghdr_cred {
	struct tz_cmsghdr cmsghdr;
	struct tz_cred cred;
};

struct tz_swd_cred {
	uint32_t pid;
	uint32_t uid;
	uint32_t gid;
	struct tz_uuid uuid;
} __attribute__((packed));

struct tz_cmsghdr_swd_cred {
	struct tz_cmsghdr cmsghdr;
	struct tz_swd_cred cred;
};

struct sock_desc {
	volatile struct iwd_sock_buf_head *iwd_buf;
	unsigned iwd_pages;
	struct circ_buf_desc write_buf;
	struct circ_buf_desc read_buf;
	struct circ_buf_desc oob_buf;
	int wq;
	int id;
	int state;
	int32_t ref_count;
	lck_mtx_t *lock;
	char listen_name[TZ_FILE_NAME_LEN];
	struct tz_cmsghdr_cred cred;
	unsigned int snd_buf_size;
	unsigned int rcv_buf_size;
	unsigned int oob_buf_size;
	unsigned int max_msg_size;
	unsigned int reply_timeout_ms;  /* tee_xmit's wait for a reply on this socket; 0 = for ever */
};

struct iwd_sock_events_buf {
	struct { unsigned int size; int32_t num_events; uint32_t events[MAX_NUM_SOCKETS / BITS_PER_INT]; } nwd_events;
	struct { unsigned int size; int32_t num_events; uint32_t events[MAX_NUM_SOCKETS / BITS_PER_INT]; } swd_events;
	struct { unsigned int size; int32_t num_events; uint32_t events[IWD_NUM_SWD_CONNECT_REQUESTS / BITS_PER_INT]; }
	accept_err_events;
} __attribute__((packed));

struct iwd_sock_connect_request {
	int32_t swd_id;
	uint32_t snd_buf_size;
	uint32_t rcv_buf_size;
	uint32_t oob_buf_size;
	uint32_t max_msg_size;
} __attribute__((packed));

struct connect_ext_data {
	enum operation op;
	struct sock_desc *sd;
	const char *name;
	int swd_id;
};

static struct sock_desc *sock_map[MAX_NUM_SOCKETS];
LCK_MTX_DECLARE(sock_map_lock, &tee_lck_grp);
static struct iwd_sock_events_buf *sock_events;
static int tz_iwsock_wq;
static int tz_iwsock_ready;

static const uint8_t kernel_client_hash[32] = {
	0x49, 0x43, 0x54, 0xd8, 0x88, 0x45, 0x37, 0xaa, 0x95, 0xff, 0x73, 0x57, 0x22, 0x07, 0xc9, 0x01,
	0x60, 0x09, 0x57, 0x37, 0xe5, 0x42, 0x6c, 0x5f, 0xfa, 0x94, 0x79, 0x6b, 0xc9, 0x37, 0x39, 0x7e
};

/* the client credential: Samsung's kernel-client form unless the caller set another (tee_set_client) */
static struct tz_cred tee_client_cred;
static bool tee_client_cred_set;

static void
tz_format_cred(struct tz_cred *cred)
{
	if (tee_client_cred_set) {
		*cred = tee_client_cred;
		return;
	}
	cred->pid = 0;
	cred->uid = 0;
	cred->gid = 0;
	memcpy(cred->hash, kernel_client_hash, sizeof(cred->hash));
}

static void
tz_iwsock_put_sd(struct sock_desc *sd)
{
	if (__atomic_sub_fetch(&sd->ref_count, 1, __ATOMIC_SEQ_CST) == 0) {
		/* (the channel's pages stay: the secure world may still look at a closed buffer) */
		lck_mtx_free(sd->lock, &tee_lck_grp);
		kfree_type(struct sock_desc, sd);
	}
}

static struct sock_desc *
tz_iwsock_get_sd_by_sid(unsigned sid)
{
	struct sock_desc *sd = NULL;

	lck_mtx_lock(&sock_map_lock);
	if (sid < MAX_NUM_SOCKETS && (sd = sock_map[sid]) != NULL) {
		__atomic_fetch_add(&sd->ref_count, 1, __ATOMIC_SEQ_CST);
	}
	lck_mtx_unlock(&sock_map_lock);
	return sd;
}

static int
tz_iwsock_publish_sd(struct sock_desc *sd)
{
	int sid = -ENOMEM;

	lck_mtx_lock(&sock_map_lock);
	for (int i = 0; i < (int)MAX_NUM_SOCKETS; i++) {
		if (sock_map[i] == NULL) {
			sock_map[i] = sd;
			sd->id = i;
			__atomic_fetch_add(&sd->ref_count, 1, __ATOMIC_SEQ_CST);
			sid = i;
			break;
		}
	}
	lck_mtx_unlock(&sock_map_lock);
	return sid;
}

static void
tz_iwsock_unpublish_sd(struct sock_desc *sd)
{
	lck_mtx_lock(&sock_map_lock);
	if (sd->id >= 0 && sd->id < (int)MAX_NUM_SOCKETS && sock_map[sd->id] == sd) {
		sock_map[sd->id] = NULL;
	}
	lck_mtx_unlock(&sock_map_lock);
	tz_iwsock_put_sd(sd);
}

static unsigned long
connected_iwd_buf_total_size(struct sock_desc *sd)
{
	return TZ_ROUND_UP(sizeof(struct iwd_sock_buf_head) + circ_buf_total_size(sd->snd_buf_size) +
	           circ_buf_total_size(sd->rcv_buf_size) + circ_buf_total_size(sd->oob_buf_size), TZ_PAGE);
}

static unsigned long
listen_iwd_buf_total_size(struct sock_desc *sd)
{
	return TZ_ROUND_UP(sizeof(struct iwd_sock_buf_head) + circ_buf_total_size(sd->snd_buf_size) +
	           circ_buf_total_size(sd->rcv_buf_size), TZ_PAGE);
}

static int
tz_iwsock_pre_connect_callback(void *buf, __unused unsigned long num_pages, void *ext)
{
	struct connect_ext_data *data = ext;
	struct sock_desc *sd = data->sd;
	struct iwd_sock_buf_head *iwd_buf = buf;
	struct circ_buf *snd_buf = (struct circ_buf *)((char *)iwd_buf + sizeof(*iwd_buf));
	struct circ_buf *rcv_buf = (struct circ_buf *)((char *)iwd_buf + sizeof(*iwd_buf) +
	    circ_buf_total_size(sd->snd_buf_size));
	int32_t nwd_id = sd->id;
	uint32_t op = data->op;
	ssize_t ret;

	sd->iwd_buf = buf;
	circ_buf_connect(&sd->write_buf, circ_buf_set(snd_buf), sd->snd_buf_size);
	circ_buf_connect(&sd->read_buf, circ_buf_set(rcv_buf), sd->rcv_buf_size);
	iwd_buf->nwd_state = BUF_SK_CONNECTED;
	iwd_buf->swd_state = BUF_SK_NEW;

	if (IS_ERR_VALUE(ret = circ_buf_write_packet(&sd->write_buf, (char *)&op, sizeof(op)))) {
		return (int)ret;
	}
	if (IS_ERR_VALUE(ret = circ_buf_write_packet(&sd->write_buf, (char *)&nwd_id, sizeof(nwd_id)))) {
		return (int)ret;
	}
	switch (data->op) {
	case CONNECT_FROM_NWD:
	case NEW_NWD_LISTEN:
		ret = circ_buf_write_packet(&sd->write_buf, data->name, strlen(data->name) + 1);
		break;
	case ACCEPT_FROM_NWD:
		ret = circ_buf_write_packet(&sd->write_buf, (char *)&data->swd_id, sizeof(data->swd_id));
		break;
	}
	if (IS_ERR_VALUE(ret)) {
		return (int)ret;
	}
	if (IS_ERR_VALUE(ret = circ_buf_write_packet(&sd->write_buf, (char *)&sd->snd_buf_size, sizeof(sd->snd_buf_size)))) {
		return (int)ret;
	}
	if (IS_ERR_VALUE(ret = circ_buf_write_packet(&sd->write_buf, (char *)&sd->rcv_buf_size, sizeof(sd->rcv_buf_size)))) {
		return (int)ret;
	}
	if (data->op == CONNECT_FROM_NWD || data->op == ACCEPT_FROM_NWD) {
		struct circ_buf *oob_buf = (struct circ_buf *)((char *)iwd_buf + sizeof(*iwd_buf) +
		    circ_buf_total_size(sd->snd_buf_size) + circ_buf_total_size(sd->rcv_buf_size));

		circ_buf_connect(&sd->oob_buf, circ_buf_set(oob_buf), sd->oob_buf_size);
		ret = circ_buf_write_packet(&sd->write_buf, (char *)&sd->oob_buf_size, sizeof(sd->oob_buf_size));
		if (IS_ERR_VALUE(ret)) {
			return (int)ret;
		}
	}
	return 0;
}

static void
tz_iwsock_notify_swd(int32_t sid)
{
	if (tz_event_add((struct iwd_events_buf *)&sock_events->nwd_events, (unsigned)sid)) {
		tz_kthread_pool_enter_swd();
	}
}

static void
tz_iwsock_notify_internally(int32_t sid)
{
	if (tz_event_add((struct iwd_events_buf *)&sock_events->swd_events, (unsigned)sid)) {
		tz_wake(&tz_iwsock_wq);
	}
}

static struct sock_desc *
tz_iwsock_socket(void)
{
	struct sock_desc *sd = kalloc_type(struct sock_desc, Z_WAITOK | Z_ZERO);

	if (sd == NULL) {
		return NULL;
	}
	sd->ref_count = 1;
	sd->lock = lck_mtx_alloc_init(&tee_lck_grp, LCK_ATTR_NULL);
	sd->cred.cmsghdr.cmsg_len = (uint32_t)(TZ_ROUND_UP(sizeof(struct tz_cmsghdr), sizeof(uint32_t)) + sizeof(struct tz_cred));
	sd->cred.cmsghdr.cmsg_level = SOL_UNSPEC_TZ;
	sd->cred.cmsghdr.cmsg_type = TZ_SCM_CREDENTIALS;
	tz_format_cred(&sd->cred.cred);
	sd->snd_buf_size = DEFAULT_TRANSMIT_BUFFER_SIZE;
	sd->rcv_buf_size = DEFAULT_TRANSMIT_BUFFER_SIZE;
	sd->oob_buf_size = DEFAULT_OOB_BUFFER_SIZE;
	sd->max_msg_size = DEFAULT_MAX_MSG_SIZE;
	sd->state = TZ_SK_NEW;
	sd->id = -1;
	sd->reply_timeout_ms = 20000;
	if (tz_iwsock_publish_sd(sd) < 0) {
		tz_iwsock_put_sd(sd);
		return NULL;
	}
	return sd;
}

static int
tz_iwsock_connect(struct sock_desc *sd, const char *name)
{
	struct connect_ext_data ext = { .op = CONNECT_FROM_NWD, .sd = sd, .name = name };
	unsigned pages;
	void *buf;
	uint64_t deadline_ms = 0;

	if (!tz_iwsock_ready) {
		return -ENODEV;
	}
	if (strnlen(name, TZ_FILE_NAME_LEN) == TZ_FILE_NAME_LEN) {
		return -ENAMETOOLONG;
	}
	lck_mtx_lock(sd->lock);
	if (sd->state != TZ_SK_NEW) {
		lck_mtx_unlock(sd->lock);
		return sd->state == TZ_SK_CONNECTED ? -EISCONN : -EALREADY;
	}
	sd->state = TZ_SK_CONNECT_IN_PROGRESS;
	pages = (unsigned)(connected_iwd_buf_total_size(sd) / TZ_PAGE);
	buf = tz_iwio_alloc_iw_channel(TZ_IWIO_CONNECT_SOCKET, pages, tz_iwsock_pre_connect_callback, &ext);
	if (buf == NULL) {
		sd->state = TZ_SK_NEW;
		sd->iwd_buf = NULL;
		lck_mtx_unlock(sd->lock);
		return -ECONNREFUSED;
	}
	sd->iwd_pages = pages;
	lck_mtx_unlock(sd->lock);

	/* blocking: until the secure world answers the buffer (5 s at most here) */
	for (;;) {
		tz_rmb();
		if (sd->iwd_buf->swd_state != BUF_SK_NEW) {
			break;
		}
		if (++deadline_ms > 5000 / TZ_WAIT_MS) {
			return -ETIMEDOUT;
		}
		tz_sleep_on(&sd->wq);
	}
	lck_mtx_lock(sd->lock);
	if (sd->iwd_buf->swd_state == BUF_SK_CONNECTED) {
		sd->state = TZ_SK_CONNECTED;
		lck_mtx_unlock(sd->lock);
		return 0;
	}
	lck_mtx_unlock(sd->lock);
	return -ECONNREFUSED;
}

static int
tz_iwsock_listen(struct sock_desc *sd, const char *name)
{
	struct connect_ext_data ext = { .op = NEW_NWD_LISTEN, .sd = sd, .name = name };
	void *buf;
	unsigned pages;

	if (!tz_iwsock_ready) {
		return -ENODEV;
	}
	lck_mtx_lock(sd->lock);
	if (sd->state != TZ_SK_NEW) {
		lck_mtx_unlock(sd->lock);
		return -EBADF;
	}
	strlcpy(sd->listen_name, name, sizeof(sd->listen_name));
	sd->snd_buf_size = DEFAULT_TRANSMIT_BUFFER_SIZE;
	sd->rcv_buf_size = DEFAULT_TRANSMIT_BUFFER_SIZE;
	pages = (unsigned)(listen_iwd_buf_total_size(sd) / TZ_PAGE);
	buf = tz_iwio_alloc_iw_channel(TZ_IWIO_CONNECT_SOCKET, pages, tz_iwsock_pre_connect_callback, &ext);
	if (buf == NULL) {
		sd->iwd_buf = NULL;
		lck_mtx_unlock(sd->lock);
		return -ECONNREFUSED;
	}
	sd->iwd_pages = pages;
	sd->state = TZ_SK_LISTENING;
	lck_mtx_unlock(sd->lock);
	return 0;
}

static struct sock_desc *
tz_iwsock_accept(struct sock_desc *sd)
{
	struct connect_ext_data ext = { .op = ACCEPT_FROM_NWD };
	struct iwd_sock_connect_request req;
	struct sock_desc *new_sd;
	ssize_t ret;
	void *buf;

	for (;;) {
		lck_mtx_lock(sd->lock);
		if (!tz_iwsock_ready || sd->state != TZ_SK_LISTENING) {
			lck_mtx_unlock(sd->lock);
			return NULL;
		}
		if (!circ_buf_is_empty(&sd->read_buf)) {
			ret = circ_buf_read(&sd->read_buf, (char *)&req, sizeof(req));
			lck_mtx_unlock(sd->lock);
			break;
		}
		lck_mtx_unlock(sd->lock);
		tz_sleep_on(&sd->wq);
	}
	if (ret != sizeof(req)) {
		return NULL;
	}
	if ((new_sd = tz_iwsock_socket()) == NULL) {
		goto refuse;
	}
	new_sd->snd_buf_size = (unsigned)(circ_buf_size_for_packet(req.snd_buf_size) +
	    circ_buf_size_for_packet(sizeof(struct tz_cmsghdr_cred)));
	new_sd->rcv_buf_size = (unsigned)(circ_buf_size_for_packet(req.rcv_buf_size) +
	    circ_buf_size_for_packet(sizeof(struct tz_cmsghdr_cred)));
	new_sd->oob_buf_size = (unsigned)(circ_buf_size_for_packet(req.oob_buf_size) +
	    circ_buf_size_for_packet(sizeof(struct tz_cmsghdr_cred)));
	if (req.max_msg_size) {
		new_sd->max_msg_size = (unsigned)(circ_buf_size_for_packet(req.max_msg_size) +
		    circ_buf_size_for_packet(sizeof(struct tz_cmsghdr_cred)));
	}
	ext.swd_id = req.swd_id;
	ext.sd = new_sd;
	new_sd->iwd_pages = (unsigned)(connected_iwd_buf_total_size(new_sd) / TZ_PAGE);
	buf = tz_iwio_alloc_iw_channel(TZ_IWIO_CONNECT_SOCKET, new_sd->iwd_pages, tz_iwsock_pre_connect_callback, &ext);
	if (buf == NULL) {
		new_sd->iwd_buf = NULL;
		tz_iwsock_unpublish_sd(new_sd);
		tz_iwsock_put_sd(new_sd);
		goto refuse;
	}
	new_sd->state = TZ_SK_CONNECTED;
	return new_sd;
refuse:
	if (tz_event_add((struct iwd_events_buf *)&sock_events->accept_err_events, (unsigned)req.swd_id)) {
		tz_kthread_pool_enter_swd();
	}
	return NULL;
}

static void
tz_iwsock_release(struct sock_desc *sd)
{
	bool notify_swd = false, notify_nwd = false;

	lck_mtx_lock(sd->lock);
	switch (sd->state) {
	case TZ_SK_NEW:
		lck_mtx_unlock(sd->lock);
		tz_iwsock_unpublish_sd(sd);
		tz_iwsock_put_sd(sd);
		return;
	case TZ_SK_LISTENING:
	case TZ_SK_CONNECT_IN_PROGRESS:
	case TZ_SK_CONNECTED:
		if (sd->iwd_buf != NULL) {
			sd->iwd_buf->nwd_state = BUF_SK_CLOSED;
			tz_mb();
			if (sd->iwd_buf->swd_state != BUF_SK_CLOSED) {
				notify_swd = true;
			} else {
				notify_nwd = true;
			}
		}
		break;
	default:
		break;
	}
	sd->state = TZ_SK_RELEASED;
	lck_mtx_unlock(sd->lock);
	if (notify_swd) {
		tz_iwsock_notify_swd(sd->id);
	} else if (notify_nwd) {
		tz_iwsock_notify_internally(sd->id);
	}
	tz_iwsock_put_sd(sd);
}

static ssize_t
__tz_iwsock_read(struct sock_desc *sd, void *buf, size_t len, bool *need_notify)
{
	uint32_t swd_state;
	ssize_t ret;

	lck_mtx_lock(sd->lock);
	if (sd->state != TZ_SK_CONNECTED) {
		lck_mtx_unlock(sd->lock);
		return -ENOTCONN;
	}
	swd_state = sd->iwd_buf->swd_state;
	tz_rmb();
	ret = circ_buf_drop_packet(&sd->read_buf);         /* the secure world's credential packet */
	if (!IS_ERR_VALUE(ret)) {
		ret = circ_buf_read_packet(&sd->read_buf, buf, len);
		if (sd->max_msg_size) {
			size_t nbytes = circ_buf_size_for_packet(sd->max_msg_size) * 2;

			if (sd->read_buf.size - circ_buf_bytes_used(&sd->read_buf) < nbytes) {
				*need_notify = true;
			}
		} else {
			*need_notify = true;
		}
	}
	if (ret == -EAGAIN && swd_state == BUF_SK_CLOSED) {
		ret = 0;
	}
	lck_mtx_unlock(sd->lock);
	return ret;
}

/* blocking read of one message; timeout_ms 0 = for ever */
static ssize_t
tz_iwsock_read(struct sock_desc *sd, void *buf, size_t count, unsigned timeout_ms)
{
	bool need_notify = false;
	unsigned waited = 0;
	ssize_t ret;

	if (!tz_iwsock_ready) {
		return -ENODEV;
	}
	while ((ret = __tz_iwsock_read(sd, buf, count, &need_notify)) == -EAGAIN) {
		if (timeout_ms && (waited += TZ_WAIT_MS) > timeout_ms) {
			return -ETIMEDOUT;
		}
		tz_sleep_on(&sd->wq);
	}
	if (ret > 0 && need_notify) {
		tz_iwsock_notify_swd(sd->id);
	}
	return ret;
}

static ssize_t
__tz_iwsock_write(struct sock_desc *sd, struct circ_buf_desc *wb, const void *data, size_t data_len)
{
	ssize_t ret;

	lck_mtx_lock(sd->lock);
	if (sd->state != TZ_SK_CONNECTED) {
		lck_mtx_unlock(sd->lock);
		return -ENOTCONN;
	}
	tz_rmb();
	if (sd->iwd_buf->swd_state == BUF_SK_CLOSED) {
		lck_mtx_unlock(sd->lock);
		return -ECONNRESET;
	}
	if (sd->max_msg_size && data_len > sd->max_msg_size) {
		lck_mtx_unlock(sd->lock);
		return -EMSGSIZE;
	}
	ret = circ_buf_write_packet_local(wb, (const char *)&sd->cred, sizeof(struct tz_cmsghdr_cred));
	if (!IS_ERR_VALUE(ret)) {
		ret = circ_buf_write_packet(wb, data, data_len);
		if (IS_ERR_VALUE(ret)) {
			circ_buf_rollback_write(wb);
		}
	}
	lck_mtx_unlock(sd->lock);
	return ret;
}

static ssize_t
tz_iwsock_write(struct sock_desc *sd, const void *buf, size_t count, bool oob)
{
	struct circ_buf_desc *wb = oob ? &sd->oob_buf : &sd->write_buf;
	unsigned waited = 0;
	ssize_t ret;

	if (!tz_iwsock_ready) {
		return -ENODEV;
	}
	while ((ret = __tz_iwsock_write(sd, wb, buf, count)) == -EAGAIN) {
		if ((waited += TZ_WAIT_MS) > 5000) {
			return -ETIMEDOUT;
		}
		tz_sleep_on(&sd->wq);
	}
	if (ret > 0) {
		tz_iwsock_notify_swd(sd->id);
	}
	return ret;
}

static void
tz_iwsock_handle_event(unsigned id)
{
	struct sock_desc *sd = tz_iwsock_get_sd_by_sid(id);

	if (sd != NULL) {
		tz_rmb();
		if (sd->state == TZ_SK_RELEASED && sd->iwd_buf != NULL && sd->iwd_buf->swd_state == BUF_SK_CLOSED) {
			tz_iwsock_unpublish_sd(sd);
		} else {
			tz_wake(&sd->wq);
		}
		tz_iwsock_put_sd(sd);
	}
}

static void
tz_iwsock_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		if (tz_event_get_num_pending((struct iwd_events_buf *)&sock_events->swd_events) <= 0) {
			assert_wait_timeout((event_t)&tz_iwsock_wq, THREAD_UNINT, 50, 1000 * NSEC_PER_USEC);
			thread_block(THREAD_CONTINUE_NULL);
			continue;
		}
		tz_event_process((struct iwd_events_buf *)&sock_events->swd_events, tz_iwsock_handle_event);
	}
}

static int
tz_iwsock_events_pre_connect_callback(void *buf, __unused unsigned long num_pages, __unused void *ext)
{
	struct iwd_sock_events_buf *sb = buf;

	tz_event_init((struct iwd_events_buf *)&sb->nwd_events, MAX_NUM_SOCKETS);
	tz_event_init((struct iwd_events_buf *)&sb->swd_events, MAX_NUM_SOCKETS);
	tz_event_init((struct iwd_events_buf *)&sb->accept_err_events, IWD_NUM_SWD_CONNECT_REQUESTS);
	return 0;
}

/* every SMC's return: the scheduler, the log and the sockets look again (Samsung's post-SMC notifiers) */
static void
tee_post_smc(void)
{
	if (tz_cpu_mask()) {
		tz_kthread_pool_wake_up_all();
	}
	if (__atomic_exchange_n(&iwlog_request, 1, __ATOMIC_ACQ_REL) == 0) {
		tz_wake(&iwlog_wq);
	}
	if (tz_iwsock_ready && tz_event_get_num_pending((struct iwd_events_buf *)&sock_events->swd_events) > 0) {
		tz_wake(&tz_iwsock_wq);
	}
}

/* ---- shared memory (mem.c) ----------------------------------------------------------------------------- */
#define TZ_MEM_IDS              1024
#define TZDEV_IWSHMEM_REG_FLAG_WRITE 1u
LCK_MTX_DECLARE(mem_lock, &tee_lck_grp);
static bool mem_ids[TZ_MEM_IDS];

/* registers `size` bytes at `buf` (4 KiB aligned, wired: tz_alloc) with the secure world; its id, or <0 */
static int
tzdev_mem_register(void *buf, size_t size, bool writable)
{
	const unsigned per_aux = TZ_PAGE / sizeof(uint32_t);
	unsigned nr_pages = (unsigned)(TZ_ROUND_UP(size, TZ_PAGE) / TZ_PAGE);
	unsigned used = (unsigned)(TZ_ROUND_UP(sizeof(struct tz_cred), sizeof(uint32_t)) / sizeof(uint32_t));
	unsigned done = 0;
	struct tz_cred cred;
	int id = -1, ret = 0;

	if (((uintptr_t)buf & (TZ_PAGE - 1)) || nr_pages == 0) {
		return -EINVAL;
	}
	tz_format_cred(&cred);
	lck_mtx_lock(&mem_lock);
	for (int i = 1; i < TZ_MEM_IDS; i++) {
		if (!mem_ids[i]) {
			mem_ids[i] = true;
			id = i;
			break;
		}
	}
	if (id < 0) {
		lck_mtx_unlock(&mem_lock);
		return -ENOMEM;
	}
	while (done < nr_pages) {
		unsigned n = MIN(nr_pages - done, per_aux - used);
		uint8_t *aux = tz_aux_get();

		if (used) {
			memcpy(aux, &cred, sizeof(cred));
		}
		for (unsigned i = 0; i < n; i++) {
			uint32_t pfn = tz_pfn(buf, done + i);

			memcpy(aux + (used + i) * sizeof(uint32_t), &pfn, sizeof(pfn));
		}
		ret = tz_smc(TZ_SMC_SHMEM_LIST_REG, (uint32_t)id, nr_pages, writable ? TZDEV_IWSHMEM_REG_FLAG_WRITE : 0);
		tz_aux_put();
		if (ret != 0) {
			break;
		}
		done += n;
		used = 0;
	}
	if (ret != 0) {
		mem_ids[id] = false;
		lck_mtx_unlock(&mem_lock);
		tee_say("shared memory: SHMEM_LIST_REG answered %d\n", ret);
		return ret < 0 ? ret : -EIO;
	}
	lck_mtx_unlock(&mem_lock);
	return id;
}

/* releases an id; the memory itself is kept by the caller until the secure world is done (here: never freed) */
static int
tzdev_mem_release(int id)
{
	int ret;

	lck_mtx_lock(&mem_lock);
	(void)tz_aux_get();
	ret = tz_smc(TZ_SMC_SHMEM_LIST_RLS, (uint32_t)id, 0, 0);
	tz_aux_put();
	lck_mtx_unlock(&mem_lock);
	return ret;
}

/* ---- REE time (ree_time.c) ---------------------------------------------------------------------------- */
struct tz_ree_time {
	uint32_t sec;
	uint32_t nsec;
} __attribute__((packed));
static uint64_t ree_time_answers;

static void
tz_ree_time_thread(__unused void *arg, __unused wait_result_t wr)
{
	struct sock_desc *listen = tz_iwsock_socket();

	if (listen == NULL || tz_iwsock_listen(listen, "ree_time_socket") != 0) {
		tee_say("REE time: no listening socket\n");
		return;
	}
	for (;;) {
		struct sock_desc *conn = tz_iwsock_accept(listen);

		if (conn == NULL) {
			tz_msleep(100);
			continue;
		}
		for (;;) {
			int req;
			struct timeval tv;
			struct tz_ree_time t;

			if (tz_iwsock_read(conn, &req, sizeof(req), 0) != sizeof(req)) {
				break;
			}
			microtime(&tv);
			t.sec = (uint32_t)tv.tv_sec;
			t.nsec = (uint32_t)tv.tv_usec * 1000;
			if (tz_iwsock_write(conn, &t, sizeof(t), false) != sizeof(t)) {
				break;
			}
			ree_time_answers++;
		}
		tz_iwsock_release(conn);
	}
}

/* ---- secure RPMB (Samsung drivers/scsi/scsi_srpmb.c) -------------------------------------------------- */
/*
 * The secure world's RPMB (ACSD keeps its database of permitted TAs there): it cannot reach the UFS device, so it
 * writes a request into a buffer this side registered (SMC_SRPMB_WSM 0x82003811: its physical address and the
 * interrupt it raises, GIC SPI 0xe1 = INTID 257, ufs-srpmb in exynos9830.dts), raises the interrupt, and polls
 * status_flag; this side runs the RPMB frames against the device's RPMB unit (ufshc_security) and sets it.
 */
#include "redwood_ufshc.h"
extern void FlushPoC_DcacheRegion(vm_offset_t va, unsigned length);

struct rpmb_req {
	uint32_t cmd;
	volatile uint32_t status_flag;
	uint32_t type;
	uint32_t data_len;
	uint32_t inlen;
	uint32_t outlen;
	uint8_t rpmb_data[];
};
#define SRPMB_SMC_WSM           0x82003811u
#define SRPMB_INTID             257
#define RPMB_BUF_MAX_SIZE       (32 * 1024)
#define RPMB_PACKET_SIZE        512
#define RPMB_GET_WRITE_COUNTER  1
#define RPMB_WRITE_DATA         2
#define RPMB_READ_DATA          3
#define RPMB_IN_PROGRESS        0xDCDC
#define RPMB_PASSED             0xBABA
static struct rpmb_req *srpmb;
static size_t srpmb_size;
static int srpmb_wq, srpmb_irqs;
static uint64_t srpmb_requests, srpmb_errors;

static void
srpmb_irq(__unused void *ctx, __unused uint32_t intid)
{
	srpmb->status_flag = RPMB_IN_PROGRESS;
	__atomic_fetch_add(&srpmb_irqs, 1, __ATOMIC_SEQ_CST);
	tz_wake(&srpmb_wq);
}

static void
srpmb_status(uint32_t status)
{
	tz_mb();
	srpmb->status_flag = status;
	FlushPoC_DcacheRegion((vm_offset_t)srpmb, (unsigned)srpmb_size);
}

static void
srpmb_work(void)
{
	struct rpmb_req *req = srpmb;
	uint32_t type, len;
	int e;

	FlushPoC_DcacheRegion((vm_offset_t)srpmb, (unsigned)srpmb_size);
	type = req->type;
	len = req->data_len;
	srpmb_requests++;
	switch (type) {
	case RPMB_GET_WRITE_COUNTER:
		if (len != RPMB_PACKET_SIZE) {
			srpmb_status(0x601);
			break;
		}
		if ((e = ufshc_security(true, req->rpmb_data, RPMB_PACKET_SIZE)) != 0) {
			srpmb_status(0x602);
			break;
		}
		bzero(req->rpmb_data, len);
		if ((e = ufshc_security(false, req->rpmb_data, len)) != 0) {
			srpmb_status(0x603);
			break;
		}
		srpmb_status(RPMB_PASSED);
		break;
	case RPMB_WRITE_DATA: {
		uint8_t packet[RPMB_PACKET_SIZE];

		if (len < RPMB_PACKET_SIZE || len > RPMB_PACKET_SIZE * 64) {
			srpmb_status(0x604);
			break;
		}
		if ((e = ufshc_security(true, req->rpmb_data, len)) != 0) {
			srpmb_status(0x605);
			break;
		}
		/* the result read request: request 0x0005, big-endian at bytes 510..511 (Samsung's swap_packet) */
		bzero(packet, sizeof(packet));
		packet[510] = 0x00;
		packet[511] = 0x05;
		bzero(req->rpmb_data, len);
		memcpy(req->rpmb_data, packet, sizeof(packet));
		if ((e = ufshc_security(true, req->rpmb_data, RPMB_PACKET_SIZE)) != 0) {
			srpmb_status(0x606);
			break;
		}
		bzero(req->rpmb_data, len);
		if ((e = ufshc_security(false, req->rpmb_data, RPMB_PACKET_SIZE)) != 0) {
			srpmb_status(0x607);
			break;
		}
		srpmb_status(RPMB_PASSED);
		break;
	}
	case RPMB_READ_DATA:
		if (len < RPMB_PACKET_SIZE || len > RPMB_PACKET_SIZE * 64) {
			srpmb_status(0x608);
			break;
		}
		if ((e = ufshc_security(true, req->rpmb_data, RPMB_PACKET_SIZE)) != 0) {
			srpmb_status(0x609);
			break;
		}
		bzero(req->rpmb_data, len);
		if ((e = ufshc_security(false, req->rpmb_data, len)) != 0) {
			srpmb_status(0x60A);
			break;
		}
		srpmb_status(RPMB_PASSED);
		break;
	default:
		srpmb_status(0x60B);
		break;
	}
	if (srpmb->status_flag != RPMB_PASSED) {
		srpmb_errors++;
		tee_say("RPMB: request type %u len %u: status %#x\n", type, len, srpmb->status_flag);
	} else if (srpmb_requests <= 8) {
		tee_say("RPMB: request type %u len %u: passed (result %02x%02x)\n", type, len,
		    req->rpmb_data[508], req->rpmb_data[509]);
	}
}

static void
srpmb_thread(__unused void *arg, __unused wait_result_t wr)
{
	int seen = 0;

	for (;;) {
		int now = __atomic_load_n(&srpmb_irqs, __ATOMIC_ACQUIRE);

		if (now == seen) {
			assert_wait_timeout((event_t)&srpmb_wq, THREAD_UNINT, 100, 1000 * NSEC_PER_USEC);
			thread_block(THREAD_CONTINUE_NULL);
			continue;
		}
		seen = now;
		srpmb_work();
	}
}

static int
srpmb_init(void)
{
	vm_offset_t va = 0;
	thread_t t;
	int ret;

	srpmb_size = round_page(sizeof(struct rpmb_req) + RPMB_BUF_MAX_SIZE);
	if (kmem_alloc_contig(kernel_map, &va, srpmb_size, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return -ENOMEM;
	}
	srpmb = (struct rpmb_req *)va;
	FlushPoC_DcacheRegion(va, (unsigned)srpmb_size);
	if (gic_v3_spi_attach_to(SRPMB_INTID, true, srpmb_irq, NULL, 0) != KERN_SUCCESS) {
		return -EBUSY;
	}
	kernel_thread_start(srpmb_thread, NULL, &t);
	thread_deallocate(t);
	ret = tz_smc_raw(SRPMB_SMC_WSM, kvtophys(va), SRPMB_INTID, 0, NULL);
	tee_say("secure RPMB: buffer %#llx (%zu bytes), interrupt %d: SMC_SRPMB_WSM answered %d\n",
	    kvtophys(va), srpmb_size, SRPMB_INTID, ret);
	return ret;
}

static void tee_ts_thread(void *arg, wait_result_t wr);

/* ---- start ------------------------------------------------------------------------------------------------ */
struct tzio_sysconf_nwd {
	uint32_t iwi_event;
	uint32_t iwi_panic;
	uint32_t flags;
} __attribute__((packed));
static struct {
	uint32_t os_version, cpu_num, big_cpus_mask, flags;
	char version[256];
} swd_sysconf;

static void
tee_fail(const char *what, int ret)
{
	snprintf(tee_status, sizeof(tee_status), "TEEGRIS: FAILED at %s (%d)", what, ret);
	tee_say("%s\n", tee_status);
	tee_state = TEE_FAILED;
}

static void
tee_start_thread(__unused void *arg, __unused wait_result_t wr)
{
	thread_t t;
	uint64_t x1 = 0;
	int ret;

	tee_ncpus = MIN(real_ncpus, TZ_MAX_CPUS);

	/* 1. version (run on CPU 0, as the earlier probes) */
	tz_on_cpu(0, true);
	ret = tz_smc_raw(TZ_SMC_CHECK_VERSION, TZ_LINUX_VERSION_CODE, TZ_DRIVER_CODE, 0, &x1);
	if (ret != 0) {
		tz_on_cpu(0, false);
		tee_fail("CHECK_VERSION", ret);
		return;
	}
	/* 2. AUX buffers, one per CPU, each registered from its own CPU */
	for (unsigned cpu = 0; cpu < tee_ncpus; cpu++) {
		if ((aux_buf[cpu] = tz_alloc(TZ_PAGE)) == NULL) {
			tz_on_cpu(0, false);
			tee_fail("AUX allocation", -ENOMEM);
			return;
		}
		tz_on_cpu(cpu, true);
		ret = tz_smc_raw(TZ_SMC_CONNECT_AUX, tz_pfn(aux_buf[cpu], 0), 0, 0, NULL);
		if (ret != 0) {
			tz_on_cpu(0, false);
			tee_fail("CONNECT_AUX", ret);
			return;
		}
	}
	tz_on_cpu(0, false);
	/* 3. service channel */
	if ((iw_service = tz_iwio_alloc_iw_channel(TZ_IWIO_CONNECT_SERVICE, 1, NULL, NULL)) == NULL) {
		tee_fail("service channel", -EIO);
		return;
	}
	/* 4. log rings, one per CPU, and the thread that prints them */
	for (unsigned cpu = 0; cpu < tee_ncpus; cpu++) {
		iwlog_buf[cpu] = tz_iwio_alloc_iw_channel(TZ_IWIO_CONNECT_LOG, TZLOG_PG_CNT, NULL, NULL);
		if (iwlog_buf[cpu] == NULL) {
			tee_fail("log channel", -EIO);
			return;
		}
	}
	kernel_thread_start(tz_iwlog_thread, NULL, &t);
	thread_deallocate(t);
	/* 5. socket events channel, sockets ready */
	sock_events = tz_iwio_alloc_iw_channel(TZ_IWIO_CONNECT_SOCKET_EVENTS,
	    (unsigned)(TZ_ROUND_UP(sizeof(struct iwd_sock_events_buf), TZ_PAGE) / TZ_PAGE),
	    tz_iwsock_events_pre_connect_callback, NULL);
	if (sock_events == NULL) {
		tee_fail("socket events channel", -EIO);
		return;
	}
	kernel_thread_start(tz_iwsock_thread, NULL, &t);
	thread_deallocate(t);
	tz_mb();
	tz_iwsock_ready = 1;
	/* 6. interrupts: the event one stays on (it wakes the scheduler), the panic one masks itself */
	if (gic_v3_spi_attach_to(254, false, tz_event_irq, NULL, 0) != KERN_SUCCESS ||
	    gic_v3_spi_attach_to(255, false, tz_event_irq, NULL, 0) != KERN_SUCCESS) {
		tee_fail("event interrupts 254/255", -EBUSY);
		return;
	}
	/* 7. sysconf */
	{
		struct tzio_sysconf_nwd nwd = { .iwi_event = 254, .iwi_panic = 255, .flags = 0 };
		uint8_t *aux;

		aux = tz_aux_get();
		memcpy(aux, &nwd, sizeof(nwd));
		ret = tz_smc(TZ_SMC_SYSCONF, 0, 0, 0);
		if (ret == 0) {
			memcpy(&swd_sysconf, aux, sizeof(swd_sysconf));
		}
		tz_aux_put();
		if (ret != 0) {
			tee_fail("SYSCONF", ret);
			return;
		}
		swd_sysconf.version[sizeof(swd_sysconf.version) - 1] = 0;
	}
	/* 7b. secure RPMB (before any TA is checked against ACSD's database) */
	(void)srpmb_init();
	/* 8. the scheduler: a worker per CPU */
	for (unsigned cpu = 0; cpu < tee_ncpus; cpu++) {
		kernel_thread_start(tz_worker, (void *)(uintptr_t)cpu, &t);
		thread_deallocate(t);
	}
	/* 9. REE time */
	kernel_thread_start(tz_ree_time_thread, NULL, &t);
	thread_deallocate(t);

	tee_state = TEE_UP;
	kernel_thread_start(tee_ts_thread, NULL, &t);
	thread_deallocate(t);
	snprintf(tee_status, sizeof(tee_status), "TEEGRIS: up; os %#x \"%s\" cpus %u big %#x",
	    swd_sysconf.os_version, swd_sysconf.version, swd_sysconf.cpu_num, swd_sysconf.big_cpus_mask);
	tee_say("%s\n", tee_status);
	tz_kthread_pool_cmd_send();             /* (let the secure world run its start) */
}

/* ---- TEE client (teec/) --------------------------------------------------------------------------------- */
#define IW_STRUCTURE            __attribute__((packed))
#define ROOT_TASK_SOCK          "/service/root_task"
#define IWD_TIMEOUT_INFINITY    ((uint64_t)(-1))
#define PROFILE_STR_LEN         17

struct shared_buffer_description { uint32_t offset, size, id; } IW_STRUCTURE;
struct tz_value { uint32_t a, b; } IW_STRUCTURE;
struct entry_point_params {
	union {
		struct shared_buffer_description shared_buffer_description;
		struct tz_value val;
	};
} IW_STRUCTURE;
struct tee_operation { uint32_t tee_param_types; struct entry_point_params tee_params[4]; } IW_STRUCTURE;
struct cmd_request { uint32_t cmd, serial; } IW_STRUCTURE;
struct cmd_reply { struct cmd_request base; uint32_t result, origin; } IW_STRUCTURE;
struct cmd_reply_initialize_context { struct cmd_reply base; uint32_t ctx_id, group_id; } IW_STRUCTURE;
struct cmd_set_shared_memory_rights { struct cmd_request base; struct shared_buffer_description buf_desc; } IW_STRUCTURE;
struct cmd_prepare_session { struct cmd_request base; struct tz_uuid ta_uuid; uint32_t ctx_id;
	                     struct shared_buffer_description buf_desc; } IW_STRUCTURE;
struct cmd_open_session { struct cmd_request base; uint32_t conn_meth; char conn_data[PROFILE_STR_LEN];
	                  uint32_t fips_enabled; struct tee_operation op; uint64_t cancel_time; } IW_STRUCTURE;
struct cmd_reply_open_session { struct cmd_reply base; struct tz_uuid session_id; struct tee_operation op; } IW_STRUCTURE;
struct cmd_invoke_command { struct cmd_request base; uint32_t tee_cmd_id; struct tee_operation op;
	                    uint64_t cancel_time; } IW_STRUCTURE;
struct cmd_reply_invoke_command { struct cmd_reply base; struct tee_operation op; } IW_STRUCTURE;

enum {
	CMD_INITIALIZE_CONTEXT = 1, CMD_FINALIZE_CONTEXT = 2, CMD_SET_SHMEM_RIGHTS = 3, CMD_PREPARE_SESSION = 4,
	CMD_OPEN_SESSION = 5, CMD_CLOSE_SESSION = 6, CMD_INVOKE_COMMAND = 7,
	CMD_REPLY_INITIALIZE_CONTEXT = 12, CMD_REPLY_FINALIZE_CONTEXT = 13, CMD_REPLY_PREPARE_SESSION = 14,
	CMD_REPLY_SET_SHMEM_RIGHTS = 15, CMD_REPLY_OPEN_SESSION = 16, CMD_REPLY_CLOSE_SESSION = 17,
	CMD_REPLY_INVOKE_COMMAND = 18,
};

#define TEEC_ORIGIN_COMMS       2
#define TEEC_ERROR_COMMUNICATION 0xFFFF000E
#define TEE_REPLY_MS            20000   /* a TA load and its start can take seconds */

static int
tee_connect_root(struct sock_desc **out)
{
	struct sock_desc *sd = tz_iwsock_socket();
	int ret = -ENOMEM;

	if (sd == NULL) {
		return ret;
	}
	for (int attempt = 0; attempt < 20; attempt++) {
		if ((ret = tz_iwsock_connect(sd, ROOT_TASK_SOCK)) == 0) {
			*out = sd;
			return 0;
		}
		tee_say("connect %s: %d, again\n", ROOT_TASK_SOCK, ret);
		if (ret != -ETIMEDOUT) {
			/* (a refused socket cannot be reused: a new one) */
			tz_iwsock_release(sd);
			if ((sd = tz_iwsock_socket()) == NULL) {
				return -ENOMEM;
			}
		}
		tz_msleep(100);
	}
	tz_iwsock_release(sd);
	return ret;
}

/* send `cmd`, read the reply into `ack`; check its type and serial; the TEE's result, or a communication error */
static uint32_t
tee_xmit(struct sock_desc *sd, void *cmd, size_t cmd_len, void *ack, size_t ack_len, uint32_t reply_cmd,
    uint32_t serial, uint32_t *origin)
{
	struct cmd_reply *r = ack;
	ssize_t n;

	*origin = TEEC_ORIGIN_COMMS;
	if ((n = tz_iwsock_write(sd, cmd, cmd_len, false)) != (ssize_t)cmd_len) {
		tee_say("xmit: write %zd\n", n);
		return TEEC_ERROR_COMMUNICATION;
	}
	if ((n = tz_iwsock_read(sd, ack, ack_len, sd->reply_timeout_ms)) != (ssize_t)ack_len) {
		tee_say("xmit: read %zd (wanted %zu)\n", n, ack_len);
		return TEEC_ERROR_COMMUNICATION;
	}
	if (r->base.cmd != reply_cmd || r->base.serial != serial) {
		tee_say("xmit: reply %u serial %u, wanted %u serial %u\n", r->base.cmd, r->base.serial, reply_cmd, serial);
		return TEEC_ERROR_COMMUNICATION;
	}
	*origin = r->origin;
	return r->result;
}

struct tee_context {
	struct sock_desc *sd;
	uint32_t serial, id;
};

struct tee_session {
	struct sock_desc *sd;
	uint32_t serial;
};

static uint32_t
tee_context_open(struct tee_context *ctx, uint32_t *origin)
{
	struct cmd_request cmd = { .cmd = CMD_INITIALIZE_CONTEXT, .serial = 0 };
	struct cmd_reply_initialize_context ack;
	uint32_t result;
	int ret;

	memset(ctx, 0, sizeof(*ctx));
	if ((ret = tee_connect_root(&ctx->sd)) != 0) {
		*origin = TEEC_ORIGIN_COMMS;
		return TEEC_ERROR_COMMUNICATION;
	}
	result = tee_xmit(ctx->sd, &cmd, sizeof(cmd), &ack, sizeof(ack), CMD_REPLY_INITIALIZE_CONTEXT, 0, origin);
	ctx->serial = 1;
	ctx->id = ack.ctx_id;
	if (result != 0) {
		tz_iwsock_release(ctx->sd);
		ctx->sd = NULL;
	}
	return result;
}

/* shared memory for the TEE: a wired copy; *id and the rights given on the context */
static uint32_t
tee_shm_register(struct tee_context *ctx, void *buf, size_t size, bool output, int *id, uint32_t *origin)
{
	struct cmd_set_shared_memory_rights cmd;
	struct cmd_reply ack;
	uint32_t result;

	*id = tzdev_mem_register(buf, size, output);
	if (*id < 0) {
		*origin = TEEC_ORIGIN_COMMS;
		return TEEC_ERROR_COMMUNICATION;
	}
	memset(&cmd, 0, sizeof(cmd));
	cmd.base.cmd = CMD_SET_SHMEM_RIGHTS;
	cmd.base.serial = ctx->serial;
	cmd.buf_desc.offset = 0;
	cmd.buf_desc.size = (uint32_t)size;
	cmd.buf_desc.id = (uint32_t)*id;
	result = tee_xmit(ctx->sd, &cmd, sizeof(cmd), &ack, sizeof(ack), CMD_REPLY_SET_SHMEM_RIGHTS, ctx->serial, origin);
	ctx->serial++;
	return result;
}

/* open a session with the TA whose image is `image` (wired, tz_alloc) and `uuid` */
static uint32_t
tee_session_open(struct tee_context *ctx, struct tee_session *ses, const struct tz_uuid *uuid, void *image,
    size_t image_size, uint32_t *origin)
{
	struct cmd_prepare_session prep;
	struct cmd_reply prep_ack;
	struct cmd_open_session open;
	struct cmd_reply_open_session open_ack;
	uint32_t result;
	int shm_id = 0;

	memset(ses, 0, sizeof(*ses));
	if (tee_connect_root(&ses->sd) != 0) {
		*origin = TEEC_ORIGIN_COMMS;
		return TEEC_ERROR_COMMUNICATION;
	}
	if (image != NULL && image_size) {
		result = tee_shm_register(ctx, image, image_size, false, &shm_id, origin);
		if (result != 0) {
			tee_say("TA image shared memory: %#x origin %u\n", result, *origin);
			goto fail;
		}
	}
	memset(&prep, 0, sizeof(prep));
	prep.base.cmd = CMD_PREPARE_SESSION;
	prep.base.serial = ses->serial;
	prep.ctx_id = ctx->id;
	if (shm_id > 0) {
		prep.buf_desc.offset = 0;
		prep.buf_desc.id = (uint32_t)shm_id;
		prep.buf_desc.size = (uint32_t)image_size;
	}
	prep.ta_uuid = *uuid;
	result = tee_xmit(ses->sd, &prep, sizeof(prep), &prep_ack, sizeof(prep_ack), CMD_REPLY_PREPARE_SESSION,
	    ses->serial, origin);
	ses->serial++;
	if (shm_id > 0) {
		tzdev_mem_release(shm_id);
	}
	if (result != 0) {
		tee_say("prepare session: %#x origin %u\n", result, *origin);
		goto fail;
	}
	memset(&open, 0, sizeof(open));
	open.base.cmd = CMD_OPEN_SESSION;
	open.base.serial = ses->serial;
	open.cancel_time = IWD_TIMEOUT_INFINITY;
	open.conn_meth = 0;                     /* TEEC_LOGIN_PUBLIC */
	result = tee_xmit(ses->sd, &open, sizeof(open), &open_ack, sizeof(open_ack), CMD_REPLY_OPEN_SESSION,
	    ses->serial, origin);
	ses->serial++;
	if (result != 0) {
		tee_say("open session: %#x origin %u\n", result, *origin);
		goto fail;
	}
	return 0;
fail:
	tz_iwsock_release(ses->sd);
	ses->sd = NULL;
	return result;
}

static void
tee_session_close(struct tee_session *ses)
{
	struct cmd_request cmd = { .cmd = CMD_CLOSE_SESSION, .serial = ses->serial };
	struct cmd_reply ack;
	uint32_t origin;

	if (ses->sd == NULL) {
		return;
	}
	(void)tee_xmit(ses->sd, &cmd, sizeof(cmd), &ack, sizeof(ack), CMD_REPLY_CLOSE_SESSION, ses->serial, &origin);
	tz_iwsock_release(ses->sd);
	ses->sd = NULL;
}

/*
 * Invoke `cmd_id`. params: up to four; each a value (in/out) or a buffer that this copies through a fresh
 * registered shared memory (TEEC_MEMREF_TEMP_*), sizes updated on return.
 */
static uint32_t
tee_invoke(struct tee_context *ctx, struct tee_session *ses, uint32_t cmd_id, struct redwood_tee_param p[4],
    uint32_t *origin)
{
	struct cmd_invoke_command cmd;
	struct cmd_reply_invoke_command ack;
	void *bufs[4] = { NULL };
	size_t sizes[4] = { 0 };
	uint32_t result;

	memset(&cmd, 0, sizeof(cmd));
	for (unsigned i = 0; i < 4; i++) {
		uint32_t type = p[i].type;

		cmd.op.tee_param_types |= (type & 0xf) << (i * 4);
		if (type == REDWOOD_TEE_VALUE_INPUT || type == REDWOOD_TEE_VALUE_INOUT) {
			cmd.op.tee_params[i].val.a = p[i].a;
			cmd.op.tee_params[i].val.b = p[i].b;
		} else if (type >= REDWOOD_TEE_MEMREF_INPUT && type <= REDWOOD_TEE_MEMREF_INOUT && p[i].size) {
			int id;

			sizes[i] = p[i].size;
			if ((bufs[i] = tz_alloc(sizes[i])) == NULL) {
				result = 0xFFFF000C;    /* TEEC_ERROR_OUT_OF_MEMORY */
				goto out;
			}
			if (type != REDWOOD_TEE_MEMREF_OUTPUT && p[i].buf != NULL) {
				memcpy(bufs[i], p[i].buf, sizes[i]);
			}
			result = tee_shm_register(ctx, bufs[i], sizes[i], type != REDWOOD_TEE_MEMREF_INPUT, &id, origin);
			if (result != 0) {
				goto out;
			}
			cmd.op.tee_params[i].shared_buffer_description.offset = 0;
			cmd.op.tee_params[i].shared_buffer_description.id = (uint32_t)id;
			cmd.op.tee_params[i].shared_buffer_description.size = (uint32_t)sizes[i];
			p[i].shm_id = id;
		}
	}
	cmd.base.cmd = CMD_INVOKE_COMMAND;
	cmd.base.serial = ses->serial;
	cmd.cancel_time = IWD_TIMEOUT_INFINITY;
	cmd.tee_cmd_id = cmd_id;
	result = tee_xmit(ses->sd, &cmd, sizeof(cmd), &ack, sizeof(ack), CMD_REPLY_INVOKE_COMMAND, ses->serial, origin);
	ses->serial++;
	for (unsigned i = 0; i < 4; i++) {
		uint32_t type = p[i].type;

		if (type == REDWOOD_TEE_VALUE_OUTPUT || type == REDWOOD_TEE_VALUE_INOUT) {
			p[i].a = ack.op.tee_params[i].val.a;
			p[i].b = ack.op.tee_params[i].val.b;
		} else if ((type == REDWOOD_TEE_MEMREF_OUTPUT || type == REDWOOD_TEE_MEMREF_INOUT) && bufs[i] != NULL) {
			size_t n = MIN(sizes[i], ack.op.tee_params[i].shared_buffer_description.size);

			if (p[i].buf != NULL) {
				memcpy(p[i].buf, bufs[i], n);
			}
			p[i].size = (uint32_t)n;
		}
	}
out:
	for (unsigned i = 0; i < 4; i++) {
		if (p[i].shm_id > 0) {
			tzdev_mem_release(p[i].shm_id);
			p[i].shm_id = 0;
		}
		/* (buffers stay allocated: the secure world releases the ids later; small, rare) */
	}
	return result;
}

static struct tee_context tee_ctx;
static bool tee_ctx_open;
LCK_MTX_DECLARE(tee_client_lock, &tee_lck_grp);

/* invoke `cmd_id` with param 0 = the registered shared memory `shm_id` (in/out, `size` bytes), nothing else */
static uint32_t
tee_invoke_shm(struct tee_session *ses, uint32_t cmd_id, int shm_id, uint32_t size, uint32_t *origin)
{
	struct cmd_invoke_command cmd;
	struct cmd_reply_invoke_command ack;
	uint32_t result;

	memset(&cmd, 0, sizeof(cmd));
	cmd.op.tee_param_types = 7;     /* TEEC_MEMREF_TEMP_INOUT */
	cmd.op.tee_params[0].shared_buffer_description.offset = 0;
	cmd.op.tee_params[0].shared_buffer_description.id = (uint32_t)shm_id;
	cmd.op.tee_params[0].shared_buffer_description.size = size;
	cmd.base.cmd = CMD_INVOKE_COMMAND;
	cmd.base.serial = ses->serial;
	cmd.cancel_time = IWD_TIMEOUT_INFINITY;
	cmd.tee_cmd_id = cmd_id;
	result = tee_xmit(ses->sd, &cmd, sizeof(cmd), &ack, sizeof(ack), CMD_REPLY_INVOKE_COMMAND, ses->serial, origin);
	ses->serial++;
	return result;
}

/*
 * The trusted storage agent (tzdaemon's tzdaemon_ts.c, reversed 2026-11-14): a session with TEEGRIS's built-in
 * storage service 00000005-0005-0005-0505-050505050505, a 0x41000-byte in/out buffer, then for ever:
 * invoke(0) = wait for a request (buffer: u32 group (5), u32 TS_CMD_* 1..12, u32 length, u32 status, payload at
 * +0x10), have it carried out, invoke(1) = the reply in the same buffer. Until it runs, the root task refuses to
 * load TAs ("Trusted storage is not ready"). The file work is done by a user-space daemon (teetsd) through
 * kern.redwood.tee_ts: read = the pending request (whole buffer, or nothing), write = the answered buffer.
 */
#define TS_BUF_SIZE             0x41000u
static void *ts_buf;
static int ts_pending, ts_done, ts_wq, ts_answer_wq;
static uint64_t ts_requests, ts_unanswered;
static uint32_t ts_last_cmd;

static void
tee_ts_thread(__unused void *arg, __unused wait_result_t wr)
{
	static const struct tz_uuid ts_uuid = { 5, 5, 5, { 5, 5, 5, 5, 5, 5, 5, 5 } };
	struct tee_session ses;
	uint32_t result, origin;
	int shm_id;

	lck_mtx_lock(&tee_client_lock);
	if (!tee_ctx_open) {
		if ((result = tee_context_open(&tee_ctx, &origin)) != 0) {
			lck_mtx_unlock(&tee_client_lock);
			tee_say("storage agent: context %#x origin %u\n", result, origin);
			return;
		}
		tee_ctx_open = true;
	}
	if ((ts_buf = tz_alloc(TS_BUF_SIZE)) == NULL) {
		lck_mtx_unlock(&tee_client_lock);
		return;
	}
	result = tee_shm_register(&tee_ctx, ts_buf, TS_BUF_SIZE, true, &shm_id, &origin);
	if (result != 0) {
		lck_mtx_unlock(&tee_client_lock);
		tee_say("storage agent: shared memory %#x origin %u\n", result, origin);
		return;
	}
	result = tee_session_open(&tee_ctx, &ses, &ts_uuid, NULL, 0, &origin);
	lck_mtx_unlock(&tee_client_lock);
	if (result != 0) {
		tee_say("storage agent: session %#x origin %u\n", result, origin);
		return;
	}
	tee_say("storage agent: session with the storage service open\n");
	ses.sd->reply_timeout_ms = 0;
	{
		/* tzts_daemon's ACSD driver (tsacsddrv.c): built in, kept open; root_task asks it about every TA it loads */
		static const struct tz_uuid acsd_uuid = { 7, 7, 7, { 7, 7, 7, 7, 7, 7, 7, 7 } };
		static struct tee_session acsd;

		lck_mtx_lock(&tee_client_lock);
		result = tee_session_open(&tee_ctx, &acsd, &acsd_uuid, NULL, 0, &origin);
		lck_mtx_unlock(&tee_client_lock);
		tee_say("ACSD driver session: %#x origin %u%s\n", result, origin, result == 0 ? " (open)" : "");
	}
	for (;;) {
		bzero(ts_buf, 64);
		result = tee_invoke_shm(&ses, 0, shm_id, TS_BUF_SIZE, &origin);
		if (result != 0) {
			tee_say("storage agent: wait for request %#x origin %u\n", result, origin);
			tz_msleep(1000);
			continue;
		}
		ts_requests++;
		ts_last_cmd = ((uint32_t *)ts_buf)[1];
		__atomic_store_n(&ts_done, 0, __ATOMIC_SEQ_CST);
		__atomic_store_n(&ts_pending, 1, __ATOMIC_SEQ_CST);
		tz_wake(&ts_wq);
		for (unsigned waited = 0; !__atomic_load_n(&ts_done, __ATOMIC_ACQUIRE); waited += 50) {
			if (waited >= 30000) {
				/* no daemon: "error" (8), so the secure world goes on */
				((uint32_t *)ts_buf)[3] = 8;
				ts_unanswered++;
				break;
			}
			assert_wait_timeout((event_t)&ts_answer_wq, THREAD_UNINT, 50, 1000 * NSEC_PER_USEC);
			thread_block(THREAD_CONTINUE_NULL);
		}
		__atomic_store_n(&ts_pending, 0, __ATOMIC_SEQ_CST);
		result = tee_invoke_shm(&ses, 1, shm_id, TS_BUF_SIZE, &origin);
		if (result != 0) {
			tee_say("storage agent: reply %#x origin %u\n", result, origin);
		}
	}
}

static int
tee_ts_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error = 0;

	if (req->newptr != USER_ADDR_NULL) {
		/* the answer: at most the whole buffer, into the pending request's */
		if (!__atomic_load_n(&ts_pending, __ATOMIC_ACQUIRE) || req->newlen > TS_BUF_SIZE || req->newlen < 16) {
			return EINVAL;
		}
		if ((error = SYSCTL_IN(req, ts_buf, req->newlen)) != 0) {
			return error;
		}
		__atomic_store_n(&ts_done, 1, __ATOMIC_SEQ_CST);
		tz_wake(&ts_answer_wq);
		return 0;
	}
	if (req->oldptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, NULL, TS_BUF_SIZE);
	}
	/* a request: wait up to 500 ms for one */
	for (unsigned waited = 0; !__atomic_load_n(&ts_pending, __ATOMIC_ACQUIRE) || __atomic_load_n(&ts_done, __ATOMIC_ACQUIRE);
	    waited += 50) {
		if (waited >= 500) {
			return SYSCTL_OUT(req, ts_buf, 0);
		}
		assert_wait_timeout((event_t)&ts_wq, THREAD_INTERRUPTIBLE, 50, 1000 * NSEC_PER_USEC);
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_INTERRUPTED) {
			return EINTR;
		}
	}
	return SYSCTL_OUT(req, ts_buf, TS_BUF_SIZE);
}

/* ---- the kernel API (sys/redwood_teegris.h) ------------------------------------------------------------ */
bool
redwood_tee_up(void)
{
	return tee_state == TEE_UP;
}

void
redwood_tee_set_client(const struct tz_cred *cred)
{
	if (cred != NULL) {
		tee_client_cred = *cred;
		tee_client_cred_set = true;
	} else {
		tee_client_cred_set = false;
	}
}


/* read a file into wired memory (a TA image): the buffer and its size, or NULL */
static void *
tee_read_file(const char *path, size_t *size_out)
{
	vfs_context_t vctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	struct vnode_attr va;
	void *buf = NULL;
	int resid = 0, err;

	if ((err = vnode_open(path, FREAD, 0, 0, &vp, vctx)) != 0) {
		tee_say("%s: open %d\n", path, err);
		return NULL;
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	if (vnode_getattr(vp, &va, vctx) != 0 || va.va_data_size == 0 || va.va_data_size > 16 * 1024 * 1024) {
		goto out;
	}
	if ((buf = tz_alloc((size_t)va.va_data_size)) == NULL) {
		goto out;
	}
	err = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)va.va_data_size, 0, UIO_SYSSPACE, IO_NODELOCKED,
	    vfs_context_ucred(vctx), &resid, vfs_context_proc(vctx));
	if (err != 0 || resid != 0) {
		tz_free(buf, (size_t)va.va_data_size);
		buf = NULL;
		goto out;
	}
	*size_out = (size_t)va.va_data_size;
out:
	vnode_close(vp, FREAD, vctx);
	return buf;
}

int
redwood_tee_open(const char *ta_path, const struct tz_uuid *uuid, struct redwood_tee_session **out, uint32_t *result,
    uint32_t *origin)
{
	struct tee_session *ses;
	size_t size = 0;
	void *image = NULL;

	*result = 0;
	*origin = 0;
	if (tee_state != TEE_UP) {
		return ENXIO;
	}
	if (ta_path != NULL && (image = tee_read_file(ta_path, &size)) == NULL) {
		return ENOENT;
	}
	lck_mtx_lock(&tee_client_lock);
	if (!tee_ctx_open) {
		*result = tee_context_open(&tee_ctx, origin);
		if (*result != 0) {
			lck_mtx_unlock(&tee_client_lock);
			tee_say("context: %#x origin %u\n", *result, *origin);
			return EIO;
		}
		tee_ctx_open = true;
	}
	ses = kalloc_type(struct tee_session, Z_WAITOK | Z_ZERO);
	*result = tee_session_open(&tee_ctx, ses, uuid, image, size, origin);
	lck_mtx_unlock(&tee_client_lock);
	/* (the image stays: its shared memory id may still be in use while the secure world copies it) */
	if (*result != 0) {
		kfree_type(struct tee_session, ses);
		return EIO;
	}
	*out = (struct redwood_tee_session *)ses;
	return 0;
}

int
redwood_tee_invoke(struct redwood_tee_session *session, uint32_t cmd_id, struct redwood_tee_param params[4],
    uint32_t *result, uint32_t *origin)
{
	if (tee_state != TEE_UP || session == NULL) {
		return ENXIO;
	}
	lck_mtx_lock(&tee_client_lock);
	*result = tee_invoke(&tee_ctx, (struct tee_session *)session, cmd_id, params, origin);
	lck_mtx_unlock(&tee_client_lock);
	return *result == 0 ? 0 : EIO;
}

void
redwood_tee_close(struct redwood_tee_session *session)
{
	if (session == NULL) {
		return;
	}
	struct tee_session *ses = (struct tee_session *)session;

	lck_mtx_lock(&tee_client_lock);
	tee_session_close(ses);
	lck_mtx_unlock(&tee_client_lock);
	kfree_type(struct tee_session, ses);
}

/* ---- sysctl kern.redwood.tee ---------------------------------------------------------------------------- */
/*
 * write "start"                         start TEEGRIS (once; asynchronous, read for the result)
 * write "open PATH"                     load the TA at PATH (its UUID from the file name, Samsung's
 *                                       00000000-0000-0000-0000-XXXXXXXXXXXX form), open a session, close it
 * read                                  the state, counters, and the last test's result
 */
static char tee_last[256] = "";

static bool
tee_parse_uuid(const char *path, struct tz_uuid *u)
{
	const char *name = strrchr(path, '/');
	unsigned v[16], n = 0;

	name = name ? name + 1 : path;
	for (const char *p = name; *p && n < 16; p++) {
		int hi, lo;

		if (*p == '-') {
			continue;
		}
		if (!p[1]) {
			return false;
		}
		hi = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 :
		    (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
		lo = (p[1] >= '0' && p[1] <= '9') ? p[1] - '0' : (p[1] >= 'a' && p[1] <= 'f') ? p[1] - 'a' + 10 :
		    (p[1] >= 'A' && p[1] <= 'F') ? p[1] - 'A' + 10 : -1;
		if (hi < 0 || lo < 0) {
			return false;
		}
		v[n++] = (unsigned)(hi << 4 | lo);
		p++;
	}
	if (n != 16) {
		return false;
	}
	u->time_low = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
	u->time_mid = (uint16_t)(v[4] << 8 | v[5]);
	u->time_hi_and_version = (uint16_t)(v[6] << 8 | v[7]);
	for (unsigned i = 0; i < 8; i++) {
		u->clock_seq_and_node[i] = (uint8_t)v[8 + i];
	}
	return true;
}

static char tee_open_path[256];
static bool tee_open_keep;

static void
tee_open_test_thread(__unused void *arg, __unused wait_result_t wr)
{
	struct redwood_tee_session *s = NULL;
	struct tz_uuid u;
	uint32_t result = 0, origin = 0;
	int err;

	if (!tee_parse_uuid(tee_open_path, &u)) {
		snprintf(tee_last, sizeof(tee_last), "open: no UUID in the file name");
		return;
	}
	snprintf(tee_last, sizeof(tee_last), "open %s: in progress", tee_open_path);
	err = redwood_tee_open(tee_open_path, &u, &s, &result, &origin);
	snprintf(tee_last, sizeof(tee_last), "open %s: err %d result %#x origin %u%s", tee_open_path, err, result, origin,
	    err == 0 ? (tee_open_keep ? " -- SESSION OPEN (kept)" : " -- SESSION OPEN (closed again)") : "");
	tee_say("%s\n", tee_last);
	if (err == 0 && !tee_open_keep) {
		redwood_tee_close(s);
	}
}

/*
 * The fingerprint TA (FINGE, Samsung libbauthtzcommon's protocol, reversed 2026-11-14): one session, one 0x2ee00c-byte
 * shared buffer, every command is TEEC command N with param 0 = the whole buffer (partial in/out):
 *   request  +0x0 u32 1, +0x4 u32 N, +0xc u32 handle (the last response's), +0x10 data (<= 0xa000), +0xa010 u32 length
 *   response +0x177004 u32 status, +0x177008 u32 handle, +0x177010 u32 ?, +0x177014 data, +0x181014 u32 length,
 *            +0x2ee008 u32 error
 * N: 1 PREPARE 2 ENROLL_INIT 3 ENROLL_DO 4 ENROLL_FINAL 5 IDENTIFY_INIT 6 IDENTIFY_DO 7 IDENTIFY_FINAL 8 SET_PASSWORD
 *    9 VERIFY_PASSWORD 10 CANCEL 12 CONTROL_OP 13 HAT_OP 14 TEMPLATE_MIGRATION 16 TYPE_CHECK
 */
#define FP_SHM_SIZE             0x2ee00cu
#define FP_STORE_PATH           "fingerprint" /* Legacy enrollment binding; preserve for existing templates. */
#define FP_TA_PATH              "/var/root/tee/00000000-0000-0000-0000-46494e474502"
static struct tee_session fp_ses;
static bool fp_open;
static uint8_t *fp_buf;
static int fp_shm_id;
static uint32_t fp_handle;
static char fp_cmd_text[300];
LCK_MTX_DECLARE(fp_command_lock, &tee_lck_grp);
static bool fp_command_active; /* protected by fp_command_lock */
static bool fp_run_stopping; /* accessed with compiler atomics */
static bool fp_stopping(void) { return __atomic_load_n(&fp_run_stopping, __ATOMIC_ACQUIRE); }

static int
fp_ensure(void)
{
	struct redwood_tee_session *s = NULL;
	struct tz_uuid u = { 0, 0, 0, { 0, 0, 0x46, 0x49, 0x4e, 0x47, 0x45, 0x02 } };
	uint32_t result = 0, origin = 0;
	int err;

	if (fp_open) {
		return 0;
	}
	if ((err = redwood_tee_open(FP_TA_PATH, &u, &s, &result, &origin)) != 0) {
		snprintf(tee_last, sizeof(tee_last), "fp: open err %d result %#x origin %u", err, result, origin);
		return err;
	}
	fp_ses = *(struct tee_session *)s;
	if (fp_buf == NULL && (fp_buf = tz_alloc(FP_SHM_SIZE)) == NULL) {
		return ENOMEM;
	}
	lck_mtx_lock(&tee_client_lock);
	result = tee_shm_register(&tee_ctx, fp_buf, FP_SHM_SIZE, true, &fp_shm_id, &origin);
	lck_mtx_unlock(&tee_client_lock);
	if (result != 0) {
		snprintf(tee_last, sizeof(tee_last), "fp: shared memory %#x origin %u", result, origin);
		return EIO;
	}
	fp_open = true;
	return 0;
}

static int
fp_hex(char c)
{
	return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

/*
 * The QBT2000 sensor's normal-world side, as Samsung's qbt2000 driver does it (drivers/fingerprint/qbt2000_common.c
 * power_control + qbt2000_platform.c sec_spi_prepare; x1s dts "qbt2000"): VDD_BTP_1P8 = S2MPS19 LDO19M on; the pins to
 * "pins_poweron" = spi4-bus (gpp2-0/1/2 function 2, no pull) + spi4-fp-cs-func-high (gpp2-3 function 2, data 1); the SPI
 * clocks (fp-spi-pclk = Q-channel USI04_USI, fp-spi-sclk = DIV_CLK_PERIC0_USI04_USI) on at 4 x 25 MHz. The SPI itself
 * (USI04, 0x105D0000) is the secure world's. Pins: PERIC0 pinctrl 0x10430000, bank gpp2 at +0x40.
 */
extern int redwood_pmic_ldo_on(unsigned ldo, char *report, size_t report_size);
#include <sys/redwood_bbi2c.h>
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
#define FP_CMU_PERIC0           0x10400000UL
#define  FP_USI04_USER_MUX      0x0660
#define  FP_USI04_DIV           0x1814
#define  FP_USI04_RSTNSYNC      0x20cc
#define  FP_USI04_QCH           0x303c

static int
fp_power(void)
{
	static vm_offset_t cmu;
	volatile uint8_t *gpp2;
	char line[160];
	int r;

	r = redwood_pmic_ldo_on(19, line, sizeof(line));
	tee_say("fp power: %s (%d)\n", line, r);
	if (r != 0) { return r; }
	delay(10000);
	if (cmu == 0) {
		cmu = ml_io_map(FP_CMU_PERIC0, 0x4000);
	}
#define CMU(o)  (*(volatile uint32_t *)(cmu + (o)))
	tee_say("fp power: USI04 before: user mux %08x div %08x rstnsync %08x qch %08x\n", CMU(FP_USI04_USER_MUX),
	    CMU(FP_USI04_DIV), CMU(FP_USI04_RSTNSYNC), CMU(FP_USI04_QCH));
	CMU(FP_USI04_RSTNSYNC) |= (1u << 20) | (1u << 21);
	CMU(FP_USI04_QCH) |= 3;
	if ((CMU(FP_USI04_USER_MUX) >> 4) & 1) {
		CMU(FP_USI04_DIV) = (CMU(FP_USI04_DIV) & ~0xfu) | 3;    /* 400 MHz / 4 = 100 MHz: SCLK 25 MHz */
	}
	delay(100);
	tee_say("fp power: USI04 after: user mux %08x div %08x rstnsync %08x qch %08x\n", CMU(FP_USI04_USER_MUX),
	    CMU(FP_USI04_DIV), CMU(FP_USI04_RSTNSYNC), CMU(FP_USI04_QCH));
	for (uint32_t o = 0x2000; o < 0x2080; o += 4) {
		if (CMU(o) != 0) {
			tee_say("fp power: PERIC0 gate +%#x = %08x\n", o, CMU(o));
		}
	}
#undef CMU
	{
		/* the sensor's IPC line, GPA1-3: Samsung's btp-ipc = EINT, pull-up, IRQ on the falling edge (active low) */
		volatile uint8_t *gpa1 = redwood_gpio_bank(0x15850000, 0x20);

		redwood_gpio_pull(gpa1, 3, 3);
		redwood_gpio_func(gpa1, 3, 0);
		delay(1000);
		tee_say("fp power: IPC line (gpa1-3) pulled up: con %08x dat %02x pud %08x\n", *(volatile uint32_t *)gpa1,
		    *(volatile uint32_t *)(gpa1 + 4) & 0xff, *(volatile uint32_t *)(gpa1 + 8));
	}
	gpp2 = redwood_gpio_bank(0x10430000, 0x40);
	redwood_gpio_set(gpp2, 3, 1);           /* data 1 first (this makes it an output), then the SPI function */
	for (uint32_t pin = 0; pin < 4; pin++) {
		redwood_gpio_pull(gpp2, pin, 0);
		redwood_gpio_func(gpp2, pin, 2);
	}
	tee_say("fp power: gpp2 con %08x dat %08x pud %08x\n", *(volatile uint32_t *)gpp2, *(volatile uint32_t *)(gpp2 + 4),
	    *(volatile uint32_t *)(gpp2 + 8));
	return 0;
}

/*
 * "run N OP SECONDS": Samsung's loop (libbauthserver check_opcode): command N again and again with the op code the TA
 * answered, until it answers 0, fails, or SECONDS pass. Op 4 waits for steady contact on the sensor's screen area.
 * Op 7 sleeps the requested timeout. Each step's response head is logged.
 */
/* ENROLL_INIT: user id "0", finger index IDX (1-4), type 7, stable logical storage path. */
static uint32_t
fp_einit(uint32_t idx)
{
	uint32_t e;
	if (fp_stopping()) { return ECANCELED; }

	bzero(fp_buf, 0x200);
	bzero(fp_buf + 0x177000, 0x40);
	*(uint32_t *)(fp_buf + 0x0) = 2;            /* libbauthtzcommon: the command at +0 and +4 */
	*(uint32_t *)(fp_buf + 0x4) = 2;
	*(uint32_t *)(fp_buf + 0xc) = 1;
	fp_buf[0x10] = '0';
	*(uint32_t *)(fp_buf + 0x110) = idx;
	*(uint32_t *)(fp_buf + 0x114) = 7;
	memcpy(fp_buf + 0x118, FP_STORE_PATH, sizeof(FP_STORE_PATH));
	*(uint32_t *)(fp_buf + FP_WIRE_ERROR) = 0;
	{
		uint32_t origin = 0;

		lck_mtx_lock(&tee_client_lock);
		e = tee_invoke_shm(&fp_ses, 2, fp_shm_id, FP_SHM_SIZE, &origin);
		lck_mtx_unlock(&tee_client_lock);
	}
	if (e == 0 && *(uint32_t *)(fp_buf + 0x177004) != 2) {
		return 0x14; /* Samsung client: missing/wrong response command */
	}
	return e != 0 ? e : *(uint32_t *)(fp_buf + FP_WIRE_ERROR);
}

/* one bare command (no data) with op code OP; the TA's error, or the invoke's result when that failed */
static uint32_t
fp_call(uint32_t n, uint32_t op)
{
	uint32_t result, origin = 0;
	if (fp_stopping()) { return ECANCELED; }

	bzero(fp_buf, 0x10);
	bzero(fp_buf + 0x177000, 0x40);
	*(uint32_t *)(fp_buf + 0x0) = n;
	*(uint32_t *)(fp_buf + 0x4) = n;
	*(uint32_t *)(fp_buf + 0xc) = op;
	*(uint32_t *)(fp_buf + 0x181014) = 0;
	*(uint32_t *)(fp_buf + 0x2ee008) = 0;
	lck_mtx_lock(&tee_client_lock);
	result = tee_invoke_shm(&fp_ses, n, fp_shm_id, FP_SHM_SIZE, &origin);
	lck_mtx_unlock(&tee_client_lock);
	return result != 0 ? result : *(uint32_t *)(fp_buf + 0x2ee008);
}

extern volatile uint32_t redwood_touch_mask, redwood_touch_x, redwood_touch_y;
/*
 * The sensor's spot in touch units (0-65535 over 1080x2400): Samsung's qcom,position puts the centre 32.34 mm above the
 * screen's bottom edge, x centred (panel 540,1860 = 32768,50790); +-8 mm for the finger. A capture waits for 400 ms of
 * steady contact: at 150 ms the finger is not pressed flat yet (most samples came back LIGHT_TOUCH / PARTIAL).
 */
#define FP_X_MIN        27500
#define FP_X_MAX        38000
#define FP_Y_MIN        46500
#define FP_Y_MAX        55500
#define FP_HOLD_MS      700     /* 400 ms left too many TOO_FAST / LIGHT_TOUCH / PARTIAL rejections (Touch ID enrolment, 2026-11-19) */

static uint32_t fp_cbge_load(void);
static void fp_cbge(void);

/*
 * The enrolled template. The FINGE TA keeps none: ENROLL_DO hands it out at progress 100 (+0x177024, length at
 * +0x1db024, libbauthtzcommon BAuth_Enroll_Do) and IDENTIFY_INIT takes it back (+0x110, length +0x64110, chunk
 * number at +0x8, libbauthserver FPBAuthService::identify). Kept in /var/root/fp-template.bin (root only).
 */
#define FP_TMPL_PATH    "/var/root/fp-template.bin"
#define FP_TMPL_MAX     0x64000u
static uint8_t *fp_tmpl;
static uint32_t fp_tmpl_len;

static int
fp_tmpl_save(void)
{
	vfs_context_t vctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int resid = 0, err;

	if ((err = vnode_open(FP_TMPL_PATH, FWRITE | O_CREAT | O_TRUNC, 0600, 0, &vp, vctx)) != 0) {
		tee_say("fp template: open for writing %d\n", err);
		return err;
	}
	err = vn_rdwr(UIO_WRITE, vp, (caddr_t)fp_tmpl, (int)fp_tmpl_len, 0, UIO_SYSSPACE, IO_NODELOCKED | IO_SYNC,
	    vfs_context_ucred(vctx), &resid, vfs_context_proc(vctx));
	vnode_close(vp, FWRITE, vctx);
	if (err == 0 && resid != 0) {
		err = EIO;
	}
	tee_say("fp template: saved %u bytes to %s (error %d)\n", fp_tmpl_len, FP_TMPL_PATH, err);
	return err;
}

static int
fp_tmpl_load(void)
{
	size_t size = 0;
	void *buf;

	if (fp_tmpl != NULL && fp_tmpl_len != 0) {
		return 0;
	}
	buf = tee_read_file(FP_TMPL_PATH, &size);
	if (buf == NULL || size == 0 || size > FP_TMPL_MAX) {
		if (buf != NULL) {
			tz_free(buf, size);
		}
		tee_say("fp template: none saved (enroll first)\n");
		return ENOENT;
	}
	if (fp_tmpl == NULL) {
		fp_tmpl = kalloc_data(FP_TMPL_MAX, Z_WAITOK | Z_ZERO);
	}
	if (fp_tmpl != NULL) {
		memcpy(fp_tmpl, buf, size);
		fp_tmpl_len = (uint32_t)size;
	}
	tz_free(buf, size);
	tee_say("fp template: loaded %u bytes\n", fp_tmpl_len);
	return fp_tmpl ? 0 : ENOMEM;
}

/* keep the template ENROLL_DO just handed out (progress 100) */
static void
fp_tmpl_take(void)
{
	if (fp_stopping()) { return; }
	uint32_t len = *(uint32_t *)(fp_buf + 0x1db024);

	tee_say("fp template: enroll handed out %u bytes\n", len);
	if (len == 0 || len > FP_TMPL_MAX) {
		return;
	}
	if (fp_tmpl == NULL) {
		fp_tmpl = kalloc_data(FP_TMPL_MAX, Z_WAITOK | Z_ZERO);
	}
	if (fp_tmpl == NULL) {
		return;
	}
	memcpy(fp_tmpl, fp_buf + 0x177024, len);
	fp_tmpl_len = len;
	fp_tmpl_save();
}

/*
 * The matcher's sealed metadata (2026-11-18). Samsung's common_prepare (libbauthserver 0x14abbc) reads its saved
 * metadata (type 0) and calls PREPARE with op 1 and that record (op 2 only for the very first initialization), then
 * saves what PREPARE returns; BAuth_Identify_Init passes the same record at +0x64114 (length +0x64514). Before this,
 * every session sent PREPARE op 2 (a fresh matcher) and identify got no metadata: a match then held only for the
 * exact enrolment placement. BAuth_Prepare: input +0x10 / length +0xa010, output +0x177014 / length +0x181014.
 */
#define FP_META_PATH    "/var/root/fp-meta.bin"
#define FP_META_MAX     0xa000u
static uint8_t *fp_meta;
static uint32_t fp_meta_len;

static int
fp_meta_save(const uint8_t *data, uint32_t len)
{
	vfs_context_t vctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int resid = 0, err;

	if (len == 0 || len > FP_META_MAX) {
		return EINVAL;
	}
	if (fp_meta == NULL) {
		fp_meta = kalloc_data(FP_META_MAX, Z_WAITOK | Z_ZERO);
		if (fp_meta == NULL) {
			return ENOMEM;
		}
	}
	memcpy(fp_meta, data, len);
	fp_meta_len = len;
	if ((err = vnode_open(FP_META_PATH, FWRITE | O_CREAT | O_TRUNC, 0600, 0, &vp, vctx)) != 0) {
		tee_say("fp metadata: open for writing %d\n", err);
		return err;
	}
	err = vn_rdwr(UIO_WRITE, vp, (caddr_t)fp_meta, (int)fp_meta_len, 0, UIO_SYSSPACE, IO_NODELOCKED | IO_SYNC,
	    vfs_context_ucred(vctx), &resid, vfs_context_proc(vctx));
	vnode_close(vp, FWRITE, vctx);
	if (err == 0 && resid != 0) {
		err = EIO;
	}
	tee_say("fp metadata: saved %u bytes to %s (error %d)\n", fp_meta_len, FP_META_PATH, err);
	return err;
}

static void
fp_meta_load(void)
{
	size_t size = 0;
	void *buf;

	if (fp_meta != NULL && fp_meta_len != 0) {
		return;
	}
	buf = tee_read_file(FP_META_PATH, &size);
	if (buf == NULL || size == 0 || size > FP_META_MAX) {
		if (buf != NULL) {
			tz_free(buf, size);
		}
		return;
	}
	if (fp_meta == NULL) {
		fp_meta = kalloc_data(FP_META_MAX, Z_WAITOK | Z_ZERO);
	}
	if (fp_meta != NULL) {
		memcpy(fp_meta, buf, size);
		fp_meta_len = (uint32_t)size;
	}
	tz_free(buf, size);
}

/* PREPARE as Samsung does it: op 1 with the saved metadata, op 2 only without any; the returned metadata is saved. */
static uint32_t
fp_prepare(uint32_t *op_used, uint32_t *out_len)
{
	uint32_t result, origin = 0, op, len;
	if (fp_stopping()) { return ECANCELED; }

	fp_meta_load();
	op = fp_meta_len ? 1 : 2;
	bzero(fp_buf, 0x10);
	bzero(fp_buf + 0x177000, 0x40);
	*(uint32_t *)(fp_buf + 0x0) = 1;
	*(uint32_t *)(fp_buf + 0x4) = 1;
	*(uint32_t *)(fp_buf + 0xc) = op;
	if (fp_meta_len) {
		memcpy(fp_buf + 0x10, fp_meta, fp_meta_len);
	}
	*(uint32_t *)(fp_buf + 0xa010) = fp_meta_len;
	*(uint32_t *)(fp_buf + 0x181014) = 0;
	*(uint32_t *)(fp_buf + 0x2ee008) = 0;
	lck_mtx_lock(&tee_client_lock);
	result = tee_invoke_shm(&fp_ses, 1, fp_shm_id, FP_SHM_SIZE, &origin);
	lck_mtx_unlock(&tee_client_lock);
	*op_used = op;
	len = *(uint32_t *)(fp_buf + 0x181014);
	*out_len = len;
	if (result != 0) {
		return result;
	}
	if (*(uint32_t *)(fp_buf + 0x2ee008) != 0) {
		return *(uint32_t *)(fp_buf + 0x2ee008);
	}
	if (len > 0 && len <= FP_META_MAX) {
		fp_meta_save(fp_buf + 0x177014, len);
	}
	return 0;
}

/*
 * IDENTIFY_INIT (command 5, op 1): CHUNK = 0 with the template (first attempt), 0xffffffff without data (re-arm
 * for the next touch, as FPBAuthService::identify does). The TA's error, or the invoke's result.
 */
static uint32_t
fp_idinit(uint32_t chunk)
{
	uint32_t result, origin = 0;
	if (fp_stopping()) { return ECANCELED; }

	if (chunk == 0 && fp_tmpl_load() != 0) {
		return 0x3a;
	}
	/* Same complete request clearing as BAuth_Identify_Init. */
	bzero(fp_buf, 0x645c1);
	bzero(fp_buf + 0x177000, 0x1b0);
	*(uint32_t *)(fp_buf + 0x0) = 5;
	*(uint32_t *)(fp_buf + 0x4) = 5;
	*(uint32_t *)(fp_buf + 0x8) = chunk;
	*(uint32_t *)(fp_buf + 0xc) = 1;
	fp_buf[0x10] = '0';
	/* +0x118 during enrollment was a storage path, not a display name.
	 * Restore the identical authenticated 100-byte field on every Init. */
	memcpy(fp_buf + 0x6455d, FP_STORE_PATH, sizeof(FP_STORE_PATH));
	if (chunk == 0) {
		memcpy(fp_buf + 0x110, fp_tmpl, fp_tmpl_len);
		*(uint32_t *)(fp_buf + 0x64110) = fp_tmpl_len;
		/* and the matcher's metadata, as BAuth_Identify_Init (see fp_prepare) */
		fp_meta_load();
		if (fp_meta_len != 0 && fp_meta_len <= 0x400) {
			memcpy(fp_buf + 0x64114, fp_meta, fp_meta_len);
			*(uint32_t *)(fp_buf + 0x64514) = fp_meta_len;
		}
		tee_say("fp idinit: template %u bytes, metadata %u bytes\n", fp_tmpl_len, fp_meta_len);
	}
	*(uint32_t *)(fp_buf + 0x2ee008) = 0;
	lck_mtx_lock(&tee_client_lock);
	result = tee_invoke_shm(&fp_ses, 5, fp_shm_id, FP_SHM_SIZE, &origin);
	lck_mtx_unlock(&tee_client_lock);
	if (result == 0 && *(uint32_t *)(fp_buf + 0x177004) != 5) {
		return 0x14;
	}
	if (result == 0 && *(uint32_t *)(fp_buf + 0x17700c) !=
	    *(uint32_t *)(fp_buf + FP_WIRE_ERROR)) {
		return 0x14;
	}
	if (result == 0 && *(uint32_t *)(fp_buf + FP_WIRE_ERROR) == 0) {
		uint32_t length = *(uint32_t *)(fp_buf + 0x1771a4);
		uint32_t ids = fp_wire_id_mask(fp_buf + 0x177014, length);

		tee_say("fp idinit: template list mask %#x (expected finger 1), length %u\n", ids, length);
		if (ids != 1) {
			return 0x21; /* This prototype loads exactly one enrolled finger. */
		}
	}
	return result != 0 ? result : *(uint32_t *)(fp_buf + FP_WIRE_ERROR);
}

/* wait until no finger is on the sensor's spot for 100 ms (or the deadline) */
static void
fp_wait_lift(uint32_t n, uint64_t deadline)
{
	uint32_t off = 0;

	while (!fp_stopping() && mach_absolute_time() < deadline && off < 100) {
		uint32_t x = redwood_touch_x, y = redwood_touch_y;
		bool on = redwood_touch_mask != 0 && x > FP_X_MIN && x < FP_X_MAX && y > FP_Y_MIN && y < FP_Y_MAX;

		off = on ? 0 : off + 5;
		delay(5000);
	}
	tee_say("fp run %u: finger lifted\n", n);
}

/* close one touch as Samsung does: ENROLL_FINAL op loop (op 4 there = wait for the finger to leave), then ENROLL_INIT
 * again (after a 67: cancel first). The TA's error, or 0. */
static uint32_t
fp_close_touch(uint32_t n, uint64_t deadline, bool cancel, uint32_t mode)
{
	uint32_t *r = (uint32_t *)(fp_buf + 0x177008);
	uint32_t fop = 1, e = 0, k;

	if (mode == 4) {
		/*
		 * The TA (FINGE, decompiled dispatcher case 2/3): ENROLL_INIT continues an active enrollment (QFP_EnrollBegin
		 * only when none is active) and re-arms ENROLL_DO's per-touch state machine (finger detect -> op 4, capture
		 * -> op 5, image -> op 6, process -> progress or 39 + quality flags). ENROLL_FINAL, PREPARE and CANCEL all end
		 * or reset the enrollment, so between touches: only ENROLL_INIT.
		 */
		e = fp_einit(1);
		tee_say("fp run 3: next touch: enroll-init -> error %#x op %u\n", e, r[0]);
		return e;
	}
	if (!cancel && mode == 2) {
		/* BAuth_Control_OP 84 (libbauthserver check_opcode, around WAIT_INTERRUPT / NOTIFY_REMOVE_FINGER): command 12 */
		fp_wait_lift(n, deadline);
		*(uint32_t *)(fp_buf + 0xfa110) = 0;
		*(uint32_t *)(fp_buf + 0x271014) = 0;
		e = fp_call(12, 84);
		tee_say("fp run 3: control 84 -> error %#x op %u | %08x %08x %08x\n", e, r[0], r[1], r[2], r[4]);
		return 0;
	}
	if (cancel) {
		e = fp_call(10, 1);
		tee_say("fp run 3: cancel -> error %#x\n", e);
	} else {
		for (k = 0; k < 40; k++) {
			e = fp_call(4, fop);
			tee_say("fp run 3: enroll-final op %u -> error %#x op %u | %08x %08x %08x\n", fop, e, r[0], r[1], r[2], r[4]);
			if (e != 0 || r[0] == 0) {
				break;
			}
			if (r[0] == 7 && r[2] != 0 && r[2] < 5000000) {
				delay(r[2]);
			}
			if (r[0] == 4) {
				fp_wait_lift(n, deadline);
			}
			fop = r[0];
		}
		if (mode == 1) {
			return 0;
		}
	}
	e = fp_einit(1);
	tee_say("fp run 3: enroll-init again -> error %#x op %u\n", e, r[0]);
	for (k = 0; k < 40 && e == 0 && r[0] != 0; k++) {
		uint32_t iop = r[0];

		if (iop == 7 && r[2] != 0 && r[2] < 5000000) {
			delay(r[2]);
		}
		e = fp_call(2, iop);
		tee_say("fp run 3: enroll-init op %u -> error %#x op %u\n", iop, e, r[0]);
	}
	return e;
}

static void
fp_run(uint32_t n, uint32_t op, uint32_t seconds, uint32_t recover)
{
	uint64_t deadline;
	uint32_t steps = 0, last = ~0u, same = 0, bad_quality = 0, good = 0, refused = 0, after_good = 0;

	volatile uint8_t *gpa1 = redwood_gpio_bank(0x15850000, 0x20);

	tee_say("fp run %u: gpa1 con %08x dat %02x pud %08x\n", n, *(volatile uint32_t *)gpa1,
	    *(volatile uint32_t *)(gpa1 + 4) & 0xff, *(volatile uint32_t *)(gpa1 + 8));
	if (n == 6 && !(*(volatile uint32_t *)(gpa1 + 4) & 0x8)) {
		tee_say("fp run 6: IPC line low at the start: background calibration\n");
		fp_cbge();
	}
	clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
	while (!fp_stopping() && mach_absolute_time() < deadline) {
		uint32_t result, origin = 0, status, error, timeout;
		uint32_t *r = (uint32_t *)(fp_buf + 0x177008);

		bzero(fp_buf, 0x10);
		bzero(fp_buf + 0x177000, 0x40);
		*(uint32_t *)(fp_buf + 0x0) = n;            /* libbauthtzcommon: the command at +0 and +4 */
		*(uint32_t *)(fp_buf + 0x4) = n;
		*(uint32_t *)(fp_buf + 0xc) = op;
		*(uint32_t *)(fp_buf + 0x181014) = 0;
		*(uint32_t *)(fp_buf + 0x2ee008) = 0;
		lck_mtx_lock(&tee_client_lock);
		result = tee_invoke_shm(&fp_ses, n, fp_shm_id, FP_SHM_SIZE, &origin);
		lck_mtx_unlock(&tee_client_lock);
		if (fp_stopping()) { break; }
		status = *(uint32_t *)(fp_buf + 0x177004);
		error = *(uint32_t *)(fp_buf + 0x2ee008);
		timeout = r[2];
		steps++;
		if (r[0] != last || same++ % 100 == 0) {
			tee_say("fp run %u step %u: op %u -> result %#x status %#x op %u error %#x timeout %d bad %u gpa1 %02x | %08x %08x %08x %08x\n",
			    n, steps, op, result, status, r[0], error, (int)timeout, bad_quality, *(volatile uint32_t *)(gpa1 + 4) & 0xff,
			    r[1], r[3], r[4], r[5]);
			if (r[0] != last) {
				same = 1;
			}
		}
		last = r[0];
		/*
		 * Samsung's enroll() (libbauthserver 0x4e8dc..0x4feac) per touch: ENROLL_INIT (2) -> ENROLL_DO (3) op loop ->
		 * ENROLL_FINAL (4) op loop -> ENROLL_INIT again, until progress 100. A bad image (0x27/0x29) takes the same
		 * ENROLL_FINAL path (0x4eb44 -> 0x4ef4c), not a cancel. DO again without that is refused (39); 67 = bad state.
		 */
		if (n == 6) {
			enum fp_wire_result reply = fp_wire_identify(fp_buf, FP_SHM_SIZE, result, 1);
			uint32_t e;

			if (reply == FP_WIRE_PROTOCOL_ERROR) {
				tee_say("fp run 6: invalid response: transport %#x status %#x error %#x inner %#x op %u: stop\n",
				    result, status, error, r[1], r[0]);
				break;
			}
			if (reply != FP_WIRE_PENDING) {
				/* Raw wrapper 0x11993c returns score at r[4], bio ID at r[5].
				 * r[6] is not populated. A token-present flag is not HMAC proof. */
				if (reply == FP_WIRE_MATCH_CANDIDATE) {
					good++;
					tee_say("fp run 6: SENSOR MATCH candidate (bio %u score %u; token not verified)\n", r[5], r[4]);
				} else if (reply == FP_WIRE_NO_MATCH) {
					tee_say("fp run 6: NO MATCH\n");
				} else {
					bad_quality++;
					tee_say("fp run 6: capture rejected (%#x): lift and press again, quality %08x\n", error, r[3]);
				}
				fp_wait_lift(n, deadline);
				/* as Samsung's new_cbge_mode (libbauthserver 0x15abc0): the sensor raises its IPC line when its
				 * background needs a new estimate; calibrate now, finger off, before the next capture. Without it
				 * identify compared against a stale background: NO MATCH / contamination 0x1d (2026-11-19) */
				if (!(*(volatile uint32_t *)(gpa1 + 4) & 0x8)) {
					tee_say("fp run 6: IPC line low after the lift: background calibration\n");
					fp_cbge();
				}
				e = fp_idinit(0xffffffffu);
				if (e != 0) {
					tee_say("fp run 6: re-arm failed (%#x): stop\n", e);
					break;
				}
				op = 1;
				continue;
			}
		}
		if (n == 3 && result == 0 && error == 0x44) {
			/* This is not proof that CBGE is required. The same result was
			 * observed for capture failures; automatic calibration here
			 * previously replaced the working factory background model. */
			tee_say("fp run 3: capture error 0x44: stop; factory calibration preserved\n");
			break;
		}
		if (n == 3 && result == 0 && (error == 0x27 || error == 0x29 || error == 0x43 || (error == 0 && r[0] == 0))) {
			uint32_t progress = r[4], e;

			if (error == 0) {
				good++;
				tee_say("fp run 3: SAMPLE %u ACCEPTED: progress %u | %08x %08x %08x %08x\n", good, progress, r[1], r[3], r[5], r[6]);
				if (progress >= 100) {
					fp_tmpl_take();
					break;
				}
			} else {
				bad_quality++;
				tee_say("fp run 3: %s (%#x): lift and press again, quality %08x\n", error == 0x43 ? "capture rejected" : "bad image",
				    error, r[3]);
			}
			if (error == 0x43 && ++refused > 3) {
				tee_say("fp run 3: 67 keeps coming: stop\n");
				break;
			}
			if (error != 0x43) {
				refused = 0;
			}
			e = fp_close_touch(n, deadline, error == 0x43, recover);
			if (e != 0) {
				tee_say("fp run 3: closing the touch failed (%#x): stop\n", e);
				break;
			}
			fp_wait_lift(n, deadline);
			op = 1;
			continue;
		}
		if (result != 0 || error != 0 || r[0] == 0) {
			break;
		}
		op = r[0];
		if (op == 4 || op == 5) {
			if (after_good > 1) {
				tee_say("fp run %u: next sample ready after %u retries\n", n, after_good - 1);
			}
			after_good = 0;
		}
		if (op == 5) {
			refused = 0;                    /* a capture is under way */
		}
		if (op == 4) {
			/*
			 * The S20 starts a capture from the touch panel (finger on display), not from the sensor's lines: wait
			 * for a finger held FP_HOLD_MS on the sensor's spot (x centred, 32 mm up: 0-65535 units).
			 */
			uint32_t held = 0, logged_x = 0, logged_y = 0;

			while (!fp_stopping() && mach_absolute_time() < deadline && held < FP_HOLD_MS) {
				uint32_t x = redwood_touch_x, y = redwood_touch_y;


				bool down = redwood_touch_mask != 0;
				bool on = down && x > FP_X_MIN && x < FP_X_MAX && y > FP_Y_MIN && y < FP_Y_MAX;

				if (down && (x / 2000 != logged_x || y / 2000 != logged_y)) {
					tee_say("fp run %u: touch at %u,%u: %s\n", n, x, y, on ? "ON THE SENSOR" : "off the sensor");
					logged_x = x / 2000;
					logged_y = y / 2000;
				}
				held = on ? held + 5 : 0;
				delay(5000);
			}
			if (fp_stopping() || mach_absolute_time() >= deadline) {
				break;
			}
			tee_say("fp run %u: finger held on the sensor: capture (IPC line %s)\n", n,
			    (*(volatile uint32_t *)(gpa1 + 4) & 0x8) ? "high" : "low: the sensor wants calibration");
		} else if (op == 7 && timeout != 0 && timeout < 5000000) {
			delay(timeout);
		}
	}
	if (fp_stopping()) { tee_say("fp run %u: cancelled\n", n); }
	fp_handle = last;
	snprintf(tee_last, sizeof(tee_last), "fp run %u: %u steps, last op %u, %u bad images, %u accepted", n, steps, last, bad_quality, good);
	tee_say("%s\n", tee_last);
}

/*
 * "cbge": the QBT2000 background calibration as libbauthserver runs it (new_cbge_mode 0x5abc0, check_opcode table,
 * update_cbge 0x4c794): BAuth_Control_OP (command 12) 69, then each op the TA answers (68, 70 with a timeout in ms),
 * op 67 = update_cbge: per slot 0..15 the slot number as a string at +0x10, no data (+0xfa110 = 0), the slot byte at
 * +0x177014, the output room 0x96000 at +0x271014; finally 71. The sensor's IPC line (GPA1-3) asks for this.
 */
static uint8_t *fp_cbge_slot[16];
static uint32_t fp_cbge_slot_len[16];

/* load_cbge (0x4b484): BAuth_Control_OP 66 for slot 0: the slot as a string at +0x10, the blob at +0x110, its length at
 * +0xfa110, the slot number at +0x271014; then follow the ops the TA answers (68, 70 timeout ms) to 0. The TA's error. */
static uint32_t
fp_cbge_load(void)
{
	uint32_t *r = (uint32_t *)(fp_buf + 0x177008);
	uint32_t e = 0, k, slot, loaded = 0;

	for (slot = 0; slot < 16; slot++) {
		uint32_t op = 66;

		for (k = 0; k < 20; k++) {
			uint32_t origin = 0, result;

			bzero(fp_buf, 0x120);
			bzero(fp_buf + 0x177000, 0x40);
			*(uint32_t *)(fp_buf + 0x0) = 12;
			*(uint32_t *)(fp_buf + 0x4) = 12;
			*(uint32_t *)(fp_buf + 0xc) = op;
			snprintf((char *)fp_buf + 0x10, 0x100, "%u", slot);
			if (op == 66 && fp_cbge_slot[slot] != NULL) {
				memcpy(fp_buf + 0x110, fp_cbge_slot[slot], fp_cbge_slot_len[slot]);
				*(uint32_t *)(fp_buf + 0xfa110) = fp_cbge_slot_len[slot];
			} else {
				*(uint32_t *)(fp_buf + 0xfa110) = 0;
			}
			*(uint32_t *)(fp_buf + 0x271014) = slot;
			*(uint32_t *)(fp_buf + 0x2ee008) = 0;
			lck_mtx_lock(&tee_client_lock);
			result = tee_invoke_shm(&fp_ses, 12, fp_shm_id, FP_SHM_SIZE, &origin);
			lck_mtx_unlock(&tee_client_lock);
			e = result != 0 ? result : *(uint32_t *)(fp_buf + 0x2ee008);
			tee_say("fp cbge-load: slot %u op %u (%u bytes) -> error %#x op %u\n", slot, op,
			    op == 66 && fp_cbge_slot[slot] ? fp_cbge_slot_len[slot] : 0, e, r[0]);
			if (e != 0 || r[0] == 0 || r[0] == op) {
				break;
			}
			if (r[0] == 70 && r[2] != 0 && r[2] < 5000) {
				delay(r[2] * 1000);
			}
			op = r[0];
		}
		if (e == 0 && fp_cbge_slot[slot] != NULL) {
			loaded++;
		}
	}
	tee_say("fp cbge-load: %u slots with data\n", loaded);
	return e;
}

static void
fp_cbge(void)
{
	uint32_t *r = (uint32_t *)(fp_buf + 0x177008);
	uint32_t op = 69, e = 0, k, slot, done = 0;
	volatile uint8_t *gpa1 = redwood_gpio_bank(0x15850000, 0x20);

	tee_say("fp cbge: start, IPC line %s\n", (*(volatile uint32_t *)(gpa1 + 4) & 0x8) ? "high" : "low");
	for (k = 0; k < 200 && op != 0; k++) {
		if (op == 67) {
			for (slot = 0; slot < 16; slot++) {
				uint32_t sop = 67, j;

				for (j = 0; j < 20; j++) {
					uint32_t origin = 0, result;

					bzero(fp_buf, 0x120);
					bzero(fp_buf + 0x177000, 0x40);
					*(uint32_t *)(fp_buf + 0x0) = 12;
					*(uint32_t *)(fp_buf + 0x4) = 12;
					*(uint32_t *)(fp_buf + 0xc) = sop;
					snprintf((char *)fp_buf + 0x10, 0x100, "%u", slot);
					*(uint32_t *)(fp_buf + 0xfa110) = 0;
					fp_buf[0x177014] = (uint8_t)slot;
					*(uint32_t *)(fp_buf + 0x271014) = 0x96000;
					*(uint32_t *)(fp_buf + 0x2ee008) = 0;
					lck_mtx_lock(&tee_client_lock);
					result = tee_invoke_shm(&fp_ses, 12, fp_shm_id, FP_SHM_SIZE, &origin);
					lck_mtx_unlock(&tee_client_lock);
					e = result != 0 ? result : *(uint32_t *)(fp_buf + 0x2ee008);
					tee_say("fp cbge: slot %u op %u -> error %#x op %u out %u | %08x %08x\n", slot, sop, e, r[0],
					    *(uint32_t *)(fp_buf + 0x271014), r[1], r[2]);
					if (e == 0) {
						uint32_t out = *(uint32_t *)(fp_buf + 0x271014);

						if (out != 0 && out <= 0x96000) {
							if (fp_cbge_slot[slot] != NULL && fp_cbge_slot_len[slot] < out) {
								kfree_data(fp_cbge_slot[slot], 0x96000);
								fp_cbge_slot[slot] = NULL;
							}
							if (fp_cbge_slot[slot] == NULL) {
								fp_cbge_slot[slot] = kalloc_data(0x96000, Z_WAITOK | Z_ZERO);
							}
							if (fp_cbge_slot[slot] != NULL) {
								memcpy(fp_cbge_slot[slot], fp_buf + 0x177014, out);
								fp_cbge_slot_len[slot] = out;
							}
						}
					}
					if (e != 0 || r[0] == 0 || r[0] == 67) {
						break;
					}
					if (r[0] == 70 && r[2] != 0 && r[2] < 5000) {
						delay(r[2] * 1000);
					}
					sop = r[0];
				}
				if (e == 0) {
					done++;
				}

			}
			op = r[0] == 67 ? 0 : r[0];
			continue;
		}
		*(uint32_t *)(fp_buf + 0xfa110) = 0;
		*(uint32_t *)(fp_buf + 0x271014) = 0;
		e = fp_call(12, op);
		tee_say("fp cbge: op %u -> error %#x op %u timeout %u\n", op, e, r[0], r[2]);
		if (e != 0) {
			break;
		}
		if (r[0] == 70 && r[2] != 0 && r[2] < 5000) {
			delay(r[2] * 1000);
		}
		op = r[0];
	}
	*(uint32_t *)(fp_buf + 0xfa110) = 0;
	*(uint32_t *)(fp_buf + 0x271014) = 0;
	k = fp_call(12, 71);
	delay(50000);
	snprintf(tee_last, sizeof(tee_last), "fp cbge: last error %#x, %u slots, finish %#x, IPC line %s", e, done, k,
	    (*(volatile uint32_t *)(gpa1 + 4) & 0x8) ? "high" : "low");
	tee_say("%s\n", tee_last);
}

/* Structured research exchange. Fixed paths, root-owned private files only.
 * This is a manually armed FINGE client, not an authenticator or native unlock API.
 * Keeping packing in the host tool avoids a reboot for every protocol experiment.
 */
#define FP_RESEARCH_DIR "/var/root/fp-research"
#define FP_REQUEST_PATH FP_RESEARCH_DIR "/request.bin"
#define FP_RESPONSE_PATH FP_RESEARCH_DIR "/reply.bin"

static int
fp_private_attr(vnode_t vp, enum vtype type, mode_t mode, off_t *size)
{
 struct vnode_attr va;
 int err;

 if (vnode_vtype(vp) != type) {
  return EINVAL;
 }
 VATTR_INIT(&va);
 VATTR_WANTED(&va, va_uid);
 VATTR_WANTED(&va, va_mode);
 VATTR_WANTED(&va, va_nlink);
 VATTR_WANTED(&va, va_data_size);
 err = vnode_getattr(vp, &va, vfs_context_kernel());
 if (err != 0) {
  return err;
 }
 if (!VATTR_IS_SUPPORTED(&va, va_uid) || !VATTR_IS_SUPPORTED(&va, va_mode) ||
     !VATTR_IS_SUPPORTED(&va, va_nlink) || !VATTR_IS_SUPPORTED(&va, va_data_size) ||
     va.va_uid != 0 || (va.va_mode & 07777) != mode || (type == VREG && va.va_nlink != 1)) {
  return EPERM;
 }
 if (size != NULL) {
  *size = va.va_data_size;
 }
 return 0;
}

static int
fp_private_parent(void)
{
 vnode_t vp = NULLVP;
 vfs_context_t ctx = vfs_context_kernel();
 int err = vnode_open(FP_RESEARCH_DIR, FREAD | O_NOFOLLOW | O_NONBLOCK, 0, 0, &vp, ctx);

 if (err == 0) {
  err = fp_private_attr(vp, VDIR, 0700, NULL);
  vnode_close(vp, FREAD, ctx);
 }
 return err;
}

static int
fp_exchange_read(uint8_t **request)
{
 vnode_t vp = NULLVP;
 vfs_context_t ctx = vfs_context_kernel();
 off_t size = 0;
 int resid = 0, err;

 *request = NULL;
 err = vnode_open(FP_REQUEST_PATH, FREAD | O_NOFOLLOW | O_NONBLOCK, 0, 0, &vp, ctx);
 if (err != 0) {
  return err;
 }
 err = fp_private_attr(vp, VREG, 0600, &size);
 if (err == 0 && size != FP_SHM_SIZE) {
  err = EINVAL;
 }
 if (err == 0 && (*request = tz_alloc(FP_SHM_SIZE)) == NULL) {
  err = ENOMEM;
 }
 if (err == 0) {
  err = vn_rdwr(UIO_READ, vp, (caddr_t)*request, FP_SHM_SIZE, 0, UIO_SYSSPACE,
      IO_NODELOCKED, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
  if (err == 0 && (resid != 0 || !fp_wire_request_valid(*request, FP_SHM_SIZE))) {
   err = EINVAL;
  }
 }
 vnode_close(vp, FREAD, ctx);
 if (err != 0 && *request != NULL) {
  bzero(*request, FP_SHM_SIZE);
  tz_free(*request, FP_SHM_SIZE);
  *request = NULL;
 }
 return err;
}

static int
fp_exchange_write(uint32_t result, uint32_t origin, uint32_t cmd)
{
 vnode_t vp = NULLVP;
 vfs_context_t ctx = vfs_context_kernel();
 struct vnode_attr va;
 uint32_t header[5] = { 0x31525046, result, origin, cmd, FP_SHM_SIZE }; /* FPR1 */
 int resid = 0, err;

 err = vnode_open(FP_RESPONSE_PATH, FWRITE | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600, 0, &vp, ctx);
 if (err != 0) {
  return err;
 }
 err = fp_private_attr(vp, VREG, 0600, NULL);
 if (err == 0) {
  VATTR_INIT(&va);
  VATTR_SET(&va, va_data_size, 0);
  err = vnode_setattr(vp, &va, ctx);
 }
 if (err == 0) {
  err = vn_rdwr(UIO_WRITE, vp, (caddr_t)header, sizeof(header), 0, UIO_SYSSPACE,
      IO_NODELOCKED | IO_SYNC, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
  if (err == 0 && resid != 0) {
   err = EIO;
  }
 }
 if (err == 0) {
  err = vn_rdwr(UIO_WRITE, vp, (caddr_t)fp_buf, FP_SHM_SIZE, sizeof(header), UIO_SYSSPACE,
      IO_NODELOCKED | IO_SYNC, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
  if (err == 0 && resid != 0) {
   err = EIO;
  }
 }
 vnode_close(vp, FWRITE, ctx);
 return err;
}

static void
fp_exchange(void)
{
 uint8_t *request = NULL;
 uint32_t result = 0, origin = 0, cmd = 0;
 int err = fp_private_parent();

 if (err == 0) {
  err = fp_exchange_read(&request);
 }
 if (err == 0) {
  cmd = fp_wire_u32(request);
  lck_mtx_lock(&tee_client_lock);
  memcpy(fp_buf, request, FP_SHM_SIZE);
  result = tee_invoke_shm(&fp_ses, cmd, fp_shm_id, FP_SHM_SIZE, &origin);
  lck_mtx_unlock(&tee_client_lock);
  err = fp_exchange_write(result, origin, cmd);
  bzero(fp_buf, FP_SHM_SIZE);
 }
 if (request != NULL) {
  bzero(request, FP_SHM_SIZE);
  tz_free(request, FP_SHM_SIZE);
 }
 snprintf(tee_last, sizeof(tee_last), "fp exchange: done rc %d transport %#x origin %u cmd %u", err, result, origin, cmd);
 tee_say("%s\n", tee_last);
}

/* "N [HEX]": command N with data HEX; the reply's head into tee_last and the log */
static void
fp_cmd_execute(void)
{
	uint32_t n = 0, len = 0, result, origin = 0, rlen, status, handle, error;
	const char *p = fp_cmd_text;
	char hex[200];
	size_t at = 0;

	if (fp_ensure() != 0) {
		return;
	}
	while (*p == ' ') {
		p++;
	}
	if (strcmp(p, "exchange") == 0) {
		fp_exchange();
		return;
	}
	if (strncmp(p, "power", 5) == 0) {
		int power_error = fp_power();
		snprintf(tee_last, sizeof(tee_last), "fp power: configured, error %d", power_error);
		return;
	}
	if (strncmp(p, "prepare", 7) == 0) {
		uint32_t op = 0, out = 0, e = fp_prepare(&op, &out);

		snprintf(tee_last, sizeof(tee_last), "fp prepare: op %u metadata in %u out %u: error %#x", op,
		    op == 1 ? fp_meta_len : 0, out, e);
		tee_say("%s\n", tee_last);
		return;
	}
	if (strncmp(p, "idinit", 6) == 0) {
		uint32_t e = fp_idinit(0);

		snprintf(tee_last, sizeof(tee_last), "fp idinit: error %#x op %u template %u bytes | %08x %08x %08x", e,
		    *(uint32_t *)(fp_buf + 0x177008), fp_tmpl_len, *(uint32_t *)(fp_buf + 0x17700c),
		    *(uint32_t *)(fp_buf + 0x177010), *(uint32_t *)(fp_buf + 0x177014));
		tee_say("%s\n", tee_last);
		return;
	}
	if (strncmp(p, "cbgeefs", 7) == 0) {
		/*
		 * The phone's factory calibration, as stock loads it (FPBAuthServiceStorage readCBGEData from
		 * /efs/biometrics/meta/cbge_%d.dat, sec_efs partition): copied to /var/root/fp-calib/cbge_N.dat.
		 */
		uint32_t slot, n = 0;
		char path[64];

		for (slot = 0; slot < 16; slot++) {
			size_t size = 0;
			void *buf;

			snprintf(path, sizeof(path), "/var/root/fp-calib/cbge_%u.dat", slot);
			if ((buf = tee_read_file(path, &size)) == NULL) {
				continue;
			}
			if (size != 0 && size <= 0x96000) {
				if (fp_cbge_slot[slot] == NULL) {
					fp_cbge_slot[slot] = kalloc_data(0x96000, Z_WAITOK | Z_ZERO);
				}
				if (fp_cbge_slot[slot] != NULL) {
					memcpy(fp_cbge_slot[slot], buf, size);
					fp_cbge_slot_len[slot] = (uint32_t)size;
					n++;
				}
			}
			tz_free(buf, size);
		}
		snprintf(tee_last, sizeof(tee_last), "fp cbgeefs: %u factory slots read, load error %#x", n, n ? fp_cbge_load() : 0);
		tee_say("%s\n", tee_last);
		return;
	}
	if (strncmp(p, "cbgeload", 8) == 0) {
		snprintf(tee_last, sizeof(tee_last), "fp cbgeload: error %#x", fp_cbge_load());
		return;
	}
	if (strncmp(p, "cbge", 4) == 0) {
		fp_cbge();
		return;
	}
	if (strncmp(p, "einit", 5) == 0) {
		/*
		 * ENROLL_INIT as Samsung's enroll() sends it (libbauthtzcommon BAuth_Enroll_Init): +0x10 the user id (a
		 * string, <= 0x100), +0x110 the finger's index (1-4, BAuthUserInfo get_req_bioindex), +0x114 7, +0x118 a
		 * storage path (<= 100). "einit [INDEX]".
		 */
		uint32_t idx = 0, e;

		for (p += 5; *p == ' '; p++) {
		}
		while (*p >= '0' && *p <= '9') {
			idx = idx * 10 + (uint32_t)(*p++ - '0');
		}
		if (idx < 1 || idx > 4) {
			idx = 1;
		}
		e = fp_einit(idx);
		snprintf(tee_last, sizeof(tee_last), "fp einit %u: result %#x error %#x op %u | %08x %08x %08x", idx, e,
		    *(uint32_t *)(fp_buf + 0x2ee008), *(uint32_t *)(fp_buf + 0x177008), *(uint32_t *)(fp_buf + 0x17700c),
		    *(uint32_t *)(fp_buf + 0x177010), *(uint32_t *)(fp_buf + 0x177014));
		tee_say("%s\n", tee_last);
		return;
	}
	if (strncmp(p, "run ", 4) == 0) {
		uint32_t v[4] = { 0, 0, 10, 0 }, k = 0;

		for (p += 4; *p && k < 4; k++) {
			v[k] = 0;
			while (*p == ' ') {
				p++;
			}
			while (*p >= '0' && *p <= '9') {
				v[k] = v[k] * 10 + (uint32_t)(*p++ - '0');
			}
		}
		fp_run(v[0], v[1], v[2] ? v[2] : 10, v[3]);
		return;
	}
	while (*p >= '0' && *p <= '9') {
		n = n * 10 + (uint32_t)(*p++ - '0');
	}
	if (*p == ':') {
		fp_handle = 0;
		for (p++; *p >= '0' && *p <= '9'; p++) {
			fp_handle = fp_handle * 10 + (uint32_t)(*p - '0');
		}
	}
	bzero(fp_buf, 0x10);
	bzero(fp_buf + 0x177000, 0x20);
	*(uint32_t *)(fp_buf + 0x0) = n;
	*(uint32_t *)(fp_buf + 0x4) = n;
	*(uint32_t *)(fp_buf + 0xc) = fp_handle;
	while (*p == ' ') {
		p++;
	}
	while (p[0] && p[1] && fp_hex(p[0]) >= 0 && fp_hex(p[1]) >= 0 && len < 0xa000) {
		fp_buf[0x10 + len++] = (uint8_t)(fp_hex(p[0]) << 4 | fp_hex(p[1]));
		p += 2;
	}
	*(uint32_t *)(fp_buf + 0xa010) = len;
	*(uint32_t *)(fp_buf + 0x181014) = 0;
	*(uint32_t *)(fp_buf + 0x2ee008) = 0;
	lck_mtx_lock(&tee_client_lock);
	result = tee_invoke_shm(&fp_ses, n, fp_shm_id, FP_SHM_SIZE, &origin);
	lck_mtx_unlock(&tee_client_lock);
	status = *(uint32_t *)(fp_buf + 0x177004);
	handle = *(uint32_t *)(fp_buf + 0x177008);
	rlen = *(uint32_t *)(fp_buf + 0x181014);
	error = *(uint32_t *)(fp_buf + 0x2ee008);
	if (result == 0) {
		fp_handle = handle;
	}
	for (uint32_t i = 0; i < rlen && i < 64 && at + 3 < sizeof(hex); i++) {
		at += (size_t)snprintf(hex + at, sizeof(hex) - at, "%02x", fp_buf[0x177014 + i]);
	}
	hex[at] = 0;
	snprintf(tee_last, sizeof(tee_last), "fp %u (%u bytes): result %#x origin %u status %#x opcode %#x error %#x timeout %u reply %u: %s",
	    n, len, result, origin, status, handle, error, *(uint32_t *)(fp_buf + 0x177010), rlen, hex);
	tee_say("%s\n", tee_last);
}

static void
fp_cmd_thread(__unused void *arg, __unused wait_result_t wr)
{
 fp_cmd_execute();
 lck_mtx_lock(&fp_command_lock);
 fp_command_active = false;
 lck_mtx_unlock(&fp_command_lock);
}

static int
tee_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char in[300] = "", out[1024];
	int error, changed = 0;
	thread_t t;

	lck_mtx_lock(&fp_command_lock);
	snprintf(out, sizeof(out), "%s\nsmcs %llu schedules %llu event irqs %llu panic irqs %llu cpu_mask %#x user_mask %#x "
	    "pending cmds %d log lines %llu ree_time %llu\nstorage requests %llu unanswered %llu last cmd %u pending %d\n"
	    "RPMB irqs %d requests %llu errors %llu\nfingerprint wire: exchange-v2 (research only), active %u\nfingerprint ui: reject-v1 (no authentication)\nlast: %s\n",
	    tee_status, tee_smcs, tee_schedules, tz_events_irq, tz_panic_irq, tz_cpu_mask(), tz_user_cpu_mask(), tz_nr_cmds,
	    iwlog_lines, ree_time_answers, ts_requests, ts_unanswered, ts_last_cmd, ts_pending, srpmb_irqs, srpmb_requests,
	    srpmb_errors, fp_command_active ? 1 : 0, tee_last);
	lck_mtx_unlock(&fp_command_lock);
	/* (sysctl_io_string takes a written value into `out` itself: that is the command) */
	error = sysctl_io_string(req, out, sizeof(out), 0, &changed);
	if (error || req->newptr == USER_ADDR_NULL || !changed) {
		return error;
	}
	strlcpy(in, out, sizeof(in));
	if (strncmp(in, "start", 5) == 0) {
		if (tee_state != TEE_DOWN) {
			return EALREADY;
		}
		tee_state = TEE_STARTING;
		snprintf(tee_status, sizeof(tee_status), "TEEGRIS: starting");
		kernel_thread_start(tee_start_thread, NULL, &t);
		thread_deallocate(t);
		return 0;
	}
	if (strncmp(in, "fp ", 3) == 0) {
		if (tee_state != TEE_UP) {
			return ENXIO;
		}
		lck_mtx_lock(&fp_command_lock);
		if (strcmp(in, "fp stop") == 0) {
			/* Cooperative cancellation only. Never race a new TA call against
			 * the worker, and never interpret cancellation as authentication. */
			__atomic_store_n(&fp_run_stopping, true, __ATOMIC_RELEASE);
			lck_mtx_unlock(&fp_command_lock);
			return 0;
		}
		if (fp_command_active) {
			lck_mtx_unlock(&fp_command_lock);
			return EBUSY;
		}
		__atomic_store_n(&fp_run_stopping, false, __ATOMIC_RELEASE);
		fp_command_active = true;
		strlcpy(fp_cmd_text, in + 3, sizeof(fp_cmd_text));
		snprintf(tee_last, sizeof(tee_last), "fp %s: in progress", fp_cmd_text);
		lck_mtx_unlock(&fp_command_lock);
		if (kernel_thread_start(fp_cmd_thread, NULL, &t) != KERN_SUCCESS) {
			lck_mtx_lock(&fp_command_lock);
			fp_command_active = false;
			lck_mtx_unlock(&fp_command_lock);
			return EIO;
		}
		thread_deallocate(t);
		return 0;
	}
	if (strncmp(in, "open ", 5) == 0 || strncmp(in, "openkeep ", 9) == 0) {
		if (tee_state != TEE_UP) {
			return ENXIO;
		}
		tee_open_keep = in[4] == 'k';
		strlcpy(tee_open_path, in + (tee_open_keep ? 9 : 5), sizeof(tee_open_path));
		kernel_thread_start(tee_open_test_thread, NULL, &t);
		thread_deallocate(t);
		return 0;
	}
	return EINVAL;
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, tee, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, tee_sysctl, "A",
    "TEEGRIS: write start | open PATH; read the state");
SYSCTL_PROC(_kern_redwood, OID_AUTO, tee_ts, CTLTYPE_OPAQUE | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, tee_ts_sysctl, "S",
    "TEEGRIS trusted storage mailbox: read the pending request buffer, write the answered one (teetsd)");

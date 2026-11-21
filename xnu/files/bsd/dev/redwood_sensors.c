/*
 * /dev/redwood-sensors (sys/redwood_sensors.h): one queue of samples that every sensor driver of the S20 posts into
 * (bsd/dev/redwood_ssp.c, the hub; bsd/dev/redwood_tcs3407.c, the rear light sensor) and backboardd reads
 * (userland/src/inject_sensors.c). The device is made by whichever driver asks first, so it is there before backboardd
 * starts; nothing is queued while nobody has it open. Taken from Scion's drivers/input/ssp/hakios.ssp.c (S9), where the
 * queue lived inside the hub's driver.
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/redwood_sensors.h>
#include <sys/systm.h>
#include <sys/uio.h>

#define QUEUE 256

static LCK_GRP_DECLARE(sensors_lck_grp, "redwood-sensors");
static LCK_MTX_DECLARE(sensors_lock, &sensors_lck_grp);

static struct redwood_sensor_sample queue[QUEUE];
static uint32_t q_head, q_tail, q_lost;
static bool q_waiting, made;
static int q_readers;
static uint32_t proximity_mode;

uint32_t
redwood_sensors_proximity_mode(void)
{
	return __atomic_load_n(&proximity_mode, __ATOMIC_RELAXED);
}

static int
sensors_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, int flags, __unused proc_t p)
{
	if (cmd != REDWOOD_SENSORS_PROXIMITY) return ENOTTY;
	if (!(flags & FWRITE)) return EBADF;
	__atomic_store_n(&proximity_mode, *(uint32_t *)data, __ATOMIC_RELAXED);
	return 0;
}

void
redwood_sensors_post(const struct redwood_sensor_sample *s)
{
	lck_mtx_lock(&sensors_lock);
	if (q_readers > 0) {
		if (q_head - q_tail >= QUEUE) {
			q_tail++;
			q_lost++;
		}
		queue[q_head++ % QUEUE] = *s;
		if (q_waiting) {
			q_waiting = false;
			wakeup(&q_head);
		}
	}
	lck_mtx_unlock(&sensors_lock);
}

int
redwood_sensors_readers(void)
{
	return q_readers;
}

static int
sensors_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	lck_mtx_lock(&sensors_lock);
	q_readers++;
	q_tail = q_head;
	lck_mtx_unlock(&sensors_lock);
	return 0;
}

static int
sensors_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	lck_mtx_lock(&sensors_lock);
	if (q_readers > 0) {
		q_readers--;
	}
	if (!q_readers) __atomic_store_n(&proximity_mode, 0, __ATOMIC_RELAXED);
	lck_mtx_unlock(&sensors_lock);
	return 0;
}

static int
sensors_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int error = 0;

	lck_mtx_lock(&sensors_lock);
	while (q_head == q_tail) {
		q_waiting = true;
		error = msleep(&q_head, &sensors_lock, PZERO | PCATCH, REDWOOD_SENSORS_NAME, NULL);
		if (error != 0) {
			lck_mtx_unlock(&sensors_lock);
			return error;
		}
	}
	while (q_head != q_tail && uio_resid(uio) >= (user_ssize_t)sizeof(struct redwood_sensor_sample)) {
		struct redwood_sensor_sample o = queue[q_tail++ % QUEUE];

		lck_mtx_unlock(&sensors_lock);
		error = uiomove((caddr_t)&o, sizeof(o), uio);
		lck_mtx_lock(&sensors_lock);
		if (error != 0) {
			break;
		}
	}
	lck_mtx_unlock(&sensors_lock);
	return error;
}

static const struct cdevsw sensors_cdevsw = {
	.d_open = sensors_open, .d_close = sensors_close, .d_read = sensors_read, .d_write = eno_rdwrt,
	.d_ioctl = sensors_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0,
};

void
redwood_sensors_device(void)
{
	int major;

	lck_mtx_lock(&sensors_lock);
	if (made) {
		lck_mtx_unlock(&sensors_lock);
		return;
	}
	made = true;
	lck_mtx_unlock(&sensors_lock);
	major = cdevsw_add(-1, &sensors_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666,
	    REDWOOD_SENSORS_NAME) == NULL) {
		printf("sensors: /dev/%s could not be made\n", REDWOOD_SENSORS_NAME);
		return;
	}
	printf("sensors: /dev/%s made\n", REDWOOD_SENSORS_NAME);
}


/*
 * A driver line into the kernel's log and mali2metal's channel too (/m2m on rwlogd): the log keeps the first 256 KB
 * of a boot and the newest part, and the drivers' first lines fall between (2026-11-06: not one of the vibration
 * driver's was left). For the sensor drivers (redwood_ssp.c, redwood_tcs3407.c, redwood_acpm_pmic.c,
 * redwood_exynos_spi.c).
 */
#include <sys/redwood_m2m.h>
#include <stdarg.h>
extern boolean_t ml_get_interrupts_enabled(void);
extern void loader_log_line(const char *line);

int
redwood_dsay(const char *format, ...)
{
	char line[300];
	va_list ap;
	size_t n;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	/*
	 * With interrupts off (loader_return() from the stop IPI: Log access, the menu, a panic) the console
	 * panics ("Console I/O from interrupt-disabled context") and m2m may sleep: the loader's log only.
	 */
	if (!ml_get_interrupts_enabled()) {
		loader_log_line(line);
		return 0;
	}
	printf("%s", line);
	n = strlen(line);
	if (n > 0 && line[n - 1] == '\n') {
		line[n - 1] = 0;
	}
	m2m_klog('I', "DEV", "%s", line);
	return 0;
}

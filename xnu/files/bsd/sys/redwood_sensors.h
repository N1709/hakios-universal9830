/*
 * /dev/redwood-sensors: the phone's motion and environment sensors, sample by sample, from the kernel drivers that read
 * them to the code of ours in backboardd that turns them into the HID events iOS's own consumers read
 * (userland/src/inject_sensors.c). On the Galaxy S20 (x1slte) two drivers feed it:
 *   bsd/dev/redwood_ssp.c      the sensor hub in the BCM4775 (shub=6): accelerometer, gyroscope, magnetometer, barometer,
 *                              the front light sensor and the proximity sensor (both the TMD4907 under the screen)
 *   bsd/dev/redwood_tcs3407.c  the rear light/flicker sensor TCS3407 on HSI2C_25 (als=3): light only, source REAR
 * The device itself is bsd/dev/redwood_sensors.c. A queue: read() blocks until there is at least one sample and returns
 * as many whole ones as fit. The queue only fills while the device is open; a reader 256 samples behind loses the oldest.
 *
 * Values are the hub's own, in Android's frame (x right, y up, z out of the screen, the device tree's positions already
 * applied by the hub):
 *   ACCEL      v[0..2] x, y, z; scale = the range in g (32768 counts are scale g). Lying face up z is +1 g (iOS: -1 g)
 *   GYRO       v[0..2] x, y, z in the hub's units (Scion's S9 notes: about 1/16.4 thousandths of a degree a second)
 *   MAG        v[0..2] x, y, z (calibrated), v[3] the accuracy 0-3
 *   PRESSURE   v[0] the pressure (LPS22HH through the hub), v[1] the temperature
 *   LIGHT      v[0] lux, v[1] the colour temperature in K (0: none), v[2] REDWOOD_LIGHT_FRONT or _REAR, v[3] raw clear
 *   PROXIMITY  v[0] 1 when something is near, 0 when not; v[1] the raw reading
 */
#ifndef _SYS_REDWOOD_SENSORS_H_
#define _SYS_REDWOOD_SENSORS_H_

#include <stdint.h>
#include <sys/ioccom.h>

/* Native HID active/monitoring demand, consumed by the hub's SPI owner. */
#define REDWOOD_SENSORS_PROXIMITY _IOW('R', 1, uint32_t)

#define REDWOOD_SENSORS_NAME    "redwood-sensors"
#define REDWOOD_SENSORS_DEVICE  "/dev/redwood-sensors"

#define REDWOOD_SENSOR_ACCEL     1
#define REDWOOD_SENSOR_GYRO      2
#define REDWOOD_SENSOR_MAG       3
#define REDWOOD_SENSOR_PRESSURE  4
#define REDWOOD_SENSOR_LIGHT     5
#define REDWOOD_SENSOR_PROXIMITY 6

#define REDWOOD_LIGHT_FRONT      0      /* the hub's TMD4907, under the screen: what auto-brightness wants */
#define REDWOOD_LIGHT_REAR       1      /* the TCS3407 next to the rear cameras */

struct redwood_sensor_sample {
	uint64_t time;                  /* mach_absolute_time() when the kernel got it */
	uint64_t hub_ns;                /* the hub's own time stamp in ns (0 for the TCS3407) */
	uint32_t type;                  /* REDWOOD_SENSOR_* */
	int32_t scale;                  /* ACCEL: the range in g; else 0 */
	int32_t v[4];
};

#ifdef KERNEL
/* bsd/dev/redwood_sensors.c: the device made once (by the first driver that asks), a sample queued for its readers */
void redwood_sensors_device(void);
void redwood_sensors_post(const struct redwood_sensor_sample *s);
int redwood_sensors_readers(void);
uint32_t redwood_sensors_proximity_mode(void);
void redwood_als_publish(void);

void redwood_ssp_init(void);            /* boot-arg shub=N: the S20's sensor hub (bsd/dev/redwood_ssp.c) */
void redwood_ssp_quiesce(void);         /* the hub into standby (NSTDBY low): the run's end, a panic's way out */
void redwood_tcs3407_init(void);        /* boot-arg als=N: the rear light sensor (bsd/dev/redwood_tcs3407.c) */
void redwood_tcs3407_quiesce(void);     /* its ALS off (ENABLE 0) */
#endif

int redwood_dsay(const char *format, ...) __printflike(1, 2);   /* printf + m2m (redwood_sensors.c) */
#endif /* _SYS_REDWOOD_SENSORS_H_ */

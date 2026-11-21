/* GPIO-I2C isolation under Samsung. Only GPP0-2/3 are requested, open-drain.
 * The reference owns the sensor exclusively while replaying; restore the mux
 * before any further hardware-I2C operation or V4L2 teardown.
 */
#include <linux/gpio.h>
#include <time.h>

static int gpio_bus_fd = -1;
static struct gpiohandle_data gpio_bus_lines = {.values = {1, 1}};
static bool gpio_bus_error;
static unsigned gpio_bus_transfers, gpio_bus_nacks;

static uint64_t gpio_bus_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec*1000000000ULL + t.tv_nsec;
}

static void gpio_bus_wait(void)
{
    uint64_t until = gpio_bus_ns() + 4000;
    while (gpio_bus_ns() < until) { }
}

static void gpio_bus_set(unsigned pin, unsigned value)
{
    gpio_bus_lines.values[pin] = value;
    if (ioctl(gpio_bus_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &gpio_bus_lines) < 0)
        gpio_bus_error = true;
}

static unsigned gpio_bus_get(unsigned pin)
{
    struct gpiohandle_data data = {0};
    if (ioctl(gpio_bus_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) < 0)
        gpio_bus_error = true;
    return data.values[pin];
}

static void gpio_scl_release(void)
{
    uint64_t until = gpio_bus_ns() + 25000000ULL;
    gpio_bus_set(0, 1);
    while (!gpio_bus_error && !gpio_bus_get(0)) {
        if (gpio_bus_ns() >= until) { gpio_bus_error = true; break; }
    }
}

static void gpio_bus_start(void)
{
    gpio_bus_set(1, 1); gpio_bus_wait();
    gpio_scl_release(); gpio_bus_wait();
    gpio_bus_set(1, 0); gpio_bus_wait();
    gpio_bus_set(0, 0); gpio_bus_wait();
}

static void gpio_bus_stop(void)
{
    gpio_bus_set(1, 0); gpio_bus_wait();
    gpio_scl_release(); gpio_bus_wait();
    gpio_bus_set(1, 1); gpio_bus_wait();
}

static bool gpio_bus_write(uint8_t byte)
{
    for (int i = 7; i >= 0 && !gpio_bus_error; i--) {
        gpio_bus_set(1, (byte >> i) & 1); gpio_bus_wait();
        gpio_scl_release(); gpio_bus_wait();
        gpio_bus_set(0, 0);
    }
    gpio_bus_set(1, 1); gpio_bus_wait();
    gpio_scl_release(); gpio_bus_wait();
    bool ack = !gpio_bus_get(1);
    gpio_bus_set(0, 0); gpio_bus_wait();
    if (!ack) gpio_bus_nacks++;
    return ack && !gpio_bus_error;
}

static uint8_t gpio_bus_read(bool ack)
{
    uint8_t value = 0;
    gpio_bus_set(1, 1);
    for (unsigned i = 0; i < 8 && !gpio_bus_error; i++) {
        gpio_bus_wait(); gpio_scl_release(); gpio_bus_wait();
        value = (value << 1) | gpio_bus_get(1);
        gpio_bus_set(0, 0);
    }
    gpio_bus_set(1, !ack); gpio_bus_wait();
    gpio_scl_release(); gpio_bus_wait();
    gpio_bus_set(0, 0); gpio_bus_set(1, 1);
    return value;
}

static int gpio_sensor_transfer(const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
    bool ok;
    gpio_bus_error = false;
    gpio_bus_transfers++;
    gpio_bus_start();
    ok = gpio_bus_write(0x20);
    for (unsigned i = 0; ok && i < wn; i++) ok = gpio_bus_write(w[i]);
    if (ok && rn) {
        gpio_bus_start();
        ok = gpio_bus_write(0x21);
        for (unsigned i = 0; ok && i < rn; i++) {
            r[i] = gpio_bus_read(i+1 < rn);
            ok = !gpio_bus_error;
        }
    }
    gpio_bus_stop();
    return ok && !gpio_bus_error ? 0 : -1;
}

static void replay_gpio_sensor(void)
{
    uint8_t count = 0;
    uint16_t id = 0;
    uint32_t saved_mux = 0;
    bool mux_saved = false;
    for (unsigned i = 0; i < 100; i++) {
        if (sensor_read8(0x0005, &count) < 0) break;
        if (count == 0xff) break;
        usleep(1000);
    }
    if (count != 0xff) {
        puts("GPIO_SENSOR_REPLAY refused: sensor not in standby"); return;
    }
    receiver_failed = false;
    receiver_kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (receiver_kmsg < 0) return;
    saved_mux = receiver_rd(0x10430000, 0);
    if (receiver_failed || ((saved_mux >> 8) & 0xff) != 0x33) goto done;
    mux_saved = true;
    for (unsigned i = 0; i < 128; i++) {
        char path[64]; struct gpiochip_info info = {0};
        snprintf(path, sizeof(path), "/dev/gpiochip%u", i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 && !strcmp(info.label, "gpp0") && info.lines >= 4) {
            struct gpiohandle_request req = { .lineoffsets = {2,3},
                .flags = GPIOHANDLE_REQUEST_OUTPUT | GPIOHANDLE_REQUEST_OPEN_DRAIN,
                .default_values = {1,1}, .consumer_label = "camera-gpio-replay", .lines = 2 };
            if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req) == 0) gpio_bus_fd = req.fd;
            else printf("GPIO_SENSOR_REPLAY request failed errno=%d\n", errno);
            close(fd); break;
        }
        close(fd);
    }
    if (gpio_bus_fd < 0) goto done;
    gpio_bus_error = false;
    gpio_bus_lines.values[0] = gpio_bus_lines.values[1] = 1;
    if (!gpio_bus_get(0) || !gpio_bus_get(1) || gpio_bus_error) goto done;
    sensor_gpio_transfer = gpio_sensor_transfer;
    if (sensor_read16(0, &id) || id != 0x30a1) goto done;
    printf("GPIO_SENSOR_REPLAY begin id=%04x\n", id); fflush(stdout);
    replay_xnu_sensor();
    printf("GPIO_SENSOR_REPLAY end transfers=%u nacks=%u io_error=%u\n",
           gpio_bus_transfers, gpio_bus_nacks, gpio_bus_error);
done:
    sensor_gpio_transfer = NULL;
    if (gpio_bus_fd >= 0) { gpio_bus_set(0,1); gpio_bus_set(1,1); close(gpio_bus_fd); gpio_bus_fd = -1; }
    if (mux_saved) {
        receiver_failed = false;
        receiver_update(0x10430000, 0, 0xff00U, saved_mux);
        uint32_t restored = receiver_rd(0x10430000, 0);
        printf("GPIO_SENSOR_REPLAY mux_restore=%s before=%08x after=%08x\n",
               !receiver_failed && ((restored ^ saved_mux) & 0xff00U) == 0 ? "ok" : "failed", saved_mux, restored);
    }
    close(receiver_kmsg); receiver_kmsg = -1;
    fflush(stdout);
}

/* Bounded Samsung V4L2 front sensor stream for the temporary reference boot. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Samsung's videodev2_exynos_camera.h; sensor node s_ctrl starts CSIS + CIS. */
#define V4L2_CID_IS_S_STREAM (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 14)
#define V4L2_CID_IS_G_STREAM (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 15)
#define V4L2_CID_IS_G_MIPI_ERR (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 62)
#define V4L2_CID_IS_S_SENSOR_SIZE (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 69)
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define CAMERA_IMAGE_BYTES (6U * 1024U * 1024U)
#define CAMERA_META_BYTES (1U * 1024U * 1024U)
struct ion_allocation_data {
    uint64_t len;
    uint32_t heap_id_mask, flags, fd, unused;
};
#define CAMERA_CHECKPOINT_OFFSET 0x1e9d04000ULL
static int log_fd = -1;
static unsigned checkpoint_number;
static int pmic_fd = -1;
static uint8_t buck2_before;
static bool buck2_changed;
static int sensor_fd = -1;
static int (*sensor_gpio_transfer)(const uint8_t *, unsigned, uint8_t *, unsigned);

static int sensor_transfer(const uint8_t *write_data, unsigned write_len,
                           uint8_t *read_data, unsigned read_len)
{
    if (sensor_gpio_transfer) return sensor_gpio_transfer(write_data, write_len, read_data, read_len);
    struct i2c_msg msg[] = {
        {.addr = 0x10, .len = write_len, .buf = (uint8_t *)write_data},
        {.addr = 0x10, .flags = I2C_M_RD, .len = read_len, .buf = read_data}
    };
    struct i2c_rdwr_ioctl_data req = {.msgs = msg, .nmsgs = read_len ? 2U : 1U};
    return sensor_fd >= 0 && ioctl(sensor_fd, I2C_RDWR, &req) == (int)req.nmsgs ? 0 : -1;
}

/* sec_debug_test.c::simulate_SFR reads with exactly one address argument.
 * Keep this whitelist in the synchronous stream owner: all these domains are
 * powered until this function returns. Never supply the optional write value.
 * SYSREG_CSIS + 0x504 is deliberately excluded (unsafe under XNU).
 */
static void dump_camera_clocks(void)
{
    static const uint32_t addresses[] = {
        0x1a33104c, 0x1a331848, 0x1a33204c, /* TOP CSIS bus mux/div/gate */
        0x1a33101c, 0x1a331818, 0x1a332020, 0x1a333008, /* CIS_CLK1 */
        0x17000600, 0x17000604, 0x17001800, 0x17001804, /* CSIS bus */
        0x17003058, 0x17003064, 0x17003078, 0x1700307c, /* QCH */
        0x17002000, 0x17002018, 0x1700202c, 0x17002030, /* gates */
        0x17002004, 0x170020d4, 0x170020d8, 0x170020f4, /* oscillator/reset-sync/SYSREG */
        0x17002054, 0x17002070, 0x170030a0, 0x1700310c, /* APB link and SYSREG requests */
        0x170d1010, /* documented shared PHY M1_BIAS_CON4 */
        0x15860730, 0x15861d84, 0x17020500, /* isolation, power, reset */
        0x170d1000, 0x170d1004, 0x170d1008, /* PHY bias */
        0x17040004, 0x17040008, 0x17040500, 0x17040700, 0x17040704,
        0x1a330280, 0x1a33028c, 0x1a331910, /* SHARED4 source read-only */
        0x107300a0, 0x107300a4, 0x107300a8, 0x107300ac, /* GPC0 */
        0x15c30160, 0x15c30164, 0x15c30168, 0x15c3016c, /* GPM11 */
        0x170d2b08, 0x170d2b0c, 0x170d2b18, /* untouched PHY analog controls */
        0x170d2c08, 0x170d2c0c, 0x170d2c18,
        0x170d2d08, 0x170d2d0c, 0x170d2d18,
        0x170d2e08, 0x170d2e0c, 0x170d2e18,
        0x170d2f08, 0x170d2f0c, 0x170d2f18
    };
    const char *path = "/sys/module/sec_debug/parameters/force_error";
    char supported[4096] = {0};
    int fd = open(path, O_RDONLY);
    ssize_t got = fd < 0 ? -1 : read(fd, supported, sizeof(supported) - 1);
    if (fd >= 0) close(fd);
    if (got <= 0 || (strncmp(supported, "SFR\n", 4) && !strstr(supported, "\nSFR\n"))) {
        puts("CAMERA_SFR reader unavailable");
        return;
    }
    for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        char command[32];
        int length = snprintf(command, sizeof(command), "SFR 0x%08x\n", addresses[i]);
        printf("CAMERA_SFR read 0x%08x\n", addresses[i]);
        fflush(stdout);
        fd = open(path, O_WRONLY);
        if (fd < 0) { printf("CAMERA_SFR open failed errno=%d\n", errno); break; }
        ssize_t sent = write(fd, command, (size_t)length);
        close(fd);
        if (sent != length) { printf("CAMERA_SFR read failed errno=%d\n", errno); break; }
    }
    fflush(stdout);
}

#ifdef CAMERA_FULL_REGDUMP
#include "camera-regdump-list.h"
/* Same 273 addresses as XNU's cam.s20.front.stream bit 8, one sec_debug SFR read each (read-only form). */
static void dump_camera_full(const char *tag)
{
    const char *path = "/sys/module/sec_debug/parameters/force_error";
    printf("CAMERA_REGDUMP begin %s count=%d\n", tag, CAM_REGDUMP_COUNT); fflush(stdout);
    for (unsigned i = 0; i < CAM_REGDUMP_COUNT; i++) {
        char command[32];
        int length = snprintf(command, sizeof(command), "SFR 0x%08x\n", cam_regdump_addr[i]);
        int fd = open(path, O_WRONLY);
        if (fd < 0) { printf("CAMERA_REGDUMP open failed errno=%d\n", errno); break; }
        ssize_t sent = write(fd, command, (size_t)length);
        close(fd);
        if (sent != length) { printf("CAMERA_REGDUMP read failed errno=%d at 0x%08x\n", errno, cam_regdump_addr[i]); break; }
    }
    printf("CAMERA_REGDUMP end %s\n", tag); fflush(stdout);
}
#endif

static int sensor_read8(uint16_t reg, uint8_t *value)
{
    uint8_t address[] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return sensor_transfer(address, sizeof(address), value, 1);
}

static int sensor_read16(uint16_t reg, uint16_t *value)
{
    uint8_t address[] = {(uint8_t)(reg >> 8), (uint8_t)reg}, data[2];
    if (sensor_transfer(address, sizeof(address), data, sizeof(data))) return -1;
    *value = ((uint16_t)data[0] << 8) | data[1];
    return 0;
}

static int sensor_write16(uint16_t reg, uint16_t value)
{
    uint8_t data[] = {(uint8_t)(reg >> 8), (uint8_t)reg,
                      (uint8_t)(value >> 8), (uint8_t)value};
    return sensor_transfer(data, sizeof(data), NULL, 0);
}

#ifdef CAMERA_XNU_SENSOR_REPLAY
#include "camera-sensor-replay.h"
#ifdef CAMERA_XNU_RECEIVER_REPLAY
#include "camera-receiver-replay.h"
#ifdef CAMERA_XNU_GPIO_REPLAY
#include "camera-gpio-replay.h"
#endif
#endif
#endif

static void dump_sensor_indirect(void)
{
    static const uint16_t addresses[] = {0x918c, 0x918e, 0x9190, 0x9192, 0x9256};
    if (sensor_write16(0x602c, 0x2000) != 0) {
        printf("SENSOR_CODE page-write failed errno=%d\n", errno);
        fflush(stdout);
        return;
    }
    for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        uint16_t value = 0;
        if (sensor_write16(0x602e, addresses[i]) == 0 &&
            sensor_read16(0x6f12, &value) == 0)
            printf("SENSOR_CODE %04x=%04x\n", addresses[i], value);
        else
            printf("SENSOR_CODE %04x=ERR errno=%d\n", addresses[i], errno);
    }
    fflush(stdout);
}

static void dump_sensor_stream_registers(void)
{
    static const uint16_t regs[] = {
        0x0100, 0x0104, 0x0112, 0x0114, 0x0136,
        0x021e, 0x0300, 0x0304, 0x0306, 0x030c, 0x030e, 0x0310, 0x0312,
        0x0340, 0x0342, 0x0344, 0x0346, 0x0348, 0x034a, 0x034c, 0x034e,
        0x0380, 0x0382, 0x0384, 0x0386, 0x0400, 0x0404,
        0x0900, 0x0901, 0x0a70, 0x0a72,
        0x0850, 0x0b02, 0x0b04, 0x0b06, 0x0116, 0x0118,
        0x0812, 0x0814, 0x080e, 0x0810, 0x0816,
        0x6214, 0x6244, 0x6b5e, 0x6b60, 0x0a7a, 0x0a7c, 0x0a76,
        0x0202, 0x0204, 0x020e
    };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint16_t value = 0;
        if (sensor_read16(regs[i], &value) == 0)
            printf("SENSOR_REG %04x=%04x\n", regs[i], value);
        else
            printf("SENSOR_REG %04x=ERR\n", regs[i]);
    }
    fflush(stdout);
}

static int pmic_read(uint8_t reg, uint8_t *value)
{
    struct i2c_msg msg[] = {
        {.addr = 0x59, .len = 1, .buf = &reg},
        {.addr = 0x59, .flags = I2C_M_RD, .len = 1, .buf = value}
    };
    struct i2c_rdwr_ioctl_data req = {.msgs = msg, .nmsgs = 2};
    return ioctl(pmic_fd, I2C_RDWR, &req);
}

static void dump_camera_pmic(void)
{
    static const uint8_t regs[] = {0x0c, 0x0d, 0x0e,
        0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
        0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f};
    for (unsigned i = 0; i < sizeof(regs); i++) {
        uint8_t value = 0;
        if (pmic_fd >= 0 && pmic_read(regs[i], &value) == 2)
            printf("CAMERA_PMIC %02x=%02x\n", regs[i], value);
        else
            printf("CAMERA_PMIC %02x=ERR\n", regs[i]);
    }
    fflush(stdout);
}

static int pmic_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    struct i2c_msg msg = {.addr = 0x59, .len = sizeof(data), .buf = data};
    struct i2c_rdwr_ioctl_data req = {.msgs = &msg, .nmsgs = 1};
    return ioctl(pmic_fd, I2C_RDWR, &req);
}

#ifdef CAMERA_XNU_POWER_REPLAY
#include "camera-power-replay.h"
#endif
#ifdef CAMERA_XNU_COLD_POWER
/* After Samsung's S_INPUT: bring the sensor and its MCLK to XNU's idle state, then run XNU's exact power-up
 * (redwood_camera_s20.c cam_s20_front_stream_probe, reference_mclk/reference_drive branch, mode 12/14). */
static bool cold_pmic(uint8_t reg, uint8_t value)
{
    uint8_t actual = 0;
    if (pmic_write(reg, value) < 0) return false;
    for (unsigned i = 0; i < 10; i++) {
        if (pmic_read(reg, &actual) == 2 && actual == value) return true;
        usleep(1000);
    }
    printf("XNU_COLD_POWER pmic %02x wanted %02x got %02x\n", reg, value, actual);
    return false;
}
static bool xnu_cold_power(void)
{
    /* XNU order: BUCK2, LDO16, LDO11, LDO6, LDO5 */
    static const uint8_t regs[] = {0x0d, 0x2d, 0x28, 0x23, 0x22};
    const uint32_t rst = 0x15c30160, mclk = 0x107300a0, top = 0x1a330000;
    uint8_t on[5] = {0}, count = 0;
    bool ok = false;
    receiver_failed = false;
    receiver_kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (receiver_kmsg < 0) return false;
    for (unsigned i = 0; i < 5; i++) if (pmic_read(regs[i], &on[i]) != 2) goto out;
    printf("XNU_COLD_POWER samsung rails %02x %02x %02x %02x %02x reset_con=%#x dat=%#x pud=%#x mclk_con=%#x dat=%#x pud=%#x drv=%#x\n",
           on[0], on[1], on[2], on[3], on[4], receiver_rd(rst, 0), receiver_rd(rst, 4), receiver_rd(rst, 8),
           receiver_rd(mclk, 0), receiver_rd(mclk, 4), receiver_rd(mclk, 8), receiver_rd(mclk, 12));
    printf("XNU_COLD_POWER samsung cis_clk1 mux=%#x div=%#x gate=%#x qch=%#x\n", receiver_rd(top, 0x101c),
           receiver_rd(top, 0x1818), receiver_rd(top, 0x2020), receiver_rd(top, 0x3008));
    (void)replay_write8(0x0100, 0);
    /* ---- to XNU's idle state (its cleanup order) ---- */
    receiver_update(mclk, 4, 2, 0); receiver_update(mclk, 0, 0xf0, 0x10);       /* MCLK pin output low */
    receiver_update(top, 0x3008, 7, 2);                                          /* XNU idle QCH = 2 */
    receiver_update(rst, 4, 1, 0);                                               /* reset low */
    for (unsigned i = 5; i > 0; i--)                                             /* LDO5, LDO6, LDO11, LDO16, BUCK2 off */
        if (!cold_pmic(regs[i-1], on[i-1] & (i == 1 ? 0x3f : 0x7f))) goto out;
    receiver_update(rst, 0, 0xf, 0); receiver_update(rst, 8, 0xf, 1);            /* idle: input, pull-down */
    receiver_update(mclk, 0, 0xf0, 0); receiver_update(mclk, 8, 0xf0, 0x10);     /* idle: input, pull-down */
    receiver_update(mclk, 12, 0xf0, 0x20);                                       /* idle drive 2 */
    if (receiver_failed) goto out;
    usleep(200000);
    puts("XNU_COLD_POWER sensor off, XNU idle state reached"); fflush(stdout);
    /* ---- XNU's power-up ---- */
    receiver_update(rst, 4, 1, 0); receiver_update(rst, 8, 0xf, 0); receiver_update(rst, 0, 0xf, 1);
    for (unsigned i = 0; i < 5; i++) if (!cold_pmic(regs[i], on[i])) goto out;   /* back-to-back, as XNU */
    usleep(10000);
    receiver_update(rst, 4, 1, 1);                                               /* reset high */
    receiver_update(top, 0x101c, 1, 0);
    receiver_update(top, 0x1818, 0x1f, 0);
    receiver_update(top, 0x3008, 7, 6);
    receiver_update(top, 0x2020, (1U << 20) | (1U << 21), 1U << 21);
    receiver_update(mclk, 12, 0xf0, 0);                                          /* Samsung/XNU reference drive */
    receiver_update(mclk, 4, 2, 0); receiver_update(mclk, 0, 0xf0, 0x20); receiver_update(mclk, 8, 0xf0, 0);
    usleep(9000);
    if (receiver_failed) goto out;
    if (sensor_read8(0x0005, &count) < 0) { puts("XNU_COLD_POWER sensor does not answer after power-up"); goto out; }
    printf("XNU_COLD_POWER sensor re-powered the XNU way, counter=%#x\n", count); fflush(stdout);
    ok = true;
out:
    if (!ok) printf("XNU_COLD_POWER failed receiver_failed=%d errno=%d\n", receiver_failed, errno);
    close(receiver_kmsg); receiver_kmsg = -1;
    fflush(stdout);
    return ok;
}
#endif

static void restore_buck2(void)
{
    uint8_t current;
    if (pmic_fd < 0) return;
    if (buck2_changed && pmic_read(0x0d, &current) >= 0)
        (void)pmic_write(0x0d, (current & ~0xc0U) | (buck2_before & 0xc0U));
    close(pmic_fd);
}

static void checkpoint(const char *stage)
{
    char page[4096] = {0};
    off_t offset = CAMERA_CHECKPOINT_OFFSET + (off_t)(checkpoint_number++ % 32) * sizeof(page);
    int n = snprintf(page, sizeof(page), "camera-ref vision checkpoint %u: %s\n",
                     checkpoint_number, stage);
    if (n < 0 || n >= (int)sizeof(page) ||
        pwrite(log_fd, page, sizeof(page), offset) != sizeof(page) || fsync(log_fd) != 0) {
        fprintf(stderr, "checkpoint %s failed: %s\n", stage, strerror(errno));
        exit(90);
    }
    puts(page); fflush(stdout);
}

static void print_csi_interrupts(int second)
{
    FILE *interrupts = fopen("/proc/interrupts", "r");
    char line[512];
    if (!interrupts) {
        printf("second=%d CSI interrupts unavailable: %s\n", second, strerror(errno));
        return;
    }
    while (fgets(line, sizeof(line), interrupts)) {
        if (strstr(line, " CSI\n") || strstr(line, " CSI ")) {
            printf("second=%d CSI interrupts: %s", second, line);
            break;
        }
    }
    fclose(interrupts);
}

int main(int argc, char **argv)
{
    const char *path = "/dev/video102"; /* EXYNOS_VIDEONODE_FIMC_IS=100 + SS1 video ID=2 */
    struct v4l2_capability cap = {0};
    struct v4l2_format fmt = {0};
    struct v4l2_control stream = {.id = V4L2_CID_IS_S_STREAM, .value = 1};
    struct v4l2_control state = {.id = V4L2_CID_IS_G_STREAM};
    struct v4l2_control mipi_error = {.id = V4L2_CID_IS_G_MIPI_ERR};
    struct v4l2_control size = {.id = V4L2_CID_IS_S_SENSOR_SIZE,
                                .value = (1824 << 16) | 1368};
    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    struct v4l2_requestbuffers request = {
        .count = 2, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_DMABUF
    };
    enum v4l2_buf_type queue_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    int ionfd = -1, buffer_fds[2][2] = {{-1, -1}, {-1, -1}};
    const bool queue = true; /* stand-alone Vision with receiver DMA buffers */
    uint8_t buck2_after = 0;
    struct stat log_stat;
    unsigned long long log_size = 0;
    int fd;
    unsigned input = (1U << 26) | (1U << 16); /* VISION stand-alone + front position */
    if (argc != 4 || (log_fd = open(argv[1], O_WRONLY | O_CLOEXEC)) < 0 ||
        fstat(log_fd, &log_stat) < 0 || !S_ISBLK(log_stat.st_mode) ||
        ioctl(log_fd, BLKGETSIZE64, &log_size) < 0 ||
        log_size < CAMERA_CHECKPOINT_OFFSET + 32 * 4096ULL) {
        fprintf(stderr, "validated super block device required for checkpoints\n");
        return 91;
    }
    checkpoint("helper start, before BUCK2 enable");
    pmic_fd = open(argv[2], O_RDWR | O_CLOEXEC);
    if (pmic_fd < 0 || atexit(restore_buck2) != 0 ||
        pmic_read(0x0d, &buck2_before) < 0) {
        fprintf(stderr, "BUCK2 snapshot failed: %s\n", strerror(errno));
        return 92;
    }
    buck2_changed = true;
    if (pmic_write(0x0d, buck2_before | 0xc0U) < 0 ||
        pmic_read(0x0d, &buck2_after) < 0 ||
        (buck2_after & 0xc0U) != 0xc0U) {
        fprintf(stderr, "BUCK2 enable/readback failed: %s\n", strerror(errno));
        return 92;
    }
    checkpoint("BUCK2 enabled and read back, before camera open");
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 1; }
    checkpoint("camera open returned, before QUERYCAP");
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "QUERYCAP: %s\n", strerror(errno)); close(fd); return 2;
    }
    checkpoint("QUERYCAP returned, before S_INPUT");
    printf("node=%s driver=%s card=%s caps=%#x\n", path, cap.driver, cap.card, cap.capabilities);
    fflush(stdout);
    if (ioctl(fd, VIDIOC_S_INPUT, &input) < 0) {
        fprintf(stderr, "S_INPUT front=%#x: %s\n", input, strerror(errno)); close(fd); return 3;
    }
    checkpoint("S_INPUT returned, before SENSOR_SIZE");
    sensor_fd = open(argv[3], O_RDWR | O_CLOEXEC);
    printf("SAK3J1 I2C device=%s open=%s\n", argv[3], sensor_fd >= 0 ? "yes" : "no");
    puts("S_INPUT front accepted"); fflush(stdout);
#ifdef CAMERA_XNU_COLD
    /* Split test (session B, 2026-11-11): Samsung only opens the front camera (S_INPUT: power domain + DTZPC, clocks,
     * DVFS, PHY power, sensor rails/reset/MCLK). Its receiver is never configured and its stream never started.
     * XNU's receiver sequence and XNU's sensor programming + stream then run on top. */
    checkpoint("COLD: after S_INPUT, before XNU receiver");
    puts("XNU_COLD begin: Samsung S_INPUT only, no Samsung stream"); fflush(stdout);
    dump_camera_clocks();
    for (unsigned r = 0; r <= 0x2f; r++) {   /* full S2MPB02 map after Samsung's camera open */
        uint8_t value = 0;
        if (pmic_read((uint8_t)r, &value) == 2) printf("CAMERA_PMIC_MAP %02x=%02x\n", r, value);
        else printf("CAMERA_PMIC_MAP %02x=ERR\n", r);
    }
    fflush(stdout);
#ifdef CAMERA_FULL_REGDUMP
    dump_camera_full("after-sinput");
#endif
#ifdef CAMERA_XNU_COLD_POWER
    checkpoint("COLD: before XNU-style sensor power cycle");
    if (!xnu_cold_power()) puts("XNU_COLD_POWER did not complete; continuing with whatever state resulted");
    checkpoint("COLD: XNU-style sensor power cycle returned");
#endif
    if (replay_xnu_receiver(true)) {
        checkpoint("COLD: XNU receiver configured, before XNU sensor stream");
        replay_xnu_sensor();
        checkpoint("COLD: XNU sensor stream returned");
    } else {
        puts("XNU_COLD receiver setup refused (see XNU_RECEIVER_REPLAY line)");
    }
#ifdef CAMERA_XNU_COLD_BASEOFF
    {
        /* Stage B: Samsung's always-on S2MPB02 supplies (BUCK1 0x0b, buck-boost 0x0f, LDO18 0x2f) switched to XNU's
         * idle state (off), then the same XNU power-up + receiver + sensor. Reproduces XNU's failure? */
        uint8_t b1 = 0, bb = 0, l18 = 0;
        if (pmic_read(0x0b, &b1) == 2 && pmic_read(0x0f, &bb) == 2 && pmic_read(0x2f, &l18) == 2) {
            printf("XNU_COLD stage B: base supplies were 0b=%02x 0f=%02x 2f=%02x, switching off\n", b1, bb, l18); fflush(stdout);
            (void)replay_write8(0x0100, 0);
            (void)pmic_write(0x2f, l18 & 0x7f); (void)pmic_write(0x0f, bb & 0x3f); (void)pmic_write(0x0b, b1 & 0x3f);
            usleep(100000);
            checkpoint("COLD-B: base supplies off, before XNU power cycle");
            if (!xnu_cold_power()) puts("XNU_COLD stage B: power cycle did not complete");
            if (replay_xnu_receiver(true)) replay_xnu_sensor();
            else puts("XNU_COLD stage B: receiver setup refused");
            (void)replay_write8(0x0100, 0);
            (void)pmic_write(0x0b, b1); usleep(2000); (void)pmic_write(0x0f, bb); usleep(2000); (void)pmic_write(0x2f, l18);
            usleep(20000);
            printf("XNU_COLD stage B end: base supplies restored\n"); fflush(stdout);
            checkpoint("COLD-B: returned, supplies restored");
        } else puts("XNU_COLD stage B: could not read base supplies");
    }
#endif
    puts("XNU_COLD end"); fflush(stdout);
    close(fd);
    if (sensor_fd >= 0) close(sensor_fd);
    close(log_fd);
    return 0;
#endif
    if (ioctl(fd, VIDIOC_S_CTRL, &size) < 0) {
        fprintf(stderr, "S_CTRL sensor size: %s\n", strerror(errno)); close(fd); return 4;
    }
    checkpoint("SENSOR_SIZE returned, before S_PARM");
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
        fprintf(stderr, "S_PARM 30 fps: %s\n", strerror(errno)); close(fd); return 5;
    }
    checkpoint("S_PARM returned, before S_FMT");
    puts("sensor size and 30 fps accepted"); fflush(stdout);
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = 1824;
    fmt.fmt.pix_mp.height = 1368;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SBGGR10;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "S_FMT 1824x1368 RAW10: %s\n", strerror(errno)); close(fd); return 6;
    }
    checkpoint("S_FMT returned, before STREAM_ON");
    printf("S_FMT accepted %ux%u format=%#x\n", fmt.fmt.pix_mp.width,
           fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.pixelformat);
    fflush(stdout);
    if (queue) {
    checkpoint("before REQBUFS");
    if (ioctl(fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
        fprintf(stderr, "REQBUFS: %s count=%u\n", strerror(errno), request.count);
        close(fd); return 8;
    }
    checkpoint("REQBUFS returned, before ION allocation");
    ionfd = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (ionfd < 0) { fprintf(stderr, "open /dev/ion: %s\n", strerror(errno)); close(fd); return 9; }
    for (unsigned i = 0; i < 2; i++) {
        struct ion_allocation_data image = {.len = CAMERA_IMAGE_BYTES, .heap_id_mask = 0xffffffffU};
        struct ion_allocation_data meta = {.len = CAMERA_META_BYTES, .heap_id_mask = 0xffffffffU};
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}};
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_DMABUF,
            .index = i,
            .length = 2,
            .m.planes = planes
        };
        if (ioctl(ionfd, ION_IOC_ALLOC, &image) < 0 ||
            ioctl(ionfd, ION_IOC_ALLOC, &meta) < 0) {
            fprintf(stderr, "ION alloc %u: %s\n", i, strerror(errno));
            close(fd); close(ionfd); return 11;
        }
        buffer_fds[i][0] = (int)image.fd;
        buffer_fds[i][1] = (int)meta.fd;
        planes[0].m.fd = buffer_fds[i][0]; planes[0].length = CAMERA_IMAGE_BYTES;
        planes[1].m.fd = buffer_fds[i][1]; planes[1].length = CAMERA_META_BYTES;
        checkpoint(i == 0 ? "first ION pair allocated, before QBUF" : "second ION pair allocated, before QBUF");
        if (ioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
            fprintf(stderr, "QBUF %u: %s\n", i, strerror(errno));
            close(fd); close(ionfd); return 12;
        }
        checkpoint(i == 0 ? "first DMA buffer queued" : "second DMA buffer queued");
    }
    checkpoint("before queue STREAMON");
    if (ioctl(fd, VIDIOC_STREAMON, &queue_type) < 0) {
        fprintf(stderr, "queue STREAMON: %s\n", strerror(errno));
        close(fd); return 10;
    }
    checkpoint("queue STREAMON returned, before sensor STREAM_ON");
    }
    if (ioctl(fd, VIDIOC_S_CTRL, &stream) < 0) {
        fprintf(stderr, "S_CTRL IS_S_STREAM=1: %s\n", strerror(errno));
        close(fd); return 7;
    }
    checkpoint("STREAM_ON returned");
    puts("S_CTRL stream started"); fflush(stdout);
    dump_camera_clocks();
#ifdef CAMERA_FULL_REGDUMP
    dump_camera_full("stream");
#endif
    dump_camera_pmic();
    dump_sensor_stream_registers();
    dump_sensor_indirect();
    print_csi_interrupts(0);
    for (int i = 0; i < 5; i++) {
        uint8_t sensor_count = 0;
        sleep(1);
        if (ioctl(fd, VIDIOC_G_CTRL, &state) == 0)
            printf("second=%d G_STREAM=%d\n", i + 1, state.value);
        else
            fprintf(stderr, "G_STREAM: %s\n", strerror(errno));
        if (ioctl(fd, VIDIOC_G_CTRL, &mipi_error) == 0)
            printf("second=%d G_MIPI_ERR=%d\n", i + 1, mipi_error.value);
        else
            fprintf(stderr, "G_MIPI_ERR: %s\n", strerror(errno));
        print_csi_interrupts(i + 1);
        if (sensor_read8(0x0005, &sensor_count) >= 0)
            printf("second=%d SAK3J1_FRAME_COUNT=%u\n", i + 1, sensor_count);
        else
            printf("second=%d SAK3J1_FRAME_COUNT=unavailable\n", i + 1);
        fflush(stdout);
    }
#ifdef CAMERA_XNU_SENSOR_REPLAY
    checkpoint("before XNU sensor replay on Samsung receiver");
    replay_xnu_sensor();
    checkpoint("XNU sensor replay returned");
#ifdef CAMERA_XNU_RECEIVER_REPLAY
    if (replay_xnu_receiver(false)) {
        replay_xnu_sensor();
        checkpoint("XNU PHY replay and sensor returned");
        if (replay_xnu_receiver(true)) {
            replay_xnu_sensor();
            checkpoint("XNU CSI and PHY replay and sensor returned");
#ifdef CAMERA_XNU_POWER_REPLAY
            if (replay_sensor_power(false)) {
                (void)replay_sensor_power(true);
                checkpoint("Sensor reset and rail replay returned");
            }
#endif
#ifdef CAMERA_XNU_GPIO_REPLAY
            replay_gpio_sensor();
            checkpoint("GPIO sensor replay returned");
#endif
        }
    }
#endif
#endif
    stream.value = 0;
    checkpoint("before STREAM_OFF");
    if (ioctl(fd, VIDIOC_S_CTRL, &stream) < 0)
        fprintf(stderr, "S_CTRL IS_S_STREAM=0: %s\n", strerror(errno));
    if (queue && ioctl(fd, VIDIOC_STREAMOFF, &queue_type) < 0)
        fprintf(stderr, "queue STREAMOFF: %s\n", strerror(errno));
    close(fd);
    for (unsigned i = 0; i < 2; i++)
        for (unsigned j = 0; j < 2; j++)
            if (buffer_fds[i][j] >= 0) close(buffer_fds[i][j]);
    if (ionfd >= 0) close(ionfd);
    if (sensor_fd >= 0) close(sensor_fd);
    checkpoint("STREAM_OFF returned, camera closed");
    close(log_fd);
    return 0;
}

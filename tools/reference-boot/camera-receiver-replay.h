/* Opt-in isolation test, only in the temporary Samsung reference boot.
 * The sensor must be stopped. Samsung retains power/clocks and V4L2 ownership.
 * Unlike dump_camera_clocks(), this uses SFR's WRITE operation, restricted to
 * the exact CSI1/PHY configuration registers used by the XNU bounded probe.
 * No clock, PMIC, DMA, arbitrary address, or SYSREG +0x504 write is accepted.
 */
static bool receiver_failed;
static int receiver_kmsg = -1;

static bool receiver_allowed(uint32_t address)
{
#ifdef CAMERA_XNU_POWER_REPLAY
    if (address == 0x15c30160 || address == 0x15c30164 ||
        address == 0x107300a0 || address == 0x107300a4) return true;
#endif
#ifdef CAMERA_XNU_COLD_POWER
    /* XNU-style sensor power/MCLK start: reset pin GPM11, MCLK pin GPC0-1 (CON/DAT/PUD/DRV), CMU_TOP CIS_CLK1. */
    if (address == 0x15c30160 || address == 0x15c30164 || address == 0x15c30168 ||
        address == 0x107300a0 || address == 0x107300a4 || address == 0x107300a8 || address == 0x107300ac ||
        address == 0x1a33101c || address == 0x1a331818 || address == 0x1a332020 || address == 0x1a333008) return true;
#endif
#ifdef CAMERA_XNU_GPIO_REPLAY
    /* Only the two sensor-I2C pin mux fields are changed by the GPIO replay. */
    if (address == 0x10430000) return true;
#endif
    static const uint32_t csi[] = {4,8,12,16,24,0x40,0x44,0x50,0x54,0x600,0x690,0x704};
    static const uint32_t sc[] = {0,4,8,0x10,0x14,0x18,0x30};
    static const uint32_t sd[] = {0,4,0x10,0x14,0x18,0x30,0x34,0x40,0x50};
    if (address == 0x17020500 || address == 0x170d1000 ||
        address == 0x170d1004 || address == 0x170d1008) return true;
    for (unsigned i = 0; i < sizeof(csi)/sizeof(csi[0]); i++)
        if (address == 0x17040000 + csi[i]) return true;
    for (unsigned i = 0; i < sizeof(sc)/sizeof(sc[0]); i++)
        if (address == 0x170d2b00 + sc[i]) return true;
    for (unsigned lane = 1; lane <= 4; lane++)
        for (unsigned i = 0; i < sizeof(sd)/sizeof(sd[0]); i++)
            if (address == 0x170d2b00 + lane*0x100 + sd[i]) return true;
    return false;
}

static bool receiver_command(uint32_t address, bool write_value, uint32_t value)
{
    char command[64];
    if (!receiver_allowed(address) &&
        (write_value || (address != 0x17040500 && address != 0x17040700))) return false;
    int n = write_value ? snprintf(command, sizeof(command), "SFR 0x%08x 0x%08x\n", address, value) :
                         snprintf(command, sizeof(command), "SFR 0x%08x\n", address);
    int fd = open("/sys/module/sec_debug/parameters/force_error", O_WRONLY);
    if (fd < 0) return false;
    ssize_t sent = write(fd, command, n);
    close(fd);
    return sent == n;
}

static uint32_t receiver_rd(uint32_t base, uint32_t offset)
{
    char line[8192];
    uint32_t address = base + offset, value = 0, seen = 0;
    bool matched = false;
    if (receiver_failed) return 0;
    /* Start after existing records so stale SFR results cannot satisfy a read. */
    if (lseek(receiver_kmsg, 0, SEEK_END) < 0 || !receiver_command(address, false, 0)) goto failed;
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        ssize_t n = read(receiver_kmsg, line, sizeof(line)-1);
        if (n < 0) {
            if (errno == EAGAIN) { usleep(100); continue; }
            goto failed;
        }
        line[n] = 0;
        char *p = strstr(line, "simulate_SFR() 1st parameter:");
        if (p && sscanf(p, "simulate_SFR() 1st parameter: %x", &seen) == 1)
            matched = seen == address;
        p = strstr(line, "simulate_SFR() result :");
        if (matched && p && sscanf(p, "simulate_SFR() result : %x", &value) == 1) return value;
    }
failed:
    printf("XNU_RECEIVER_REPLAY read failed address=%08x errno=%d\n", address, errno);
    receiver_failed = true;
    return 0;
}

static void receiver_wr(uint32_t base, uint32_t offset, uint32_t value)
{
    if (receiver_failed) return;
    if (!receiver_command(base+offset, true, value)) receiver_failed = true;
}

static void receiver_update(uint32_t base, uint32_t offset, uint32_t mask, uint32_t value)
{
    uint32_t old = receiver_rd(base, offset);
    receiver_wr(base, offset, (old & ~mask) | (value & mask));
}

/* PHY recipe copied from redwood_camera_s20.c; generated below. */
static void
receiver_phy_recipe(uint32_t dphy, uint32_t bias)
{
    static const uint32_t lane_base[] = { 0x0100, 0x0200, 0x0300, 0x0400 };
    unsigned i;

    receiver_wr(dphy, 0x0000, 0);
    for (i = 0; i < 4; i++) receiver_wr(dphy, lane_base[i], 0);
    usleep(200);
    receiver_wr(bias, 0x0000, 0x10);
    receiver_wr(bias, 0x0004, 0x110);
    receiver_wr(bias, 0x0008, 0x3223);

    /* Exact __set_phy_cfg_0503_0000_dcphy() recipe, mode D/type D (CSI B/D/F). */
    receiver_wr(dphy, 0x0004, 0x1450);
    receiver_wr(dphy, 0x0008, 0x8);
    /* Keep the write order as well as the final values: skew is updated
     * by the data-lane loop after the clock lane has been enabled. */
    receiver_wr(dphy, 0x0010, 0x2);
    receiver_wr(dphy, 0x0014, 0x8680);
    receiver_wr(dphy, 0x0018, 0x4000);
    receiver_wr(dphy, 0x0030, 0x301);
    receiver_wr(dphy, 0x0000, 1); /* clock enable last */

    for (i = 0; i < 4; i++) {
        uint32_t b = lane_base[i];
        receiver_wr(dphy, b + 0x0004, 0x1450);
        receiver_wr(dphy, b + 0x0010, 0x2);
        /* Samsung updates the preceding lane (the clock on iteration zero). */
        receiver_update(dphy, i * 0x100 + 0x0010, 3U << 8, 3U << 8);
        receiver_wr(dphy, b + 0x0014, 0x8680);
        receiver_wr(dphy, b + 0x0018, 0x4000);
        receiver_update(dphy, b + 0x0030, 0xffU, 9);
		receiver_update(dphy, b + 0x0030, 1U << 8, 0); /* settle_clk_sel=0 at 1807 Mbps */
        receiver_wr(dphy, b + 0x0034, 3);
		receiver_update(dphy, b + 0x0040, 1U, 1U); /* skew_cal_en=1 at >=1500 Mbps */
        receiver_wr(dphy, b + 0x0050, 0x81a);
        receiver_wr(dphy, b, 1); /* data enable last */
    }
	/* __set_phy_cfg_0503_0000_dcphy() settles for 200 us after lane enable. */
	usleep(200);
}


static bool replay_xnu_receiver(bool core)
{
    const uint32_t csi = 0x17040000, phy = 0x170d2b00, reset = 0x17020500;
    uint8_t count = 0;
    uint32_t saved_reset;
    receiver_failed = false;
    receiver_kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (receiver_kmsg < 0) return false;
    /* Previous replay stops the sensor. Require an observed standby counter. */
    for (unsigned i = 0; i < 100; i++) {
        if (sensor_read8(0x0005, &count) < 0) goto failed;
        if (count == 0xff) break;
        usleep(1000);
    }
    if (count != 0xff) goto failed;
    printf("XNU_RECEIVER_REPLAY begin stage=%s sensor_standby=ff\n", core ? "core-and-phy" : "phy-only");
    fflush(stdout);
    saved_reset = receiver_rd(reset, 0);
#ifdef CAMERA_XNU_COLD
    /* After S_INPUT only, Samsung may not have released the PHY reset yet: XNU ends with bit 3 released. */
    printf("XNU_COLD reset_sysreg_before=%#x phy_status_before=%#x\n", saved_reset, receiver_rd(csi, 0x700));
    if (receiver_failed) goto failed;
    saved_reset |= 8U;
#else
    if (receiver_failed || saved_reset != 8) goto failed;
#endif
    if (core) {
        uint32_t cmn = receiver_rd(csi, 4);
        receiver_wr(csi, 4, cmn & ~1U);
        receiver_wr(csi, 4, (cmn & ~1U) | 2U);
        unsigned retry;
        for (retry = 0; retry < 10; retry++) {
            if (!(receiver_rd(csi, 4) & 2U)) break;
            usleep(10);
        }
        if (retry == 10 || receiver_failed) goto failed;
    }
    receiver_wr(reset, 0, saved_reset & ~8U);
    receiver_phy_recipe(phy, 0x170d1000);
    /* Always release the previously verified reset even on a recipe error. */
    if (!receiver_command(reset, true, saved_reset)) receiver_failed = true;
    if (receiver_failed) goto failed;
    if (core) {
        receiver_wr(csi, 4, 0x4b00);
        receiver_update(csi, 8, 0x10, 0x10);
        receiver_update(csi, 0x704, 0x1f, 0x1e);
        receiver_wr(csi, 0x40, 0x20ac);
        receiver_wr(csi, 0x44, (1368U << 16) | 1824U);
        receiver_wr(csi, 0x50, 0x120ac);
        receiver_wr(csi, 0x54, (342U << 16) | 1824U);
        receiver_update(csi, 0x600, 1U << 31, 0);
        receiver_update(csi, 0x690, 1U << 23, 1U << 23);
        receiver_wr(csi, 12, 15);
        receiver_update(csi, 0x704, 1, 1);
        receiver_wr(csi, 0x10, 0x00fff07f);
        receiver_wr(csi, 0x18, 0x7f);
        receiver_update(csi, 4, 1, 1);
        usleep(2000);
    }
    /* Readback gates before the sensor is allowed to restart. */
    if ((receiver_rd(phy, 0) & 1) != 1 || receiver_rd(reset, 0) != saved_reset ||
        receiver_rd(csi, 4) != 0x4b01 || receiver_rd(csi, 0x704) != 0x1f) goto failed;
    for (unsigned i = 1; i <= 4; i++)
        if (!(receiver_rd(phy, i*0x100) & 1)) goto failed;
    if (receiver_failed) goto failed;
    printf("XNU_RECEIVER_REPLAY configured stage=%s frame_count=%u\n", core ? "core-and-phy" : "phy-only", receiver_rd(csi, 0x500));
    close(receiver_kmsg);
    receiver_kmsg = -1;
    fflush(stdout);
    return !receiver_failed;
failed:
    printf("XNU_RECEIVER_REPLAY failed stage=%s errno=%d\n", core ? "core-and-phy" : "phy-only", errno);
    fflush(stdout);
    close(receiver_kmsg);
    receiver_kmsg = -1;
    return false;
}

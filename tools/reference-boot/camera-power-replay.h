/* Cold-start isolation with Samsung's receiver kept alive. Only the front
 * sensor's established rails, GPM11-0 reset and GPC0-1 MCLK pin are changed.
 * MCLK is held low and reset asserted before switching sensor supplies.
 */
static bool power_replay_pmic(uint8_t reg, uint8_t value)
{
    uint8_t actual = 0;
    int sent = pmic_write(reg, value), send_errno = errno;
    int got = pmic_read(reg, &actual), read_errno = errno;
    /* Allow a bounded settling window without rewriting the regulator or
     * relaxing the exact-value check. Preserve the first mismatch in the log. */
    unsigned retries = 0;
    if (sent == 1 && got == 2 && actual != value) {
        printf("SENSOR_POWER_PMIC initial reg=%02x wanted=%02x actual=%02x\n",
               reg, value, actual);
        fflush(stdout);
        while (got == 2 && actual != value && retries < 10) {
            usleep(1000);
            retries++;
            got = pmic_read(reg, &actual);
            read_errno = errno;
        }
    }
    bool ok = sent == 1 && got == 2 && actual == value;
    printf("SENSOR_POWER_PMIC reg=%02x wanted=%02x actual=%02x sent=%d got=%d write_errno=%d read_errno=%d retries=%u ok=%u\n",
           reg, value, actual, sent, got, sent < 0 ? send_errno : 0, got < 0 ? read_errno : 0, retries, ok);
    fflush(stdout);
    return ok;
}

static bool replay_sensor_power(bool rails)
{
    static const uint8_t regs[] = {0x0d, 0x2d, 0x28, 0x23, 0x22};
    static const uint8_t expected[] = {0xd8, 0xd8, 0xde, 0xb0, 0xa4};
    uint8_t saved[5] = {0}, count = 0;
    const uint32_t reset = 0x15c30160, mclk = 0x107300a0;
    uint32_t rcon = 0, rdat = 0, mcon = 0, mdat = 0;
    bool pins_saved = false, rails_changed = false, ok = false, restored = true;
    for (unsigned i = 0; i < 100; i++) {
        if (sensor_read8(5, &count) < 0) return false;
        if (count == 0xff) break;
        usleep(1000);
    }
    if (count != 0xff) return false;
    receiver_failed = false;
    receiver_kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (receiver_kmsg < 0) return false;
    for (unsigned i = 0; i < 5; i++)
        if (pmic_read(regs[i], &saved[i]) != 2 || saved[i] != expected[i]) goto cleanup;
    rcon = receiver_rd(reset, 0); rdat = receiver_rd(reset, 4);
    mcon = receiver_rd(mclk, 0); mdat = receiver_rd(mclk, 4);
    if (receiver_failed || (rcon & 15) != 1 || !(rdat & 1) || ((mcon >> 4) & 15) != 2) goto cleanup;
    pins_saved = true;
    printf("SENSOR_POWER_REPLAY begin stage=%s\n", rails ? "cold-rails" : "reset-only"); fflush(stdout);
    receiver_update(reset, 4, 1, 0);
    receiver_update(mclk, 4, 2, 0);
    receiver_update(mclk, 0, 0xf0, 0x10); /* output low */
    if (receiver_failed) goto cleanup;
    if (rails) {
        rails_changed = true;
        for (unsigned i = 5; i > 0; i--)
            if (!power_replay_pmic(regs[i-1], saved[i-1] & (i == 1 ? 0x3f : 0x7f))) goto cleanup;
    }
    usleep(10000);
    if (rails)
        for (unsigned i = 0; i < 5; i++)
            if (!power_replay_pmic(regs[i], saved[i])) goto cleanup;
    usleep(10000);
    receiver_update(reset, 4, 1, 1);
    receiver_update(mclk, 0, 0xf0, 0x20);
    usleep(9000);
    if (receiver_failed) goto cleanup;
    puts("SENSOR_POWER_REPLAY sensor re-powered; replaying setfiles"); fflush(stdout);
    replay_xnu_sensor();
    ok = true;
cleanup:
    /* Restore supplies in their normal enable order even after a partial error. */
    if (rails_changed)
        for (unsigned i = 0; i < 5; i++)
            if (!power_replay_pmic(regs[i], saved[i])) restored = false;
    if (pins_saved) {
        receiver_failed = false;
        receiver_update(reset, 4, 1, rdat);
        receiver_update(reset, 0, 15, rcon);
        receiver_update(mclk, 4, 2, mdat);
        receiver_update(mclk, 0, 0xf0, mcon);
        if (((receiver_rd(reset, 0) ^ rcon) & 15) ||
            ((receiver_rd(reset, 4) ^ rdat) & 1) ||
            ((receiver_rd(mclk, 0) ^ mcon) & 0xf0) || receiver_failed) restored = false;
    }
    close(receiver_kmsg); receiver_kmsg = -1;
    printf("SENSOR_POWER_REPLAY end stage=%s setup=%s restored=%s\n",
           rails ? "cold-rails" : "reset-only", ok ? "ok" : "failed", restored ? "true" : "false");
    fflush(stdout);
    return ok && restored;
}

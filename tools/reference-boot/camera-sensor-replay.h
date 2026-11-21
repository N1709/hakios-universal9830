/* Optional isolation experiment: XNU sensor writes with Samsung's receiver.
 * Included only by camera-ref.c when CAMERA_XNU_SENSOR_REPLAY is defined.
 * Uses the same setfile data as XNU, with hardware I2C instead of GPIO I2C.
 * The V4L2 owner keeps the rails, MCLK, receiver and bounded DMA buffers alive.
 */
#include "../../xnu/files/bsd/dev/redwood_camera_s20_3j1.h"

static int replay_send(uint8_t *data, unsigned length)
{
    return sensor_transfer(data, length, NULL, 0);
}

static int replay_write8(uint16_t reg, uint8_t value)
{
    uint8_t data[] = {reg >> 8, reg, value};
    return replay_send(data, sizeof(data));
}

static int replay_table(const uint32_t *table, unsigned count)
{
    uint8_t burst[402] = {0};
    unsigned words = 0;
    bool pending = false;
    usleep(3000);
    for (unsigned i = 0; i + 2 < count; i += 3) {
        uint32_t reg = table[i], value = table[i + 1];
        if (reg == 0xf0000002 && pending) {
            if (words == 200) return -1;
            burst[2 + 2 * words] = value >> 8;
            burst[3 + 2 * words++] = value;
            continue;
        }
        if (pending && replay_send(burst, 2 + 2 * words)) return -1;
        pending = false;
        if (reg == 0xf0000001) {
            burst[0] = value >> 8; burst[1] = value;
            words = 0; pending = true;
        } else if (reg == 0xf0000003) {
            usleep(value);
        } else if (reg > 0xffff || value > 0xffff || table[i + 2] != 2 ||
                   sensor_write16(reg, value)) return -1;
    }
    return pending ? replay_send(burst, 2 + 2 * words) : 0;
}

static void replay_xnu_sensor(void)
{
    uint8_t count = 0;
    const char *phase = "stop sensor";
    puts("XNU_SENSOR_REPLAY begin: receiver stays Samsung-owned");
    fflush(stdout);
    if (replay_write8(0x0100, 0)) goto failed;
    phase = "global table";
    if (replay_table(redwood_3j1_global, REDWOOD_3J1_GLOBAL_WORDS)) goto failed;
    phase = "mode table";
    if (replay_table(redwood_3j1_1824x1368_30fps_pd,
                     REDWOOD_3J1_1824X1368_30FPS_PD_WORDS)) goto failed;
    phase = "normal timing and gains";
    if (replay_write8(0x0118, 0) || sensor_write16(0xfcfc, 0x4000) ||
        replay_write8(0x0702, 0) || replay_write8(0x0704, 0) ||
        sensor_write16(0x0204, 0x0200) || sensor_write16(0x020e, 0x1000)) goto failed;
    phase = "MIPI clock";
    if (sensor_write16(0x0136, 0x1a00) || sensor_write16(0x0300, 7) ||
        sensor_write16(0x0304, 4) || sensor_write16(0x0306, 0xd9) ||
        sensor_write16(0x030c, 0) || sensor_write16(0x030e, 4) ||
        sensor_write16(0x0310, 0x116) || sensor_write16(0x0312, 1) ||
        replay_write8(0x0104, 0)) goto failed;
    usleep(8000);
    phase = "stream on";
    if (sensor_write16(0x0a70, 1) || sensor_write16(0x0a72, 0x0100) ||
        replay_write8(0x0100, 1) || replay_write8(0x021e, 0)) goto failed;
    usleep(500000);
    if (sensor_read8(0x0005, &count) < 0) goto failed;
    printf("XNU_SENSOR_REPLAY active: sensor_counter=%u\n", count);
    dump_camera_clocks();
    if (replay_write8(0x0100, 0)) goto failed;
    puts("XNU_SENSOR_REPLAY complete: sensor stopped");
    fflush(stdout);
    return;
failed:
    printf("XNU_SENSOR_REPLAY failed: phase=%s errno=%d\n", phase, errno);
    (void)replay_write8(0x0100, 0);
    fflush(stdout);
}

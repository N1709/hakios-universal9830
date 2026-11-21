/* MIT. Bounded one-attempt S5123 power/SPI lifecycle. No physical I/O here.
 * Board facts: pinned modem_ctrl_s5100.c + x1slte overlay (see docs).
 * GPIO callbacks must set direction/value glitch-free and verify readback.
 * Only AP-owned outputs are exposed: NEVER drive CP2AP_WAKE/PHONE_ACTIVE/PSHOLD.
 * Before handing back memory/booting a fallback, caller MUST require cleanup_safe.
 * Controller-complete BOOT transmission is not modem acceptance/registration.
 */
#ifndef S5123_BOOT_LIFECYCLE_H
#define S5123_BOOT_LIFECYCLE_H
#include <stdint.h>
#include <stddef.h>

enum s5123_ap_output { S5123_RESET_N, S5123_POWER_ON, S5123_AP_STATUS,
    S5123_AP_WAKE, S5123_DUMP_NOTI, S5123_OUTPUT_COUNT };
enum {
    S5123_BOOT_OK=0, S5123_BOOT_ARGUMENT=-1, S5123_BOOT_GUARD=-2,
    S5123_BOOT_ALREADY_ATTEMPTED=-3, S5123_BOOT_BAD_IMAGE=-4,
    S5123_BOOT_PREPARE=-5, S5123_BOOT_GPIO=-6, S5123_BOOT_WAIT=-7,
    S5123_BOOT_SPI=-8, S5123_BOOT_CLEANUP=-9
};
enum s5123_boot_phase { S5123_BOOT_IDLE, S5123_BOOT_PREPARING,
    S5123_BOOT_RESET_ASSERTED, S5123_BOOT_POWER_REQUESTED,
    S5123_BOOT_RESET_RELEASED, S5123_BOOT_SPI_ROUTED, S5123_BOOT_TX_COMPLETE };
struct s5123_boot_ops {
    void *context;
    int (*ready)(void *); /* Memory/cache, mappings, owner checks, PHY1 isolated. */
    int (*spi_prepare)(void *); /* Controller/clocks only; external pads stay inputs. */
    int (*output)(void *, enum s5123_ap_output, int level);
    int (*sleep_us)(void *, uint32_t);
    int (*spi_route)(void *); /* Transfer dump/shared pad ownership to SPI. */
    int (*spi_send)(void *, const uint8_t *, size_t, uint32_t *completed);
    int (*spi_stop)(void *); /* Park channels/packet generation/chip select. */
    int (*spi_restore)(void *); /* Partial-prepare-safe clock/USI/pad restore. */
    int (*quiescent)(void *); /* Reset/power outputs held low, PHY1 still isolated. */
};
struct s5123_boot_result {
    uint32_t attempted, power_requested, spi_completed, cleanup_safe;
    enum s5123_boot_phase phase;
    int error, cleanup_error;
};

static inline uint32_t
s5123_boot_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc=~0u;
    for (size_t i=0;i<length;i++) {
        crc^=data[i];
        for (unsigned bit=0;bit<8;bit++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1)));
    }
    return ~crc;
}

static inline int
s5123_boot_once(const struct s5123_boot_ops *ops, struct s5123_boot_result *result,
    const uint8_t *image, size_t length, uint32_t expected_crc)
{
    if (!ops || !result || !image || !length || length>131072 || !ops->ready ||
        !ops->spi_prepare || !ops->output || !ops->sleep_us || !ops->spi_route ||
        !ops->spi_send || !ops->spi_stop || !ops->spi_restore || !ops->quiescent) return S5123_BOOT_ARGUMENT;
    if (result->attempted) return S5123_BOOT_ALREADY_ATTEMPTED;
    if (s5123_boot_crc32(image,length)!=expected_crc) return S5123_BOOT_BAD_IMAGE;
    if (!ops->ready(ops->context)) return S5123_BOOT_GUARD;
    *result=(struct s5123_boot_result){.attempted=1,.phase=S5123_BOOT_PREPARING};
    int prepared=0,gpio_touched=0,error=S5123_BOOT_OK;
    /* Even failed preparation can change clocks; restore is always called. */
    if (ops->spi_prepare(ops->context)) {error=S5123_BOOT_PREPARE;goto cleanup;}
    prepared=1;
    gpio_touched=1;
    /* Assert reset before taking other controls. Initialize AP_WAKE low as in
     * Samsung's GPIO setup; AP_STATUS high and DUMP low precede the reset wait.
     */
    if (ops->output(ops->context,S5123_RESET_N,0) ||
        ops->output(ops->context,S5123_POWER_ON,0) ||
        ops->output(ops->context,S5123_AP_WAKE,0) ||
        ops->output(ops->context,S5123_AP_STATUS,1) ||
        ops->output(ops->context,S5123_DUMP_NOTI,0)) {error=S5123_BOOT_GPIO;goto cleanup;}
    result->phase=S5123_BOOT_RESET_ASSERTED;
    if (ops->sleep_us(ops->context,50000) || ops->sleep_us(ops->context,50)) {error=S5123_BOOT_WAIT;goto cleanup;}
    /* Mark BEFORE requesting power: a failed readback may still have powered CP. */
    result->power_requested=1;
    result->phase=S5123_BOOT_POWER_REQUESTED;
    if (ops->output(ops->context,S5123_POWER_ON,1)) {error=S5123_BOOT_GPIO;goto cleanup;}
    if (ops->sleep_us(ops->context,50000)) {error=S5123_BOOT_WAIT;goto cleanup;}
    if (ops->output(ops->context,S5123_RESET_N,1)) {error=S5123_BOOT_GPIO;goto cleanup;}
    result->phase=S5123_BOOT_RESET_RELEASED;
    if (ops->sleep_us(ops->context,50000)) {error=S5123_BOOT_WAIT;goto cleanup;}
    if (ops->spi_route(ops->context)) {error=S5123_BOOT_PREPARE;goto cleanup;}
    result->phase=S5123_BOOT_SPI_ROUTED;
    if (ops->spi_send(ops->context,image,length,&result->spi_completed) || result->spi_completed!=length) {
        error=S5123_BOOT_SPI;goto cleanup;
    }
    result->phase=S5123_BOOT_TX_COMPLETE;
cleanup:;
    /* Never short-circuit cleanup after any failure. Critical controls remain
     * output-low; do NOT restore them to floating inputs while freeing CP RAM.
     */
    int parked=!prepared || ops->spi_stop(ops->context)==0;
    if (gpio_touched) {
        if (ops->output(ops->context,S5123_RESET_N,0)) result->cleanup_error=1;
        if (ops->output(ops->context,S5123_POWER_ON,0)) result->cleanup_error=1;
        if (ops->sleep_us(ops->context,50000)) result->cleanup_error=1;
        if (ops->output(ops->context,S5123_AP_STATUS,0)) result->cleanup_error=1;
        if (ops->output(ops->context,S5123_AP_WAKE,0)) result->cleanup_error=1;
    }
    int restored=ops->spi_restore(ops->context)==0;
    int quiescent=!result->power_requested || ops->quiescent(ops->context);
    /* A failed settle delay or GPIO readback cannot be waived by a later
     * low-level snapshot. Keep RAM until a complete shutdown succeeds. */
    result->cleanup_safe=parked && restored && quiescent && !result->cleanup_error;
    if (!result->cleanup_safe || result->cleanup_error) {
        result->cleanup_error=1;
        if (!error) error=S5123_BOOT_CLEANUP;
    }
    result->error=error;
    return error;
}
#endif

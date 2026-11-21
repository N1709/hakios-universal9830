/* MIT. S5123 BOOT PIO transport core. No direct physical-memory access.
 * Register/board facts: pinned Samsung spi-s3c64xx.c + x1slte overlay (see docs).
 * Caller must prove mappings, clocks <=2.15 MHz, mode0, pin ownership, CP memory
 * safety and power/reset sequencing before setting ready. Not wired to hardware.
 * Controller completion is NOT evidence that the modem accepted/executed BOOT.
 */
#ifndef S5123_SPI_PIO_H
#define S5123_SPI_PIO_H
#include <stdint.h>
#include <stddef.h>

enum {
    S5123_SPI_OK=0, S5123_SPI_ARGUMENT=-1, S5123_SPI_NOT_READY=-2,
    S5123_SPI_ALREADY_ATTEMPTED=-3, S5123_SPI_TIMEOUT=-4, S5123_SPI_IO=-5
};
enum {
    S5123_CH=0x00, S5123_MODE=0x08, S5123_CS=0x0c, S5123_IRQ=0x10,
    S5123_STATUS=0x14, S5123_TX=0x18, S5123_RX=0x1c, S5123_PACKET=0x20,
    S5123_PENDING=0x24, S5123_SWAP=0x28, S5123_FEEDBACK=0x2c, S5123_USI_CON=0xc4
};
struct s5123_spi_ops {
    void *context;
    uint32_t (*read32)(void *, uint32_t offset);
    uint8_t (*read8)(void *, uint32_t offset);
    void (*write32)(void *, uint32_t offset, uint32_t value);
    void (*write8)(void *, uint32_t offset, uint8_t value);
    uint64_t (*now_us)(void *);
};
struct s5123_spi_result {
    uint32_t attempted, queued_bytes, completed_bytes, packets, last_status;
    int error;
};
struct s5123_spi_wait {
    uint64_t start, budget;
    uint32_t polls_left;
};

static inline int
s5123_spi_expired(const struct s5123_spi_ops *ops, struct s5123_spi_wait *wait)
{
    if (!wait->polls_left) return 1; /* Also bounded if a broken timer stops. */
    wait->polls_left--;
    return ops->now_us(ops->context) - wait->start >= wait->budget;
}

static inline void
s5123_spi_stop(const struct s5123_spi_ops *ops)
{
    ops->write32(ops->context, S5123_CH, 0);
    ops->write32(ops->context, S5123_PACKET, 0);
    ops->write32(ops->context, S5123_CS, 1);
    ops->write32(ops->context, S5123_PENDING, 0x1f);
    ops->write32(ops->context, S5123_PENDING, 0);
}

static inline int
s5123_spi_flush(const struct s5123_spi_ops *ops, struct s5123_spi_wait *wait,
    struct s5123_spi_result *result)
{
    ops->write32(ops->context, S5123_CH, 0);
    ops->write32(ops->context, S5123_PACKET, 0);
    ops->write32(ops->context, S5123_CH, 1u << 5);
    uint64_t start = ops->now_us(ops->context);
    for (;;) {
        if (s5123_spi_expired(ops, wait) || ops->now_us(ops->context)-start >= 1000) return S5123_SPI_TIMEOUT;
        uint32_t status = ops->read32(ops->context, S5123_STATUS);
        result->last_status = status;
        uint32_t tx = (status >> 6) & 0x7f, rx = (status >> 15) & 0x7f;
        if (tx > 64 || rx > 64) return S5123_SPI_IO;
        if (rx) (void)ops->read8(ops->context, S5123_RX);
        if (!tx && !rx) break;
    }
    ops->write32(ops->context, S5123_CH, 0);
    return S5123_SPI_OK;
}

static inline int
s5123_spi_send_boot(const struct s5123_spi_ops *ops, struct s5123_spi_result *result,
    const uint8_t *image, size_t length, uint32_t sclk_hz, uint64_t budget_us, int ready)
{
    if (!ops || !result || !image || !length || length > 131072 || !sclk_hz || sclk_hz > 2150000 ||
        !ops->read32 || !ops->read8 || !ops->write32 || !ops->write8 || !ops->now_us ||
        budget_us > 5000000 || budget_us < ((uint64_t)length*8000000+sclk_hz-1)/sclk_hz)
        return S5123_SPI_ARGUMENT;
    if (!ready) return S5123_SPI_NOT_READY;
    if (result->attempted) return S5123_SPI_ALREADY_ATTEMPTED;
    *result = (struct s5123_spi_result){.attempted=1};
    struct s5123_spi_wait wait = {ops->now_us(ops->context), budget_us, 16777216};
    int error = S5123_SPI_OK;
    /* Clock source/prescaler is external on this controller: never write CLK_CFG. */
    ops->write32(ops->context, S5123_CS, 1);
    ops->write32(ops->context, S5123_IRQ, 0);
    ops->write32(ops->context, S5123_USI_CON, 0); /* Clear active-high pad reset. */
    ops->write32(ops->context, S5123_MODE, 0x3ffu << 19); /* Byte bus/channel, no DMA/loopback. */
    ops->write32(ops->context, S5123_SWAP, 0);
    ops->write32(ops->context, S5123_FEEDBACK, 0);
    ops->write32(ops->context, S5123_PENDING, 0x1f);
    ops->write32(ops->context, S5123_PENDING, 0);
    for (size_t offset = 0; offset < length;) {
        error = s5123_spi_flush(ops, &wait, result);
        if (error) break;
        uint32_t count = length-offset > 64 ? 64 : (uint32_t)(length-offset);
        /* Board uses auto CS with NSC=2. Repeated <=64-byte packets match PIO. */
        ops->write32(ops->context, S5123_CS, 0x23);
        ops->write32(ops->context, S5123_PACKET, 0);
        ops->write32(ops->context, S5123_PACKET, count);
        ops->write32(ops->context, S5123_PACKET, count | (1u << 16));
        for (uint32_t i = 0; i < count; i++) ops->write8(ops->context, S5123_TX, image[offset+i]);
        result->queued_bytes += count;
        ops->write32(ops->context, S5123_CH, 3); /* Mode0 master, RX+TX, 8-bit. */
        for (;;) {
            if (s5123_spi_expired(ops, &wait)) { error = S5123_SPI_TIMEOUT; break; }
            uint32_t status = ops->read32(ops->context, S5123_STATUS);
            result->last_status = status;
            uint32_t tx = (status >> 6) & 0x7f, rx = (status >> 15) & 0x7f;
            if ((status & 0x3c) || tx > 64 || rx > count) { error = S5123_SPI_IO; break; }
            if (!tx && rx == count && (status & (1u << 25))) break;
        }
        if (error) break;
        /* TX-only still enables/drains RX, preventing FIFO overflow. */
        for (uint32_t i = 0; i < count; i++) (void)ops->read8(ops->context, S5123_RX);
        result->completed_bytes += count;
        result->packets++;
        offset += count;
    }
    if (!error) error = s5123_spi_flush(ops, &wait, result);
    s5123_spi_stop(ops);
    result->error = error;
    return error;
}
#endif

/* S20 rear controls shared with host fault-injection tests. No device I/O here. */
#ifndef REDWOOD_CAMERA_S20_CONTROLS_H
#define REDWOOD_CAMERA_S20_CONTROLS_H
#include <stdbool.h>
#include <stdint.h>

#define REDWOOD_2LD_PIXEL_CLOCK 1547000000U

static inline uint32_t
redwood_2ld_line_ns(uint32_t line_clocks)
{
    return (uint32_t)((uint64_t)line_clocks * 1000000000ULL / REDWOOD_2LD_PIXEL_CLOCK);
}

/* Current rear paths are normal/LN-off only. LN2/LN4 need separate constraints. */
static inline uint32_t
redwood_2ld_even_exposure(uint32_t lines)
{
    return lines & ~1U;
}

struct redwood_2ld_control_io {
    void *context;
    bool (*write16)(void *, uint16_t, uint16_t);
    bool (*write8)(void *, uint16_t, uint8_t);
};

/* Preserve existing grouped writes, but acknowledge every write and always try
 * to release a hold that may have reached the sensor, even when its ACK failed.
 * No retries: a partial transaction must end the stream through normal cleanup.
 */
static inline bool
redwood_2ld_write_exposure_mode(const struct redwood_2ld_control_io *io, bool grouped,
    uint32_t lines, uint32_t gain, uint32_t frame_lines, uint16_t *failed_reg)
{
    uint16_t failed = 0;
    if (!io->write16(io->context, 0xfcfc, 0x4000)) {
        *failed_reg = 0xfcfc;
        return false;
    }
    if (grouped && !io->write8(io->context, 0x0104, 1)) failed = 0x0104;
    if (!failed && frame_lines && !io->write16(io->context, 0x0340, (uint16_t)frame_lines)) failed = 0x0340;
    if (!failed && !io->write16(io->context, 0x0202, (uint16_t)lines)) failed = 0x0202;
    if (!failed && !io->write16(io->context, 0x0204, (uint16_t)gain)) failed = 0x0204;
    if (grouped && !io->write8(io->context, 0x0104, 0) && !failed) failed = 0x0104;
    *failed_reg = failed;
    return failed == 0;
}
/* Existing callers retain grouped operation. Stock-style no-hold is explicit. */
static inline bool
redwood_2ld_write_exposure(const struct redwood_2ld_control_io *io,
    uint32_t lines, uint32_t gain, uint32_t frame_lines, uint16_t *failed_reg)
{
    return redwood_2ld_write_exposure_mode(io, true, lines, gain, frame_lines, failed_reg);
}
#endif

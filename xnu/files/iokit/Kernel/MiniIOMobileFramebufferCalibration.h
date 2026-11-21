/* S6E3HAB gamma: three 10-bit values, 27 byte values, six packed nibbles.
 * Interpolate channel values, never the packed bytes (a carry changes colors).
 * Input/output are 34 parameter bytes, excluding DCS command 0xCA.
 */
#ifndef MINI_FB_CALIBRATION_H
#define MINI_FB_CALIBRATION_H
#include <stdint.h>
/* Samsung copy_from_dim_flash: raw row = nrow - logical_row - 1. */
static inline unsigned
fbCalRow(unsigned rows, unsigned logical)
{
	return rows - logical - 1;
}
static inline uint32_t
fbCalLerp(uint32_t a, uint32_t b, uint32_t weight)
{
	return (a * (65536 - weight) + b * weight + 32768) >> 16;
}
static inline void
fbCalGamma(const uint8_t *lo, const uint8_t *hi, uint32_t weight, uint8_t *out)
{
	out[0] = lo[0] & 0xc0;
	for (unsigned c = 0; c < 3; c++) {
		unsigned shift = (2 - c) * 2;
		uint32_t a = (((lo[0] >> shift) & 3) << 8) | lo[1 + c];
		uint32_t b = (((hi[0] >> shift) & 3) << 8) | hi[1 + c];
		uint32_t v = fbCalLerp(a, b, weight);
		out[0] |= (uint8_t)((v >> 8) << shift);
		out[1 + c] = (uint8_t)v;
	}
	for (unsigned i = 4; i < 31; i++) out[i] = (uint8_t)fbCalLerp(lo[i], hi[i], weight);
	for (unsigned i = 31; i < 34; i++) {
		out[i] = (uint8_t)((fbCalLerp(lo[i] >> 4, hi[i] >> 4, weight) << 4) |
		    fbCalLerp(lo[i] & 15, hi[i] & 15, weight));
	}
}
#endif

/* USB mux wire framing. Independent implementation of the public usbmuxd
 * device.c framing (network byte order); not the host daemon socket protocol.
 */
#ifndef REDWOOD_USBMUX_WIRE_H
#define REDWOOD_USBMUX_WIRE_H
#include <stddef.h>
#include <stdint.h>
#define RWMUX_MAX 65536u
#define RWMUX_MAGIC 0xfeedfaceu
static inline uint16_t rwmux_get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static inline uint32_t rwmux_get32(const uint8_t *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void rwmux_put16(uint8_t *p, uint16_t v) {
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}
static inline void rwmux_put32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}
/* 0 incomplete, -1 invalid, positive packet size. Never dereference a short header. */
static inline int rwmux_frame(const uint8_t *p, size_t n, int version) {
	if (n < 8)
		return 0;
	uint32_t len = rwmux_get32(p + 4), header = version >= 2 ? 16 : 8;
	if (len < header || len > RWMUX_MAX)
		return -1;
	if (n < header)
		return 0;
	if (version >= 2 && rwmux_get32(p + 8) != RWMUX_MAGIC)
		return -1;
	return n < len ? 0 : (int)len;
}
#endif

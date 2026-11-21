/* Samsung FINGE wire replies, research diagnostics only.
 * A candidate below is not an authenticated iOS unlock result. No HAT HMAC,
 * challenge, SID, freshness or keybag binding is verified by this parser.
 * Evidence: TA wrappers 0x11993c/0x119304 (+0x100000 Ghidra base).
 */
#ifndef REDWOOD_FP_WIRE_H
#define REDWOOD_FP_WIRE_H
#include <stdint.h>
#include <stddef.h>
#define FP_WIRE_SIZE 0x2ee00cu
#define FP_WIRE_RESPONSE 0x177008u
#define FP_WIRE_ERROR 0x2ee008u
#define FP_WIRE_ID_TOKEN_PRESENT (FP_WIRE_RESPONSE + 0x164085u)
enum fp_wire_result {
 FP_WIRE_PROTOCOL_ERROR, FP_WIRE_PENDING, FP_WIRE_NO_MATCH,
 FP_WIRE_REJECTED, FP_WIRE_MATCH_CANDIDATE
};
static inline uint32_t
fp_wire_u32(const uint8_t *p)
{
 return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
     (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* Full research requests are supplied by a privileged local tool, never IPC auth. */
static inline int
fp_wire_request_valid(const uint8_t *p, size_t size)
{
 uint32_t cmd;
 if (p == NULL || size != FP_WIRE_SIZE) return 0;
 cmd = fp_wire_u32(p);
 if (cmd != fp_wire_u32(p + 4)) return 0;
 switch (cmd) {
 case 1: return fp_wire_u32(p + 0xa010) <= 0xa000;
 case 2: case 3: case 4: case 6: case 7: case 9: case 10: return 1;
 case 5: return fp_wire_u32(p + 0x64110) <= 0x64000 &&
     fp_wire_u32(p + 0x64514) <= 0x400;
 case 13: return fp_wire_u32(p + 0x451) <= 0x400;
 case 12: return fp_wire_u32(p + 0xfa110) <= 0xfa000 &&
     fp_wire_u32(p + 0x271014) <= 0xfa000;
 default: return 0;
 }
}
/* TA bioIndexList is a bounded colon-terminated list, e.g. "1:2:". */
static inline uint32_t
fp_wire_id_mask(const uint8_t *ids, uint32_t len)
{
 uint32_t mask = 0;
 if (ids == NULL || len == 0 || len > 8 || (len & 1) != 0) return 0;
 for (uint32_t i = 0; i < len; i += 2) {
  if (ids[i] < '1' || ids[i] > '4' || ids[i + 1] != ':') return 0;
  uint32_t bit = 1u << (ids[i] - '1');
  if ((mask & bit) != 0) return 0;
  mask |= bit;
 }
 return mask;
}
static inline enum fp_wire_result
fp_wire_identify(const uint8_t *p, size_t size, uint32_t transport, uint32_t expected_id)
{
 uint32_t op, error, inner, bio;
 if (p == NULL || size < FP_WIRE_SIZE || transport != 0 ||
     fp_wire_u32(p + 0x177004) != 6) return FP_WIRE_PROTOCOL_ERROR;
 op = fp_wire_u32(p + FP_WIRE_RESPONSE);
 inner = fp_wire_u32(p + FP_WIRE_RESPONSE + 4);
 error = fp_wire_u32(p + FP_WIRE_ERROR);
 if (inner != error) return FP_WIRE_PROTOCOL_ERROR;
 /* 0x4d occurs on the TA's no-match path; it is not success. */
 if (error == 0x20) return FP_WIRE_NO_MATCH;
 if (error != 0) return FP_WIRE_REJECTED;
 if (op == 4 || op == 5 || op == 6 || op == 7) return FP_WIRE_PENDING;
 if (op != 0) return FP_WIRE_PROTOCOL_ERROR;
 bio = fp_wire_u32(p + FP_WIRE_RESPONSE + 20);
 if (expected_id == 0 || expected_id > 4 || bio != expected_id ||
     fp_wire_u32(p + FP_WIRE_ID_TOKEN_PRESENT) != 1) return FP_WIRE_PROTOCOL_ERROR;
 return FP_WIRE_MATCH_CANDIDATE;
}
#endif

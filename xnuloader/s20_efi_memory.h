/* MIT. Bounded ownership checks for a strided UEFI memory map. */
#ifndef XNULOADER_S20_EFI_MEMORY_H
#define XNULOADER_S20_EFI_MEMORY_H
#include "efi.h"

/* PAR_EL1 pass: F bit 0, output PA [47:12], Attr [63:56]. This S20
 * check is for the low (<4-GiB) CP identity map and Normal NC (0x44).
 */
static inline int
s20_par_normal_nc(uint64_t par, uint64_t address)
{
    const uint64_t pa_mask = 0x0000fffffffff000ULL;
    return !(par & 1) && (par >> 56) == 0x44 && (par & pa_mask) == (address & pa_mask);
}

static inline int
s20_par_device(uint64_t par, uint64_t address)
{
    const uint64_t pa_mask = 0x0000fffffffff000ULL;
    return !(par & 1) && (par >> 56) == 0 && (par & pa_mask) == (address & pa_mask);
}

static inline int
s20_efi_map_covers(const void *map, uint64_t bytes, uint64_t stride,
    uint64_t base, uint64_t size, uint32_t type, uint64_t cache_attribute)
{
    if (!size || size > ~0ULL - base || stride < sizeof(efi_memory_descriptor) || bytes % stride) return 0;
    uint64_t cursor = base, end = base + size;
    while (cursor < end) {
        uint64_t next = cursor;
        for (uint64_t off = 0; off < bytes; off += stride) {
            efi_memory_descriptor d;
            __builtin_memcpy(&d, (const uint8_t *)map + off, sizeof(d));
            if (d.type != type || (cache_attribute && (d.attribute & 0x1f) != cache_attribute) ||
                d.pages > (~0ULL - d.phys_start) / 4096) continue;
            uint64_t hi = d.phys_start + (d.pages << 12);
            if (d.phys_start <= cursor && hi > cursor && hi > next) next = hi;
        }
        if (next == cursor) return 0;
        cursor = next < end ? next : end;
    }
    return 1;
}
#endif

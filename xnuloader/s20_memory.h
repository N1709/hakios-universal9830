/* MIT. S20 CP regions from exynos9830.dts. Half-open physical intervals.
 * Pure address selection: never maps memory or grants DMA permissions.
 */
#ifndef XNULOADER_S20_MEMORY_H
#define XNULOADER_S20_MEMORY_H
#include <stdint.h>

static const struct s20_cp_region {
    uint64_t base, size;
} s20_cp_regions[] = {
    {0xf0000000ULL, 0x800000ULL},
    {0xf6200000ULL, 0x1000ULL},
    {0xd0000000ULL, 0x4000000ULL},
};

/* Highest aligned allocation within a free UEFI descriptor, below 4 GiB and
 * above 2 GiB, excluding every CP region. Zero means no candidate. Exact-size
 * AllocateAddress avoids the extra 64-KiB headroom that overlapped cp_rmem_2.
 */
static inline uint64_t
s20_memory_candidate(uint64_t lo, uint64_t hi, uint64_t size, uint64_t alignment)
{
    if (!size || (size & 0xfff) || alignment < 0x1000 || (alignment & (alignment - 1))) return 0;
    while (hi > lo && size <= hi - lo) {
        uint64_t at = (hi - size) & ~(alignment - 1);
        if (at < lo) return 0;
        unsigned i;
        for (i = 0; i < sizeof(s20_cp_regions) / sizeof(s20_cp_regions[0]); i++) {
            uint64_t base = s20_cp_regions[i].base;
            if (at < base + s20_cp_regions[i].size && base < at + size) {
                hi = base;
                break;
            }
        }
        if (i == sizeof(s20_cp_regions) / sizeof(s20_cp_regions[0])) return at;
    }
    return 0;
}

static inline uint64_t
s20_low_dma_candidate(uint64_t lo, uint64_t hi, uint64_t size, uint64_t alignment)
{
    if (lo < 0x80000000ULL) lo = 0x80000000ULL;
    if (hi > 0x100000000ULL) hi = 0x100000000ULL;
    return s20_memory_candidate(lo, hi, size, alignment);
}
#endif

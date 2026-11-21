/* MIT. Bounded single-owner circular queue operations for the S20 IPC backend.
 * Geometry from own DT; arithmetic follows one-empty-byte ring convention.
 * Backend must establish memory ownership/cache policy and acquire/release
 * ordering. No MMIO, modem commands, allocations or DMA enable in this core.
 * A publish failure is UNKNOWN COMMIT: caller must resync, never blindly retry.
 */
#ifndef S5123_RING_H
#define S5123_RING_H
#include <stddef.h>
#include <stdint.h>
enum {S5123_RING_OK=0,S5123_RING_ARGUMENT=-1,S5123_RING_INDEX=-2,
    S5123_RING_SPACE=-3,S5123_RING_IO=-4,S5123_RING_COMMIT_UNKNOWN=-5};
struct s5123_ring {uint32_t data,size,head,tail;};
static const struct s5123_ring s5123_raw_tx={0x3000,0x1fd000,0x18,0x1c};
static const struct s5123_ring s5123_raw_rx={0x200000,0x200000,0x20,0x24};
struct s5123_ring_io {
    void *context;
    int (*load_acquire)(void *,uint32_t offset,uint32_t *value);
    int (*copy_in)(void *,uint32_t offset,const uint8_t *,size_t);
    int (*copy_out)(void *,uint32_t offset,uint8_t *,size_t);
    int (*publish_release)(void *,uint32_t offset,uint32_t value);
};
static inline int s5123_ring_valid(const struct s5123_ring *r,uint32_t extent)
{
    if (!r || r->size<2 || r->data>extent || r->size>extent-r->data ||
        extent<4 || r->head>extent-4 || r->tail>extent-4 ||
        ((r->head|r->tail)&3) || r->head==r->tail) return 0;
    /* Metadata words must not overlap payload storage. */
    return (r->head+4<=r->data || r->head>=r->data+r->size) &&
        (r->tail+4<=r->data || r->tail>=r->data+r->size);
}
static inline int s5123_ring_transfer(const struct s5123_ring_io *io,
    const struct s5123_ring *r,uint32_t extent,uint8_t *data,size_t length,int transmit)
{
    if (!io || !io->load_acquire || !io->publish_release || !data || !length ||
        !s5123_ring_valid(r,extent) || length>=r->size ||
        (transmit!=0 && transmit!=1) || (transmit?!io->copy_in:!io->copy_out)) return S5123_RING_ARGUMENT;
    uint32_t head,tail;
    if (io->load_acquire(io->context,r->head,&head) || io->load_acquire(io->context,r->tail,&tail)) return S5123_RING_IO;
    if (head>=r->size || tail>=r->size) return S5123_RING_INDEX;
    uint32_t used=head>=tail ? head-tail : r->size-(tail-head);
    uint32_t available=transmit ? r->size-1-used : used;
    if (length>available) return S5123_RING_SPACE;
    uint32_t position=transmit?head:tail;
    size_t first=length<r->size-position?length:r->size-position;
    int error=transmit ? io->copy_in(io->context,r->data+position,data,first) :
        io->copy_out(io->context,r->data+position,data,first);
    if (!error && length>first) error=transmit ? io->copy_in(io->context,r->data,data+first,length-first) :
        io->copy_out(io->context,r->data,data+first,length-first);
    if (error) return S5123_RING_IO; /* Producer/consumer index still unchanged. */
    uint32_t next=(uint32_t)(((uint64_t)position+length)%r->size);
    if (io->publish_release(io->context,transmit?r->head:r->tail,next)) return S5123_RING_COMMIT_UNKNOWN;
    return S5123_RING_OK;
}
#endif

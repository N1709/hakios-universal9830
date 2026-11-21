/* S20 GNSS wire facts recovered from stock lhd/gpsd. Independent implementation.
 * Pure codec: no device I/O, power changes, acquisition or position fabrication.
 */
#ifndef REDWOOD_GNSS_PROTOCOL_H
#define REDWOOD_GNSS_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define GNSS_MEIF_MAX 3072u
static inline uint16_t gnss_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static inline void gnss_put16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static inline uint16_t gnss_checksum(const uint8_t *p,size_t n) { uint16_t sum=0;while(n--)sum=(uint16_t)(sum+*p++);return sum; }
static inline bool gnss_meif_valid(const uint8_t *p,size_t n)
{
    return n>=8 && n<=GNSS_MEIF_MAX && !(n&1) && gnss_u16(p+2)==n-2 &&
        gnss_u16(p+n-2)==gnss_checksum(p,n-2);
}
/* Complete unescaped MEIF packet; doubles literal A5 bytes. Zero on overflow. */
static inline size_t gnss_meif_wrap(uint8_t *out,size_t cap,const uint8_t *p,size_t n)
{
    if(!gnss_meif_valid(p,n) || cap<4)return 0;
    size_t need=4+n;for(size_t i=0;i<n;i++)if(p[i]==0xa5)need++;
    if(need>cap)return 0;
    size_t at=0;out[at++]=0xa5;out[at++]=0x93;
    for(size_t i=0;i<n;i++){out[at++]=p[i];if(p[i]==0xa5)out[at++]=0xa5;}
    out[at++]=0xa5;out[at++]=0x68;return at;
}
struct gnss_meif_decoder { unsigned state;size_t length;uint8_t data[GNSS_MEIF_MAX];uint32_t malformed; };
typedef void (*gnss_packet_fn)(void *,const uint8_t *,size_t);
static inline void gnss_meif_feed(struct gnss_meif_decoder *d,const uint8_t *p,size_t n,gnss_packet_fn fn,void *ctx)
{
    for(size_t i=0;i<n;i++) {
        uint8_t b=p[i];
        switch(d->state) {
        case 0:if(b==0xa5)d->state=3;break;
        case 3:if(b==0x93){d->state=2;d->length=0;}else if(b!=0xa5)d->state=0;break;
        case 2:
            if(b==0xa5)d->state=1;
            else if(d->length<sizeof(d->data))d->data[d->length++]=b;
            else{d->malformed++;d->state=0;d->length=0;}
            break;
        case 1:
            if(b==0xa5) {
                if(d->length<sizeof(d->data)){d->data[d->length++]=b;d->state=2;}
                else{d->malformed++;d->state=0;d->length=0;}
            } else if(b==0x93){d->length=0;d->state=2;}
            else {
                if(b==0x68 && gnss_meif_valid(d->data,d->length))fn(ctx,d->data,d->length);
                else d->malformed++;
                d->state=0;d->length=0;
            }
            break;
        default:d->state=0;d->length=0;break;
        }
    }
}
/* RPC1E body, not an entire BBD frame. The kernel adds the outer RPC/transport. */
static inline size_t gnss_navigation_body(uint8_t *out,size_t cap,const uint8_t *wire,size_t n)
{
    if(n>998 || cap<n+2)return 0;gnss_put16(out,(uint16_t)n);memcpy(out+2,wire,n);return n+2;
}
#endif

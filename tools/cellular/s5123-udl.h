/* MIT. Original bounded SIPC UDL wire codec; no device I/O.
 * Protocol facts: Samsung cpboot_v3 ProtocolSipc/BootDumpExt and S20 CPIF.
 * Pinned references and stage ordering: docs/devices/x1slte/cellular/download-plan.md.
 * A wire-compatible packet is not evidence that the CP has accepted it.
 */
#ifndef S5123_UDL_H
#define S5123_UDL_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define S5123_UDL_MSS 2048u
#define S5123_UDL_WIRE_MAX 2072u
enum s5123_udl_op { S5123_UDL_START=0, S5123_UDL_SEND=1,
    S5123_UDL_CRC=12, S5123_UDL_DONE=13 };
enum s5123_udl_ack { S5123_UDL_ACK=0, S5123_UDL_PEER_FAIL=-1,
    S5123_UDL_UNEXPECTED=-2, S5123_UDL_MALFORMED=-3 };

static inline void s5123_udl_put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));}
static inline uint32_t s5123_udl_get32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline int s5123_udl_valid(unsigned stage,enum s5123_udl_op op)
{
    if(stage==15) return op==S5123_UDL_START;
    return stage>=1 && stage<=12 && (op==S5123_UDL_START || op==S5123_UDL_SEND ||
        op==S5123_UDL_CRC || op==S5123_UDL_DONE);
}
/* All supported TX packets have the ordinary 4-byte SIPC5 header, ch215,
 * little-endian length excluding zero-filled padding to 8 bytes. */
static inline size_t s5123_udl_frame(uint8_t *out,size_t cap,size_t payload)
{
    size_t len=4+payload,total=(len+7)&~(size_t)7;
    if(!out || payload>16+S5123_UDL_MSS || cap<total) return 0;
    memset(out,0,total);out[0]=0xfc;out[1]=215;
    out[2]=(uint8_t)len;out[3]=(uint8_t)(len>>8);
    return total;
}
/* Zero return means no packet produced and output untouched. */
static inline size_t s5123_udl_command(uint8_t *out,size_t cap,unsigned stage,
    enum s5123_udl_op op,uint32_t crc)
{
    if(!s5123_udl_valid(stage,op) || op==S5123_UDL_SEND) return 0;
    size_t n=s5123_udl_frame(out,cap,op==S5123_UDL_CRC?8:4);
    if(!n) return 0;
    s5123_udl_put32(out+4,0x9000u|(stage<<8)|(unsigned)op);
    if(op==S5123_UDL_CRC) s5123_udl_put32(out+8,crc);
    return n;
}
/* File-backed caller supplies exactly one chunk; caller retains storage.
 * Derive total frames and final length from image size, never peer input.
 * Payload must not overlap out. No whole-firmware allocation is needed. */
static inline size_t s5123_udl_data(uint8_t *out,size_t cap,unsigned stage,
    uint32_t image_size,uint32_t frame,const uint8_t *data,size_t data_size)
{
    if(!s5123_udl_valid(stage,S5123_UDL_SEND) || !image_size || !data) return 0;
    uint32_t frames=image_size/S5123_UDL_MSS+(image_size%S5123_UDL_MSS!=0);
    if(!frame || frame>frames) return 0;
    uint32_t offset=(frame-1)*S5123_UDL_MSS;
    size_t chunk=image_size-offset;
    if(chunk>S5123_UDL_MSS) chunk=S5123_UDL_MSS;
    if(data_size!=chunk) return 0;
    size_t n=s5123_udl_frame(out,cap,16+chunk);
    if(!n) return 0;
    s5123_udl_put32(out+4,0x9001u|(stage<<8));
    s5123_udl_put32(out+8,frames);s5123_udl_put32(out+12,frame);
    s5123_udl_put32(out+16,(uint32_t)chunk);memcpy(out+20,data,chunk);
    return n;
}
/* Exactly one complete response, after queue framing has assembled it.
 * No auto retries: wrong/failed ACK or uncertain publish ends a transaction.
 * SEND ACK is expected once per image, not once per data fragment. */
static inline enum s5123_udl_ack s5123_udl_response(const uint8_t *in,size_t n,
    unsigned stage,enum s5123_udl_op op)
{
    if(!in || n!=8 || !s5123_udl_valid(stage,op) ||
        (in[0]!=0xf8 && in[0]!=0xfc) || in[1]!=215 || in[2]!=8 || in[3]!=0)
        return S5123_UDL_MALFORMED;
    uint32_t response=s5123_udl_get32(in+4),base=0xa000u|(stage<<8);
    if(response==(base|(unsigned)op)) return S5123_UDL_ACK;
    if(response==(base|15u)) return S5123_UDL_PEER_FAIL;
    return S5123_UDL_UNEXPECTED;
}
#endif

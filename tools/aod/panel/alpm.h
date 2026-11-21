/* S20 hubble1 ALPM protocol facts from Samsung GPL kernel 69515fbb7:
 * s6e3hab_hubble1_a3_s0_panel.h tables/packets and panel_drv.c sequencing.
 * This portable transport core does not change clocks, rails or display modes.
 * Caller serializes against frame/mode/brightness writes and supplies restoration.
 * Not connected to the live driver until those integration contracts are met. */
#ifndef REDWOOD_S20_ALPM_H
#define REDWOOD_S20_ALPM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum rw_alpm_state { RW_ALPM_NORMAL, RW_ALPM_ACTIVE, RW_ALPM_FAULT };
struct rw_alpm {
    enum rw_alpm_state state;
    void *context;
    bool (*write)(void *,const uint8_t *,size_t,uint8_t);
    void (*wait_us)(void *,unsigned);
    bool (*restore_normal)(void *);
};
struct rw_alpm_gate {
    bool experimental_enabled, display_on, normal_mode_60hz, no_pending_transition;
    bool normal_settings_saved;
    uint8_t panel_id[3];
    unsigned panel_on_age_us;
};
static inline bool rw_alpm_packet(struct rw_alpm *a,const uint8_t *p,size_t n,uint8_t offset){
    return a->write(a->context,p,n,offset);
}
static inline bool rw_alpm_exit(struct rw_alpm *a){
    if(!a||!a->write||!a->wait_us||!a->restore_normal)return false;
    if(a->state==RW_ALPM_NORMAL)return true;
    const uint8_t key_on[]={0xf0,0x5a,0x5a},key_off[]={0xf0,0xa5,0xa5};
    const uint8_t key3_off[]={0xfc,0xa5,0xa5},nit[]={0xbb,0x03};
    const uint8_t aswire[]={0xb5,0x00},avc[]={0xf4,0xca},normal[]={0x53,0x00};
    /* Best-effort every exit/lock operation even after a failed transfer. */
    bool ok=rw_alpm_packet(a,key_on,sizeof(key_on),0);
    ok=rw_alpm_packet(a,nit,sizeof(nit),0)&&ok;
    ok=rw_alpm_packet(a,aswire,sizeof(aswire),0x53)&&ok;
    ok=rw_alpm_packet(a,avc,sizeof(avc),0x09)&&ok;
    ok=rw_alpm_packet(a,normal,sizeof(normal),0)&&ok;
    ok=rw_alpm_packet(a,key3_off,sizeof(key3_off),0)&&ok;
    ok=rw_alpm_packet(a,key_off,sizeof(key_off),0)&&ok;
    ok=a->restore_normal(a->context)&&ok;
    a->wait_us(a->context,34000);
    a->state=ok?RW_ALPM_NORMAL:RW_ALPM_FAULT;
    return ok;
}
static inline bool rw_alpm_enter(struct rw_alpm *a,const struct rw_alpm_gate *g){
    if(!a||!g||!a->write||!a->wait_us||!a->restore_normal)return false;
    if(a->state==RW_ALPM_FAULT)return false;
    if(!g->experimental_enabled||!g->display_on||!g->normal_mode_60hz||
       !g->no_pending_transition||!g->normal_settings_saved||
       g->panel_id[0]!=0x81||g->panel_id[1]!=0x01||g->panel_id[2]!=0x03)return false;
    if(a->state==RW_ALPM_ACTIVE)return true;
    if(g->panel_on_age_us<124000)a->wait_us(a->context,124000-g->panel_on_age_us);
    const uint8_t key_on[]={0xf0,0x5a,0x5a},key_off[]={0xf0,0xa5,0xa5};
    const uint8_t key3_on[]={0xfc,0x5a,0x5a},key3_off[]={0xfc,0xa5,0xa5};
    const uint8_t aor[]={0xb1,0x0c,0x65},gamma[]={0xf7,0x0f};
    const uint8_t nit[]={0xbb,0x0b,0x0c,0x0c,0x43,0x0c,0x43,0x80};
    const uint8_t isc[]={0xf6,0xff},avs[]={0xfd,0x0c},mode[]={0x53,0x03},avc[]={0xf4,0x8a};
    a->state=RW_ALPM_FAULT; /* Any partial entry requires a complete restoration. */
#define RW_ALPM_SEND(p,o) do{if(!rw_alpm_packet(a,p,sizeof(p),o))goto failed;}while(0)
    RW_ALPM_SEND(key_on,0);RW_ALPM_SEND(aor,0);RW_ALPM_SEND(gamma,0);RW_ALPM_SEND(key_off,0);
    a->wait_us(a->context,16700);
    RW_ALPM_SEND(key_on,0);RW_ALPM_SEND(nit,0);RW_ALPM_SEND(key3_on,0);
    RW_ALPM_SEND(isc,0x10);RW_ALPM_SEND(avs,0x28);RW_ALPM_SEND(key3_off,0);
    RW_ALPM_SEND(mode,0);RW_ALPM_SEND(avc,0x09);RW_ALPM_SEND(gamma,0);RW_ALPM_SEND(key_off,0);
#undef RW_ALPM_SEND
    a->state=RW_ALPM_ACTIVE;return true;
failed:
    (void)rw_alpm_exit(a);return false;
}
#endif

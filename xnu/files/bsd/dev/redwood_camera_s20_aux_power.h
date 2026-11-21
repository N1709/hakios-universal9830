/* Bounded 2LA rail transaction; shared by kernel adapter and host fault tests.
 * DT: BUCK2 1.3V, LDO12 analog2.8V, LDO3 core1.05V, LDO8 IO1.8V.
 * Samsung regulator formulas: BUCK400mV+6.25mV*n; LDO600mV+step*n.
 * A live dedicated rail or wrong shared buck voltage prevents all writes. */
#ifndef REDWOOD_CAMERA_S20_AUX_POWER_H
#define REDWOOD_CAMERA_S20_AUX_POWER_H
#include <stdint.h>
#include <stdbool.h>
struct cam_aux_power { uint8_t before[4]; unsigned touched; };
struct cam_aux_power_io {
    void *context;
    bool (*read)(void *, uint8_t, uint8_t *);
    bool (*write)(void *, uint8_t, uint8_t);
};
static const uint8_t cam_aux_power_regs[4] = {0x0d, 0x29, 0x20, 0x25};
static const uint8_t cam_aux_power_want[4] = {0xc0, 0xd8, 0xa4, 0xb0};
static inline bool cam_aux_power_restore(struct cam_aux_power *s, struct cam_aux_power_io io) {
    bool ok=true;
    while(s->touched){
        unsigned i=--s->touched;uint8_t after=0;
        /* Continue restoring other rails even if one bus operation fails. */
        bool written=io.write(io.context,cam_aux_power_regs[i],s->before[i]);
        bool read=io.read(io.context,cam_aux_power_regs[i],&after);
        if(!written||!read||after!=s->before[i])ok=false;
    }
    return ok;
}
static inline bool cam_aux_power_begin(struct cam_aux_power *s, struct cam_aux_power_io io) {
    uint8_t buck_voltage=0;
    s->touched=0;
    if(!io.read(io.context,0x0e,&buck_voltage)||buck_voltage!=0x90)return false;
    for(unsigned i=0;i<4;i++)if(!io.read(io.context,cam_aux_power_regs[i],&s->before[i]))return false;
    for(unsigned i=1;i<4;i++)if(s->before[i]&0x80)return false;
    for(unsigned i=0;i<4;i++){
        uint8_t want=i?cam_aux_power_want[i]:(uint8_t)(s->before[0]|0xc0),after=0;
        /* Mark BEFORE the write: a NACK may follow a latched device write. */
        s->touched=i+1;
        if(!io.write(io.context,cam_aux_power_regs[i],want)||
           !io.read(io.context,cam_aux_power_regs[i],&after)||after!=want)return false;
    }
    return true;
}
#endif

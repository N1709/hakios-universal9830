/* Fixed x1slte primary-only selectors. Change voltages only with outputs off. */
#ifndef REDWOOD_CAMERA_S20_MAIN_VOLTAGE_H
#define REDWOOD_CAMERA_S20_MAIN_VOLTAGE_H
#include <stdint.h>
#include <stdbool.h>
struct cam_main_voltage {uint8_t before[3];unsigned changed;};
struct cam_main_voltage_io {
    void *ctx;
    bool (*read)(void *,uint8_t,uint8_t,uint8_t *);
    bool (*write)(void *,uint8_t,uint8_t,uint8_t);
};
static const uint8_t cam_main_v_pmic[3]={2,3,3},cam_main_v_reg[3]={0x1e,0x04,0x07};
static const uint8_t cam_main_v_mask[3]={0x7f,0x3f,0x3f},cam_main_v_target[3]={0x24,0x1c,0x2e};
static bool cam_main_voltage_restore(struct cam_main_voltage *s,struct cam_main_voltage_io io){
    bool clean=true;
    for(unsigned j=3;j>0;j--){unsigned i=j-1;uint8_t live=0,after=0,mask=cam_main_v_mask[i];
        if(!(s->changed&(1U<<i)))continue;
        if(!io.read(io.ctx,cam_main_v_pmic[i],cam_main_v_reg[i],&live)){clean=false;continue;}
        if((live&mask)==(s->before[i]&mask)){s->changed&=~(1U<<i);continue;}
        /* Never alter a live output or overwrite another owner's new selector. */
        if((live&0x80)||(live&mask)!=cam_main_v_target[i]){clean=false;continue;}
        uint8_t want=(uint8_t)((live&~mask)|(s->before[i]&mask));
        if(!io.write(io.ctx,cam_main_v_pmic[i],cam_main_v_reg[i],want)||
           !io.read(io.ctx,cam_main_v_pmic[i],cam_main_v_reg[i],&after)||after!=want){clean=false;continue;}
        s->changed&=~(1U<<i);
    }return clean;
}
static bool cam_main_voltage_begin(struct cam_main_voltage *s,struct cam_main_voltage_io io){
    if(s->changed)return false;
    for(unsigned i=0;i<3;i++)if(!io.read(io.ctx,cam_main_v_pmic[i],cam_main_v_reg[i],&s->before[i]))return false;
    for(unsigned i=0;i<3;i++)if((s->before[i]&0x80)&&(s->before[i]&cam_main_v_mask[i])!=cam_main_v_target[i])return false;
    for(unsigned i=0;i<3;i++){
        uint8_t mask=cam_main_v_mask[i],want=(uint8_t)((s->before[i]&~mask)|cam_main_v_target[i]),after=0;
        if(want==s->before[i])continue;
        s->changed|=1U<<i; /* NACK may follow a latched write. */
        if(!io.write(io.ctx,cam_main_v_pmic[i],cam_main_v_reg[i],want)||
           !io.read(io.ctx,cam_main_v_pmic[i],cam_main_v_reg[i],&after)||after!=want)return false;
    }return true;
}
/* Enable updates must compare to CURRENT state, never a saved snapshot. */
struct cam_main_enable_io {void *ctx;bool(*read)(void *,uint8_t,uint8_t *);bool(*write)(void *,uint8_t,uint8_t);};
static inline bool cam_main_enable_bits(struct cam_main_enable_io io,uint8_t reg,uint8_t value){
    uint8_t current=0,after=0;
    if((reg!=0x04&&reg!=0x05&&reg!=0x07)||(value&~0x80))return false;
    if(!io.read(io.ctx,reg,&current))return false;
    uint8_t want=(uint8_t)((current&0x7f)|value);
    if(want==current)return true;
    return io.write(io.ctx,reg,want)&&io.read(io.ctx,reg,&after)&&after==want;
}
#endif

/* Checked executor for the ordinary A202 registers: no burst/firmware ops. */
#ifndef REDWOOD_CAMERA_S20_AUX_TABLE_H
#define REDWOOD_CAMERA_S20_AUX_TABLE_H
#include <stdint.h>
#include <stdbool.h>
struct cam_aux_table_io {void *ctx;bool(*write16)(void *,uint16_t,uint16_t);void(*pause)(void *,unsigned);};
static inline bool cam_aux_apply_table(struct cam_aux_table_io io,const uint32_t *v,unsigned words){
    if(!v||!words||words%3||words>3*256)return false;
    unsigned total_delay=0;
    /* Validate the entire input before any hardware operation. */
    for(unsigned i=0;i<words;i+=3){
        if(v[i]==0xf0000003U){if(v[i+2]!=0||v[i+1]>10000)return false;total_delay+=v[i+1];}
        else if(v[i]>0xffff||v[i+1]>0xffff||v[i+2]!=2)return false;
    }
    if(total_delay>100000)return false;
    for(unsigned i=0;i<words;i+=3){
        if(v[i]==0xf0000003U)io.pause(io.ctx,v[i+1]);
        else if(!io.write16(io.ctx,(uint16_t)v[i],(uint16_t)v[i+1]))return false;
    }
    return true;
}
#endif

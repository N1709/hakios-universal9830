/* Checked GW2 setfile executor. Validate all records before changing hardware;
 * preserve indirect address auto-increment across bounded I2C burst chunks. */
#ifndef REDWOOD_CAMERA_S20_GW2_TABLE_H
#define REDWOOD_CAMERA_S20_GW2_TABLE_H
#include <stdint.h>
#include <stdbool.h>
struct cam_gw2_table_io {void *ctx;bool(*send)(void *,const uint8_t *,unsigned);void(*pause)(void *,unsigned);};
static inline bool cam_gw2_apply_table(struct cam_gw2_table_io io,const uint32_t *v,unsigned words){
    if(!v||!words||words%3||words>15000)return false;
    unsigned delay=0;bool burst=false;
    for(unsigned i=0;i<words;i+=3){
        uint32_t reg=v[i],value=v[i+1],width=v[i+2];
        if(reg==0xf0000003U){if(width||value>10000)return false;delay+=value;burst=false;}
        else if(reg==0xf0000001U){if(width!=2||value!=0x6f12)return false;burst=true;}
        else if(reg==0xf0000002U){if(!burst||width!=2||value>0xffff)return false;}
        else {if(reg>0xffff||value>0xffff||width!=2)return false;burst=false;}
    }
    if(delay>100000)return false;
    uint8_t bytes[258];unsigned count=0;burst=false;
    io.pause(io.ctx,3000);
    for(unsigned i=0;i<words;i+=3){
        uint32_t reg=v[i],value=v[i+1];
        if(reg!=0xf0000002U&&count){if(!io.send(io.ctx,bytes,2+2*count))return false;count=0;}
        if(reg==0xf0000001U){bytes[0]=value>>8;bytes[1]=value;burst=true;}
        else if(reg==0xf0000002U){
            if(count==128){if(!io.send(io.ctx,bytes,258))return false;count=0;}
            bytes[2+2*count]=value>>8;bytes[3+2*count]=value;count++;
        }else if(reg==0xf0000003U){burst=false;io.pause(io.ctx,value);}
        else {uint8_t pair[4]={reg>>8,reg,value>>8,value};burst=false;if(!io.send(io.ctx,pair,4))return false;}
    }
    (void)burst;
    return !count||io.send(io.ctx,bytes,2+2*count);
}
#endif

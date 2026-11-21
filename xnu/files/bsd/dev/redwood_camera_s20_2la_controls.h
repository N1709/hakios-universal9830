/* Fixed full12MP mode controls from Samsung2LA driver: CIT16..FLL-16,
 * again0x20..0x200 (1..16x), dgain0x100 unity, normal non-WDR/non-LTE. */
#ifndef REDWOOD_CAMERA_S20_2LA_CONTROLS_H
#define REDWOOD_CAMERA_S20_2LA_CONTROLS_H
#include <stdint.h>
#include <stdbool.h>
struct ax_2la_control_io {void *ctx;bool(*read16)(void*,uint16_t,uint16_t*);bool(*write16)(void*,uint16_t,uint16_t);bool(*write8)(void*,uint16_t,uint8_t);};
struct ax_2la_control_report {uint16_t before[3],after[3],frame_lines,line_pck;bool applied;};
static inline bool ax_2la_controls_valid(uint64_t lines,uint64_t gain){return lines>=16&&lines<=6652-16&&gain>=0x20&&gain<=0x200;}
static inline bool ax_sensor_controls_apply(struct ax_2la_control_io io,uint16_t lines,uint16_t gain,struct ax_2la_control_report *r,uint16_t frame_lines,uint16_t line_pck){
    static const uint16_t regs[3]={0x0202,0x0204,0x020e};uint16_t value=0;
    r->applied=false;
    if(lines<16||lines>frame_lines-16||gain<0x20||gain>0x200)return false;
    if(!io.read16(io.ctx,0x0340,&r->frame_lines)||!io.read16(io.ctx,0x0342,&r->line_pck)||r->frame_lines!=frame_lines||r->line_pck!=line_pck)return false;
    for(unsigned i=0;i<3;i++)if(!io.read16(io.ctx,regs[i],&r->before[i]))return false;
    if(!io.write16(io.ctx,0xfcfc,0x4000)||!io.write8(io.ctx,0x0702,0)||!io.write8(io.ctx,0x0704,0)||
       !io.write16(io.ctx,0x021e,0)||!io.write16(io.ctx,0x0202,lines)||!io.write16(io.ctx,0x0204,gain)||!io.write16(io.ctx,0x020e,0x100))return false;
    for(unsigned i=0;i<3;i++)if(!io.read16(io.ctx,regs[i],&r->after[i]))return false;
    if(r->after[0]!=lines||r->after[1]!=gain||r->after[2]!=0x100)return false;
    if(!io.read16(io.ctx,0x021e,&value)||value!=0)return false;
    r->applied=true;return true;
}
static inline bool ax_2la_controls_apply(struct ax_2la_control_io io,uint16_t lines,uint16_t gain,struct ax_2la_control_report *r){return ax_sensor_controls_apply(io,lines,gain,r,6652,11200);}
static inline bool ax_gw2_controls_apply(struct ax_2la_control_io io,uint16_t lines,uint16_t gain,struct ax_2la_control_report *r){return ax_sensor_controls_apply(io,lines,gain,r,3608,17680);}
static inline bool ax_gw2_full_controls_apply(struct ax_2la_control_io io,uint16_t lines,uint16_t gain,struct ax_2la_control_report *r){return ax_sensor_controls_apply(io,lines,gain,r,5166,12304);}
#endif

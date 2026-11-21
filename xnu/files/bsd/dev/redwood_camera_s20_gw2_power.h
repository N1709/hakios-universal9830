/* GW2 power-on order from x1slte DT; fixed rails, never arbitrary PMIC writes.
 * Snapshot both PMICs before writes; restore across both in reverse order. */
#ifndef REDWOOD_CAMERA_S20_GW2_POWER_H
#define REDWOOD_CAMERA_S20_GW2_POWER_H
#include <stdint.h>
#include <stdbool.h>
struct cam_gw2_rail {uint8_t pmic,reg,value,selector_mask;bool shared;unsigned delay_us;};
static const struct cam_gw2_rail cam_gw2_rails[8]={
    {3,0x08,0xa8,0x3f,false,0},    /* VDDA2.8SUB: 1.8V+25mV*40 */
    {3,0x03,0x9c,0x3f,false,0},    /* VDDD1.05SUB: .7V+12.5mV*28 */
    {2,0x2a,0xd8,0x7f,false,2000}, /* VDDAF2.8SUB */
    {3,0x09,0xa8,0x3f,true,100},   /* OIS VM2.8 */
    {2,0x27,0xb0,0x7f,true,0},     /* OIS digital1.8 */
    {3,0x05,0xac,0x3f,true,100},   /* CAM IO1.8 */
    {2,0x23,0xb0,0x7f,true,0},     /* VT IO1.8 */
    {2,0x24,0xb0,0x7f,false,100}   /* SUB IO1.8 */
};
struct cam_gw2_power {uint8_t before[8];unsigned touched;bool complete;};
struct cam_gw2_io {
    void *context;
    bool (*read)(void *,uint8_t,uint8_t,uint8_t *);
    bool (*write)(void *,uint8_t,uint8_t,uint8_t);
    void (*pause)(void *,unsigned);
};
static inline bool cam_gw2_power_restore(struct cam_gw2_power *s,struct cam_gw2_io io){
    bool ok=true;
    /* A successful transaction follows the board's NORMAL_OFF order, not the
     * reverse ON order. Partial failures still roll back touched rails in reverse. */
    static const unsigned off_order[8]={2,5,7,6,0,1,4,3};
    unsigned count=s->touched;bool complete=s->complete;s->touched=0;s->complete=false;
    for(unsigned step=0;step<count;step++){
        unsigned i=complete?off_order[step]:count-1-step;uint8_t after=0;
        const struct cam_gw2_rail *r=&cam_gw2_rails[i];
        bool w=io.write(io.context,r->pmic,r->reg,s->before[i]);
        bool got=io.read(io.context,r->pmic,r->reg,&after);
        if(!w||!got||after!=s->before[i])ok=false;
        if(complete&&step==0)io.pause(io.context,9); /* DT AF-off delay9us */
    }
    return ok;
}
static inline bool cam_gw2_power_begin(struct cam_gw2_power *s,struct cam_gw2_io io){
    uint8_t v=0;s->touched=0;s->complete=false;
    /* Existing parent supplies must already match the board's always-on setup. */
    if(!io.read(io.context,2,0x0b,&v)||(v&0xc0)!=0xc0||
       !io.read(io.context,2,0x0c,&v)||v!=0x90||
       !io.read(io.context,2,0x0f,&v)||(v&0xc0)!=0xc0||
       !io.read(io.context,2,0x10,&v)||v!=0x48||
       !io.read(io.context,2,0x2f,&v)||v!=0xec)return false;
    for(unsigned i=0;i<8;i++){
        const struct cam_gw2_rail *r=&cam_gw2_rails[i];
        if(!io.read(io.context,r->pmic,r->reg,&s->before[i]))return false;
        if(s->before[i]&0x80){
            if(!r->shared||(s->before[i]&r->selector_mask)!=(r->value&r->selector_mask))return false;
        }
    }
    for(unsigned i=0;i<8;i++){
        const struct cam_gw2_rail *r=&cam_gw2_rails[i];uint8_t after=0;
        uint8_t mask=(uint8_t)(r->selector_mask|0x80);
        uint8_t want=(uint8_t)((s->before[i]&~mask)|(r->value&mask));
        s->touched=i+1; /* failed transfer may still have latched */
        if(!io.write(io.context,r->pmic,r->reg,want)||
           !io.read(io.context,r->pmic,r->reg,&after)||after!=want)return false;
        if(r->delay_us)io.pause(io.context,r->delay_us);
    }
    s->complete=true;return true;
}
#endif

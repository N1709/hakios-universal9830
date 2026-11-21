#include <assert.h>
#include <stdio.h>
#include "../../userland/src/raise_policy.h"

static unsigned replay(bool allowed, bool pickup, bool face_down, bool shaking)
{
    struct redwood_raise p={0}; unsigned detections=0;
    for(unsigned i=0;i<100;i++) {
        double t=1+i*.05, angle=pickup ? fmin(1., fmax(0., (t-2)*1.2)) : 0;
        double y=-sin(angle), z=(face_down?1:-1)*cos(angle);
        double speed=pickup && t>2 && angle<1 ? 1.2 : 0;
        detections += redwood_raise_sample(&p,t,allowed,0,y,z,speed,shaking?1.5:0);
    }
    return detections;
}
int main(void)
{
    assert(replay(true,true,false,false)==1);
    assert(replay(false,true,false,false)==0); /* display on / user disabled */
    assert(replay(true,false,false,false)==0); /* stationary on table */
    assert(replay(true,true,true,false)==0);   /* face down */
    assert(replay(true,true,false,true)==0);   /* shake */
    struct redwood_raise p={0};
    for(unsigned i=0;i<20;i++) assert(!redwood_raise_sample(&p,1+i*.05,true,0,0,-1,0,0));
    assert(p.armed);
    assert(!redwood_raise_sample(&p,2,true,0,-.4,-.9,8,1.2)); /* pickup impulse */
    assert(p.armed);
    assert(!redwood_raise_sample(&p,2.1,true,0,-.8,-.6,.5,0));
    assert(redwood_raise_sample(&p,2.3,true,0,-.8,-.6,0,0));
    for(unsigned i=0;i<20;i++) assert(!redwood_raise_sample(&p,6+i*.05,true,0,0,-1,0,0));
    assert(p.armed);
    assert(!redwood_raise_sample(&p,10,true,0,-.8,-.6,1,0)); /* stale gap */
    assert(!p.armed);
    assert(!redwood_raise_sample(&p,10,true,0,-.8,-.6,1,0)); /* duplicate */
    assert(!redwood_raise_sample(&p,11,true,NAN,0,-1,0,0));
    puts("raise policy: pickup, display gate, stationary, face-down, shake and stale samples passed");
}

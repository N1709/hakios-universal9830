#include <assert.h>
#include <stdio.h>
#include "../../userland/src/brightness_policy.h"
static void near(double a,double b){if(fabs(a-b)>=.001)fprintf(stderr,"got %.3f expected %.3f\n",a,b);assert(fabs(a-b)<.001);}
int main(void){
 /* first reading after wake: held 1 s, then follows after 1.5 s of steady light */
 struct rb_policy p={0};rb_wake(&p,0,80);
 for(int i=0;i<15;i++)near(rb_observe(&p,i*.1,10),80);
 near(rb_observe(&p,1.6,10),30);
 /* A brief bright flash (under 0.5 s) must not alter the accepted target. */
 rb_observe(&p,6,1500);near(rb_observe(&p,6.4,1500),30);rb_observe(&p,6.5,10);near(p.target,30);
 /* big jump up (>=4x): 0.5 s */
 rb_observe(&p,8,1500);near(rb_observe(&p,8.4,1500),30);near(rb_observe(&p,8.6,1500),420);
 /* big drop: 1.5 s */
 rb_observe(&p,10,0);near(rb_observe(&p,11.4,0),420);near(rb_observe(&p,11.6,0),3);
 /* small dimming (<4x): 3 s */
 rb_observe(&p,20,150);near(rb_observe(&p,20.6,150),150);   /* (big up from 0) */
 rb_observe(&p,22,60);near(rb_observe(&p,24.9,60),150);near(rb_observe(&p,25.1,60),rb_base(60));
 /* Manual calibration survives similar light, releases across a large change, and is kept across wake. */
 rb_manual(&p,30,10,90);
 for(int i=0;i<80;i++)near(rb_observe(&p,30+i*.1,12),90);
 rb_observe(&p,40,1500);near(rb_observe(&p,40.6,1500),420);
 rb_wake(&p,50,200);near(rb_observe(&p,50,10),200);near(rb_observe(&p,51.6,10),90);
 /* a level chosen at 30 lux no longer pins a dark room (the old 20-lux band did) */
 rb_manual(&p,60,30,167);assert(rb_target(&p,3)<60);
 rb_manual(&p,70,10,420);near(rb_target(&p,1500),420);
 rb_manual(&p,80,150,3);near(rb_target(&p,0),3);
 /* slider curve: monotonic, 2..420, 50 % well under half the nits */
 near(rb_slider_nits(.025),2);near(rb_slider_nits(.975),420);assert(rb_slider_nits(.5)<100);
 double old=0;for(int i=0;i<=100000;i++){double n=rb_base(i/10.);assert(isfinite(n)&&n>=3&&n<=420&&n>=old);old=n;}
 old=0;for(int i=0;i<=1000;i++){double n=rb_slider_nits(i/1000.);assert(n>=old);old=n;}
 puts("brightness policy: debounce, fast path, spikes, manual anchor, wake, slider curve and bounds passed");
}

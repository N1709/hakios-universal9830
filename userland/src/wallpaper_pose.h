#ifndef REDWOOD_WALLPAPER_POSE_H
#define REDWOOD_WALLPAPER_POSE_H
#include <math.h>
#include <stdbool.h>
/* range: radians of tilt for the full amplitude (0 = .30); tau: seconds for the rest pose to
 * follow a held tilt, as iOS re-centres its parallax (0 = never) */
struct rw_wall_pose { bool ready; double timestamp,base_x,base_y,x,y,range,tau; };
static bool rw_wall_pose_update(struct rw_wall_pose *p,double t,double gx,double gy,double gz,double amplitude)
{
    if(!isfinite(t)||!isfinite(gx)||!isfinite(gy)||!isfinite(gz)||!isfinite(amplitude)||amplitude<0) return false;
    double length=sqrt(gx*gx+gy*gy+gz*gz);
    if(length<.8 || length>1.2 || (p->ready && t<=p->timestamp)) return false;
    gx/=length;gy/=length;gz/=length;
    double x=atan2(gx,hypot(gy,gz)),y=atan2(-gy,-gz);
    if(!p->ready || t-p->timestamp>1) {
        p->base_x=x;p->base_y=y;p->ready=true;p->x=p->y=0;
    }
    double dt=t-p->timestamp,range=p->range>0?p->range:.30;
    p->timestamp=t;
    if(p->tau>0 && dt>0) {
        double k=1-exp(-dt/p->tau);
        p->base_x+=(x-p->base_x)*k;p->base_y+=remainder(y-p->base_y,2*M_PI)*k;
    }
    p->x=-amplitude*fmax(-1,fmin(1,(x-p->base_x)/range));
    p->y=-amplitude*fmax(-1,fmin(1,remainder(y-p->base_y,2*M_PI)/range));
    return true;
}
#endif

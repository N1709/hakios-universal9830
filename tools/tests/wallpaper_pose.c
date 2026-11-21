#include <assert.h>
#include <stdio.h>
#include "../../userland/src/wallpaper_pose.h"
int main(void){
 struct rw_wall_pose p={0};
 assert(rw_wall_pose_update(&p,1,0,0,-1,10));assert(p.x==0&&p.y==0);
 assert(rw_wall_pose_update(&p,1.1,sin(.4),0,-cos(.4),10));assert(p.x==-10);
 assert(rw_wall_pose_update(&p,1.2,0,-sin(.4),-cos(.4),10));assert(p.y==-10);
 assert(rw_wall_pose_update(&p,1.3,0,-sin(.4),-cos(.4),10));assert(p.y==-10);
 assert(!rw_wall_pose_update(&p,1.3,0,0,-1,10));
 assert(!rw_wall_pose_update(&p,1.4,NAN,0,-1,10));
 assert(rw_wall_pose_update(&p,4,0,-sin(.4),-cos(.4),10));assert(fabs(p.x)<1e-9&&fabs(p.y)<1e-9);
 puts("wallpaper pose: baseline, tilt bounds, held pose, duplicates, invalid data and stale reset passed");
}

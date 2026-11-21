/* Exercise the generated production AF controller with a simulated scaled9-bit motor. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include "camera_isp.h"
static unsigned position=500,writes,max_command;
static unsigned limit=1022;
static unsigned scene;
static ssize_t test_write(int fd,const void *data,size_t bytes){
 (void)fd;const struct redwood_camera_command*c=data;assert(bytes==sizeof(*c)&&c->command==REDWOOD_CAMERA_FOCUS);
 assert(c->a<=limit);position=c->a&~1U;writes++;if(position>max_command)max_command=position;return bytes;
}
static void say(const char *format,...){(void)format;}
float isp_sharpness(const struct redwood_camera_frame *f,const uint8_t *raw,float x0,float y0,float x1,float y1,float *level){
 (void)f;(void)raw;(void)x0;(void)y0;(void)x1;(void)y1;*level=100;
 float delta=(float)position-(scene==2?1022:scene==3?0:200);
 if(scene==1){float a=(float)position-100,b=(float)position-860;return .65f/(1+a*a/1000)+1.2f/(1+b*b/1000);}
 return 1/(1+delta*delta/2500);
}
#define write test_write
#include "actual-af.h"
#undef write
int main(void){
 struct redwood_camera_frame f={.sensor=1,.focus=500,.gain=32,.gain_unity=32};
 struct af af={.state=AF_STEADY,.now=true,.sweep=true};float window[4]={.3,.3,.7,.7};
 for(unsigned n=0;n<300;n++){f.focus=position;af_frame(1,&af,&f,NULL,window,false);}
 assert(af.state==AF_WATCH&&abs((int)position-200)<=AF_FINE&&max_command<=1022);
 for(scene=1;scene<=3;scene++){
  position=500;af=(struct af){.state=AF_STEADY,.now=true,.sweep=true};max_command=0;
  for(unsigned n=0;n<300;n++){f.focus=position;af_frame(1,&af,&f,NULL,window,false);}
  unsigned expected=scene==1?860:scene==2?1022:0;
  assert(af.state==AF_WATCH&&abs((int)position-(int)expected)<=AF_FINE&&max_command==1022);
 }
 unsigned old_writes=writes;af.state=AF_SWEEP;f.focus=position;af_frame(1,&af,&f,NULL,window,true);
 assert(writes==old_writes&&af.state==AF_WATCH&&af.at==f.focus);
 f.focus=REDWOOD_CAMERA_NO_FOCUS;af_frame(1,&af,&f,NULL,window,true);assert(writes==old_writes);
 f.sensor=4;scene=2;position=500;af=(struct af){.state=AF_STEADY,.now=true,.sweep=true};
 for(unsigned n=0;n<300;n++){f.focus=position;af_frame(1,&af,&f,NULL,window,false);}
 assert(af.state==AF_WATCH&&abs((int)position-1022)<=AF_FINE);
 /* Front's existing controller range is unchanged by the primary-only patch. */
 af=(struct af){.at=250};limit=768;af_go(1,&af,700);assert(af.target==700);
 puts("PASS: primary scaled9-bit search converges, lock stops moves, unavailable motor ignored, front behaviour preserved");
}

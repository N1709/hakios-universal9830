#include "../mesa/air/air_metal.h"
#include <stdio.h>
int main(void){
 char why[512]={0};struct rwa_device*d=rwa_device_open(why,sizeof why);if(!d)goto fail;
 struct rwa_texture_shape s={.pixel_format=252,.type=2,.width=3,.height=2,.depth=1,.array_length=1,.mip_levels=1,.sample_count=1,.usage=5};
 struct rwa_texture*t=rwa_texture_create(d,&s,why,sizeof why);if(!t)goto fail;
 const float in[]={0,.125,.25,.5,.75,1};float out[6]={0};unsigned origin[3]={0},size[3]={3,2,1};
 if(rwa_texture_write(t,0,0,origin,size,in,0,why,sizeof why)||rwa_depth_read(t,0,0,origin,size,out,NULL,why,sizeof why))goto fail;
 for(unsigned i=0;i<6;i++)if(in[i]!=out[i]){snprintf(why,sizeof why,"depth %u got%g expected%g",i,out[i],in[i]);goto fail;}
 rwa_texture_destroy(t);rwa_device_close(d);puts("PASS: Depth32Float upload and aspect-aware readback, six independent values");return 0;
 fail:fprintf(stderr,"%s\n",why);return 1;
}

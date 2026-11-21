#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "camera_isp.h"
int main(void){
 for(unsigned scenario=0;scenario<4;scenario++){
  struct redwood_camera_frame f={.width=scenario==3?4032:2016,.height=scenario==3?3024:1512,
    .stride=scenario==3?5040:2528,.sensor=scenario==3?1:scenario+1,.bayer=1,.black=64};
  uint8_t *raw=calloc(f.stride,f.height);assert(raw);
  for(unsigned y=0;y<f.height;y++)for(unsigned x=0;x<f.width;x+=4){uint8_t*g=raw+(size_t)y*f.stride+(x/4)*5;
   for(unsigned k=0;k<4;k++){unsigned v=180+(x+k)/16+y/32+((y&1)*2+((x+k)&1))*13;g[k]=v>>2;g[4]|=(v&3)<<(2*k);}}
  struct isp isp;isp_init(&isp);struct isp_stats s;
  clock_t start=clock();for(unsigned n=0;n<40;n++)isp_measure(&isp,&f,raw,&s);
  printf("%u %u %.9g %.9g %.9g %.6f\n",f.sensor,s.cells,s.mean[0],s.mean[1],s.mean[2],(double)(clock()-start)/CLOCKS_PER_SEC);
  free(raw);
 }
}

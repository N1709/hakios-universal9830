/* GPU checks against Apple's documented RGB10A2Unorm bit layout.
 * https://developer.apple.com/documentation/metal/mtlpixelformat/rgb10a2unorm */
#include "../mesa/air/air_metal.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
int main(void) {
 char why[512]={0};struct rwa_device *d=rwa_device_open(why,sizeof why);
 if(!d){fprintf(stderr,"%s\n",why);return 1;}
 struct rwa_texture_shape shape={.pixel_format=90,.type=2,.width=7,.height=5,.depth=1,.array_length=1,.mip_levels=1,.sample_count=1,.usage=5};
 shape.usage=0; /* Unknown usage must permit render-target use too. */
 struct rwa_texture*t=rwa_texture_create(d,&shape,why,sizeof why);
 if(!t){fprintf(stderr,"%s\n",why);return 1;}
 const float colours[][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1},{1,1,1,1},{0,0,0,0},{1,0,1,1.0f/3}};
 const uint32_t words[]={1023,1023u<<10,1023u<<20,3u<<30,0xffffffff,0,1023u|(1023u<<20)|(1u<<30)};
 unsigned origin[3]={0,0,0},size[3]={7,5,1};uint32_t pixels[35];
 for(unsigned i=0;i<sizeof words/sizeof *words;i++){
  struct rwa_draw draw={.color=&t,.color_count=1,.clear_color=colours[i]};
  if(rwa_render_clear(d,&draw,why,sizeof why)||rwa_texture_read(t,0,0,origin,size,pixels,0,why,sizeof why)){fprintf(stderr,"%s\n",why);return 1;}
  for(unsigned j=0;j<35;j++)if(pixels[j]!=words[i]){fprintf(stderr,"case%u pixel%u got%08x expected%08x\n",i,j,pixels[j],words[i]);return 1;}
 }
 rwa_texture_destroy(t);
 shape.pixel_format=92;shape.usage=5;
 t=rwa_texture_create(d,&shape,why,sizeof why);if(!t){fprintf(stderr,"%s\n",why);return 1;}
 const uint32_t float_words[]={0x3c0,0x3c0u<<11,0x1e0u<<22,0,0x781e03c0,0,0x780003c0};
 for(unsigned i=0;i<7;i++){
  struct rwa_draw draw={.color=&t,.color_count=1,.clear_color=colours[i]};
  if(rwa_render_clear(d,&draw,why,sizeof why)||rwa_texture_read(t,0,0,origin,size,pixels,0,why,sizeof why)){fprintf(stderr,"%s\n",why);return 1;}
  for(unsigned j=0;j<35;j++)if(pixels[j]!=float_words[i]){fprintf(stderr,"float case%u got%08x expected%08x\n",i,pixels[j],float_words[i]);return 1;}
 }
 rwa_texture_destroy(t);rwa_device_close(d);puts("PASS: RGB10A2 / RG11B10 GPU clear and bit-layout readback, unknown-usage render target (490 pixels)");return 0;
}

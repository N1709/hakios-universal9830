#include "../mesa/air/air_metal.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static void *load(const char *path,size_t *size){FILE*f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);*size=ftell(f);rewind(f);void*p=malloc(*size);if(!p||fread(p,1,*size,f)!=*size){free(p);p=NULL;}fclose(f);return p;}
int main(int argc,char **argv){
 if(argc!=3)return 2;size_t vsz,fsz;void*v=load(argv[1],&vsz),*f=load(argv[2],&fsz);if(!v||!f)return 2;
 char why[512]={0};struct rwa_device*d=rwa_device_open(why,sizeof why);if(!d)goto fail;
 struct rwa_render_shape shape={.color_format={125},.color_count=1,.sample_count=1};
 shape.vertex_attribute[0].format=18;shape.vertex_attribute[0].buffer_index=0;
 shape.vertex_layout[0].stride=8;shape.vertex_layout[0].step_function=1;shape.vertex_layout[0].step_rate=1;
 struct rwa_render*r=rwa_render_create(d,v,vsz,"short_float",f,fsz,"constant_fragment",&shape,NULL,why,sizeof why);if(!r)goto fail;
 const int16_t vertices[][4]={{-1,-1,0,1},{1,-1,0,1},{-1,1,0,1},{-1,1,0,1},{1,-1,0,1},{1,1,0,1}};
 struct rwa_buffer*b=rwa_buffer_create(d,sizeof vertices,why,sizeof why);if(!b)goto fail;memcpy(rwa_buffer_map(b),vertices,sizeof vertices);
 struct rwa_texture_shape ts={.pixel_format=125,.type=2,.width=8,.height=8,.depth=1,.array_length=1,.mip_levels=1,.sample_count=1,.usage=4};
 struct rwa_texture*t=rwa_texture_create(d,&ts,why,sizeof why);if(!t)goto fail;
 const float clear[4]={0};struct rwa_vertex_stream stream={.buffer=b};
 struct rwa_draw draw={.color=&t,.color_count=1,.clear_color=clear,.primitive=3,.vertex_count=6,.instance_count=1,.vertex_streams=&stream,.vertex_stream_count=1};
 if(rwa_render_draw(r,&draw,why,sizeof why))goto fail;
 float pixels[8*8*4];unsigned origin[3]={0},size[3]={8,8,1};
 if(rwa_texture_read(t,0,0,origin,size,pixels,0,why,sizeof why))goto fail;
 for(unsigned i=0;i<8*8*4;i++)if(pixels[i]!=(float)(i%4+1)){snprintf(why,sizeof why,"pixel channel%u: got %g expected %u",i,pixels[i],i%4+1);goto fail;}
 rwa_texture_destroy(t);rwa_buffer_destroy(b);rwa_render_destroy(r);rwa_device_close(d);free(v);free(f);puts("PASS: GPU Short4 vertex data converted numerically to float4, full 64-pixel quad");return 0;
 fail:fprintf(stderr,"%s\n",why);return 1;
}

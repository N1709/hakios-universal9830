#include "../mesa/air/air_metal.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char**argv){
 if(argc!=2)return 2;FILE*f=fopen(argv[1],"rb");if(!f)return 2;fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);void*data=malloc(n);if(!data||fread(data,1,n,f)!=n)return 2;fclose(f);
 char why[512]={0};struct rwa_device*d=rwa_device_open(why,sizeof why);if(!d)goto fail;
 struct rwa_argbuf_layout layout[4];unsigned count=0;
 if(rwa_module_argbufs(d,data,n,"argbuf_set",layout,4,&count,why,sizeof why))goto fail;
 if(count!=1||layout[0].member_count!=5){snprintf(why,sizeof why,"Wrong argument layout");goto fail;}
 for(unsigned i=0;i<layout[0].member_count;i++)if(layout[0].members[i].kind==RWA_ARGBUF_BUFFER&&layout[0].members[i].index!=0){snprintf(why,sizeof why,"Flattened buffer did not reuse free slot0");goto fail;}
 rwa_device_close(d);free(data);puts("PASS: direct buffer31 and argument-buffer member reuse slot0 without collision");return 0;
 fail:fprintf(stderr,"%s\n",why);return 1;
}

/* Export only a complete RWCAM001 diagnostic snapshot to stdout. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
int main(void){
 size_t size=0;
 if(sysctlbyname("kern.redwood.camera_frame",NULL,&size,NULL,0)||size<4096||size>80U*1024*1024){fprintf(stderr,"no bounded diagnostic frame\n");return 1;}
 uint8_t *p=malloc(size);if(!p)return 1;
 if(sysctlbyname("kern.redwood.camera_frame",p,&size,NULL,0)){perror("camera_frame");free(p);return 1;}
 uint32_t h[8];memcpy(h,p+8,sizeof(h));
 uint64_t bytes=(uint64_t)h[2]*h[1];
 if(memcmp(p,"RWCAM001",8)||!h[0]||!h[1]||(h[0]&3)||h[3]!=1||h[5]!=1||h[2]!=(uint64_t)h[0]*5/4||h[6]!=bytes||size!=4096+bytes){fprintf(stderr,"incomplete or unsupported diagnostic frame\n");free(p);return 1;}
 int ok=fwrite(p,1,size,stdout)==size;free(p);return ok?0:1;
}

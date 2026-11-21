#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/redwood_camera_s20_gw2.h"
#include "../../xnu/files/bsd/dev/redwood_camera_s20_gw2_table.h"
static unsigned calls,fail,total_delay,burst_words,position;
static bool send(void *ctx,const uint8_t*b,unsigned n){
 (void)ctx;assert(n>=4&&n<=258&&!(n&1));calls++;
 if(fail&&calls==fail)return false;
 unsigned reg=(b[0]<<8)|b[1];
 if(reg==0x6f12){for(unsigned i=2;i<n;i+=2){
  while(position<sizeof(redwood_GW2_setfile_A_Tnp)/sizeof(uint32_t)&&redwood_GW2_setfile_A_Tnp[position]!=0xf0000002U)position+=3;
  assert(position<sizeof(redwood_GW2_setfile_A_Tnp)/sizeof(uint32_t));
  assert(((unsigned)b[i]<<8|b[i+1])==redwood_GW2_setfile_A_Tnp[position+1]);position+=3;burst_words++;
 }}else assert(n==4);
 return true;
}
static void pause_us(void*ctx,unsigned n){(void)ctx;total_delay+=n;}
static bool accept(void*ctx,const uint8_t*b,unsigned n){(void)ctx;(void)b;(void)n;calls++;return !fail||calls!=fail;}
int main(void){
 struct cam_gw2_table_io io={NULL,send,pause_us};
 assert(cam_gw2_apply_table(io,redwood_GW2_setfile_A_Tnp,sizeof(redwood_GW2_setfile_A_Tnp)/4));
 assert(burst_words>4000&&total_delay==11000);unsigned operations=calls;
 io.send=accept;
 for(unsigned i=1;i<=operations;i++){calls=0;fail=i;assert(!cam_gw2_apply_table(io,redwood_GW2_setfile_A_Tnp,sizeof(redwood_GW2_setfile_A_Tnp)/4));assert(calls==i);}
 fail=0;calls=0;
 uint32_t invalid[]={0xfcfc,0x4000,2,0xf0000002,1,2};
 assert(!cam_gw2_apply_table(io,invalid,6)&&calls==0);
 assert(!cam_gw2_apply_table(io,invalid,5)&&calls==0);
 assert(cam_gw2_apply_table(io,redwood_GW2_setfile_A_Global,sizeof(redwood_GW2_setfile_A_Global)/4));
 assert(cam_gw2_apply_table(io,redwood_gw2_setfile_A_2432x1824_30fps,sizeof(redwood_gw2_setfile_A_2432x1824_30fps)/4));
 puts("PASS: GW2 firmware burst byte order/content/chunk boundaries, delay, prevalidation and every failed-transfer exit");
}

/* Compile the ACTUAL receiver implementation with a simulated MMIO/allocator.
 * Verify captured-frame gating, pre-reset snapshots and no-free-on-abort-timeout. */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uintptr_t vm_offset_t;typedef size_t vm_size_t;
#define CMU_TOP_PHYS 0x1a330000U
#define CMU_CSIS_PHYS 0x17000000U
#define PMU_PHYS 0x15860000U
#define ISPPRE_SYSREG_PHYS 0x17020500U
#define SMC_PREPARE_PD_ONOFF 0x82000410ULL
#define CSIS_NEED_SMC 0x17010204ULL
#define NSEC_PER_USEC 1000
static void *cam_s20_io_map(vm_offset_t,vm_size_t);
static uint32_t cam_s20_rd(volatile uint8_t *,uint32_t);
static void cam_s20_wr(volatile uint8_t *,uint32_t,uint32_t);
static int redwood_spmic_read(uint8_t,uint8_t,uint8_t*);
static uint64_t cam_s20_smc4(uint64_t,uint64_t,uint64_t,uint64_t);
static bool redwood_camera_dma_get(uint64_t,uint64_t*,void**);
static void redwood_camera_dma_put(void);
static void *redwood_camera_dma_cached(void);
static bool cam_s20_save_frame_named(const volatile uint16_t*,uint32_t,uint32_t,bool,const char*);
static void delay_for_interval(unsigned,unsigned);
static void cam_s20_wait_ms(unsigned);
/* Kernel-only barriers are tested by hardware; replace only those instructions. */
#define __asm__(...) ((void)0)
#include "../../xnu/files/bsd/dev/redwood_camera_s20_aux_rx_impl.h"
#undef __asm__
static bool testing_gw2;
static uint32_t regs[AX_REGIONS][0x8000/4];
static bool abort_fail,bad_phy_readback;static unsigned releases,exports;static void *allocation;
static const uint32_t physical[]={CMU_TOP_PHYS,CMU_CSIS_PHYS,PMU_PHYS,ISPPRE_SYSREG_PHYS,0x17070000,0x170d2300,0x170d1000,0x170c0000,0x17110000,0x17140000,0x17020414};
static void *cam_s20_io_map(vm_offset_t p,vm_size_t n){(void)n;
 if(p==0x17050000){testing_gw2=true;return regs[AX_CSI];}
 if(p==0x170d1b00)return regs[AX_PHY];if(p==0x17020420)return regs[AX_MUX];for(unsigned i=0;i<AX_REGIONS;i++)if(p==physical[i])return regs[i];return NULL;}
static uint32_t cam_s20_rd(volatile uint8_t *p,uint32_t off){uint32_t v=*(volatile uint32_t *)(p+off);
 if(testing_gw2&&p==(volatile uint8_t*)regs[AX_DMA]&&off>=0x4050&&off<0x4090)return 0xdeadc0deU;
 if(p==(volatile uint8_t *)regs[AX_CSI]&&off==0x1c){
  v&=~7U;if(regs[AX_CSI][0x24/4])v|=4;if(regs[AX_CSI][0x2c/4])v|=2;if(regs[AX_CSI][0x634/4])v|=1;
 }
 if(p==(volatile uint8_t *)regs[AX_CSI]&&off==4&&(v&2U)){v&=~2U;*(volatile uint32_t *)(p+off)=v;}
 if(bad_phy_readback&&p==(volatile uint8_t *)regs[AX_PHY]&&off==0x104&&v==0x1450){bad_phy_readback=false;return v^1;}
 return v;}
static void cam_s20_wr(volatile uint8_t *p,uint32_t off,uint32_t v){
 if(p==(volatile uint8_t *)regs[AX_CSI]&&(off==0x14||off==0x1c||off==0x24||off==0x2c||off==0x634)){*(volatile uint32_t *)(p+off)&=~v;return;}
 if(p==(volatile uint8_t *)regs[AX_DMA]&&off==(testing_gw2?0x4408:0x1408)){regs[AX_DMA][off/4]&=~v;return;}
 *(volatile uint32_t *)(p+off)=v;
 if(p==(volatile uint8_t *)regs[AX_SYS]&&off==0&&!(v&(testing_gw2?2U:4U)))memset(regs[AX_PHY],0,sizeof(regs[AX_PHY]));
 if(p==(volatile uint8_t *)regs[AX_CSI]&&off==4&&(v&2U)){
  /* Real reset can wipe ALL writable VC fields. Their pre-reset values must survive rollback. */
  for(unsigned vc=0;vc<4;vc++){regs[AX_CSI][(0x40+vc*16)/4]=0;regs[AX_CSI][(0x44+vc*16)/4]=0;}
 }
 if(p==(volatile uint8_t *)regs[AX_DMA]&&off==(testing_gw2?0x4000:0x1000)&&(v&1))regs[AX_DMA][(testing_gw2?0x4090:0x1090)/4]=1;
 if(p==(volatile uint8_t *)regs[AX_DMA]&&off==(testing_gw2?0x4414:0x1414)&&(v&1)&&!abort_fail){regs[AX_DMA][(testing_gw2?0x4408:0x1408)/4]|=1U<<16;regs[AX_DMA][(testing_gw2?0x4090:0x1090)/4]=0;}
}
static int redwood_spmic_read(uint8_t a,uint8_t b,uint8_t*v){(void)a;(void)b;*v=0xc0;return 0;}
static uint64_t cam_s20_smc4(uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)a;(void)b;(void)c;(void)d;return 0;}
static bool redwood_camera_dma_get(uint64_t n,uint64_t*p,void**v){allocation=malloc(n);*v=allocation;*p=0x90000000;return allocation!=NULL;}
static void *redwood_camera_dma_cached(void){return NULL;}
static void redwood_camera_dma_put(void){releases++;free(allocation);allocation=NULL;}
static bool cam_s20_save_frame_named(const volatile uint16_t*p,uint32_t w,uint32_t h,bool optical,const char*name){assert(p[0]==256&&(testing_gw2?((w==2432&&h==1824)||(w==4864&&h==3648)):(w==4032&&h==3024))&&optical&&!strcmp(name,testing_gw2?"S5KGW2":"S5K2LA"));exports++;return true;}
static void delay_for_interval(unsigned a,unsigned b){(void)a;(void)b;}
static void cam_s20_wait_ms(unsigned a){(void)a;}
static void initial(void){testing_gw2=false;memset(regs,0,sizeof(regs));abort_fail=bad_phy_readback=false;releases=exports=0;allocation=NULL;
 regs[AX_PMU][0x1d84/4]=1;regs[AX_MMU0][0x34/4]=regs[AX_MMU1][0x34/4]=7U<<28;
 const unsigned g[]={0x20e4,0x20e8,0x20ec,0x20f0};for(unsigned i=0;i<4;i++)regs[AX_CMU][g[i]/4]=1U<<21;
 regs[AX_SYS][0]=4;regs[AX_PHY][0x104/4]=0x777;regs[AX_BIAS][0]=0x99;
 regs[AX_CSI][0x40/4]=0x3456;regs[AX_CSI][0x44/4]=0x11223344;
 regs[AX_TOP][0x104c/4]=2;regs[AX_TOP][0x1848/4]=5;
}
static void frame(struct ax_receiver*x){uint16_t*p=(uint16_t*)((uint8_t*)x->buffer+16384);for(unsigned i=0;i<x->bytes/2;i++)p[i]=256;regs[AX_DMA][(testing_gw2?0x4408:0x1408)/4]|=1U<<8;regs[AX_CSI][0x500/4]=12;regs[AX_CSI][0x24/4]=1;regs[AX_CSI][0x2c/4]=1;regs[AX_CSI][0x634/4]=1;}
static void packed_test(void){
 initial();struct ax_receiver x={0};assert(ax_receiver_begin_mode(&x,true));
 assert(x.bytes==15240960&&x.total>=2*x.bytes+3*16384);
 assert((regs[AX_DMA][0x1004/4]&0x7f)==7);
 assert(regs[AX_DMA][0x1014/4]-regs[AX_DMA][0x1010/4]==x.bytes+16384);
 bool failed=true;assert(ax_receiver_next(&x,&failed)==NULL&&!failed);
 regs[AX_DMA][0x1408/4]=0x110;regs[AX_DMA][0x1090/4]=5;
 assert(ax_receiver_next(&x,&failed)==(uint8_t*)x.buffer+16384&&!failed);
 regs[AX_DMA][0x1408/4]=0x100;regs[AX_DMA][0x1090/4]=5;
 assert(ax_receiver_next(&x,&failed)==(uint8_t*)x.buffer+32768+x.bytes&&!failed);
 regs[AX_DMA][0x1408/4]=0x1000;assert(!ax_receiver_next(&x,&failed)&&failed);
 regs[AX_DMA][0x1408/4]=0;assert(ax_receiver_end(&x));assert(releases==1&&exports==0);
 initial();x=(struct ax_receiver){0};assert(ax_receiver_begin_mode(&x,true));
 ((uint8_t*)x.buffer)[16384+x.bytes]=0;assert(!ax_receiver_end(&x));assert(x.guard_bad==1&&releases==1);
 initial();x=(struct ax_receiver){0};assert(ax_receiver_begin_mode(&x,true));abort_fail=true;
 assert(!ax_receiver_end(&x)&&x.retained&&releases==0);free(allocation);allocation=NULL;
}
static void gw2_test(void){
 initial();struct ax_receiver x={0};assert(ax_receiver_begin_sensor(&x,false,true));
 assert(x.width==2432&&x.height==1824&&x.bytes==2432*1824*2);
 assert(regs[AX_CMU][0x3068/4]==2&&regs[AX_CMU][0x3070/4]==0);
 assert((regs[AX_PHY][0x130/4]&0xff)==1);
 assert(regs[AX_DMA][0x1000/4]==0&&regs[AX_DMA][0x4000/4]==3);
 assert(regs[AX_DMA][0x40b0/4]==0xffff);assert(regs[AX_DMA][0x4050/4]==0);assert(regs[AX_CSI][0x54/4]==((904U<<16)|608U));assert(regs[AX_MUX][0]==2);assert(regs[AX_SYS][0]==6);frame(&x);assert(ax_receiver_end(&x)&&x.exported&&releases==1);
 assert(regs[AX_SYS][0]==4&&regs[AX_CMU][0x3068/4]==0);
 initial();x=(struct ax_receiver){0};assert(ax_receiver_begin_sensor(&x,true,true));
 bool failed=true;regs[AX_DMA][0x4408/4]=0x100;regs[AX_DMA][0x4090/4]=4;
 assert(ax_receiver_next(&x,&failed)==(uint8_t*)x.buffer+32768+x.bytes&&!failed);
 assert(ax_receiver_end(&x)&&releases==1);
}
static void gw2_full_test(void){
 initial();struct ax_receiver x={0};assert(ax_receiver_begin_sensor(&x,true,2));
 assert(x.width==4864&&x.height==3648&&x.bytes==6080*3648&&x.total<48U*1024*1024);
 assert(ax_receiver_end(&x)&&releases==1);
 initial();x=(struct ax_receiver){0};assert(ax_receiver_begin_sensor(&x,false,2));frame(&x);
 assert(ax_receiver_end(&x)&&x.exported&&releases==1);
}
int main(void){gw2_full_test();gw2_test();packed_test();struct ax_receiver x={0};initial();assert(ax_receiver_begin(&x));frame(&x);assert(ax_receiver_end(&x));assert(x.exported&&exports==1&&releases==1&&!x.retained);
 assert(x.frames==12&&cam_s20_rd((volatile uint8_t*)regs[AX_CSI],0x1c)==0);
 struct ax_receiver repeated={0};assert(ax_receiver_begin(&repeated));frame(&repeated);assert(ax_receiver_end(&repeated));assert(repeated.exported&&releases==2);
 assert(regs[AX_CSI][0x40/4]==0x3456&&regs[AX_CSI][0x44/4]==0x11223344);assert(regs[AX_PHY][0x104/4]==0x777&&regs[AX_SYS][0]==4&&regs[AX_BIAS][0]==0x99);assert(regs[AX_TOP][0x104c/4]==2&&regs[AX_TOP][0x1848/4]==5);
 initial();x=(struct ax_receiver){0};assert(ax_receiver_begin(&x));frame(&x);((uint8_t*)x.buffer)[0]=0;assert(ax_receiver_end(&x));assert(!x.exported&&x.guard_bad==1&&releases==1);
 initial();x=(struct ax_receiver){0};assert(ax_receiver_begin(&x));frame(&x);((uint16_t*)((uint8_t*)x.buffer+16384))[1]=4095;assert(ax_receiver_end(&x));assert(!x.exported&&x.pixel_max==4095&&releases==1);
 initial();x=(struct ax_receiver){0};assert(ax_receiver_begin(&x));frame(&x);regs[AX_CSI][0x14/4]=1;assert(ax_receiver_end(&x));assert(!x.exported&&releases==1);
 initial();x=(struct ax_receiver){0};assert(ax_receiver_begin(&x));frame(&x);abort_fail=true;assert(!ax_receiver_end(&x));assert(x.retained&&releases==0&&allocation!=NULL);free(allocation);allocation=NULL;
 initial();x=(struct ax_receiver){0};bad_phy_readback=true;assert(!ax_receiver_begin(&x));assert(ax_receiver_end(&x));assert(releases==1&&!x.armed);assert(regs[AX_CSI][0x44/4]==0x11223344);
 initial();x=(struct ax_receiver){0};regs[AX_MMU0][0]=1;assert(!ax_receiver_begin(&x));assert(ax_receiver_end(&x));assert(releases==0&&!allocation);
 puts("PASS: actual receiver export/guards, pre-reset rollback, abort-timeout retention and live-MMU refusal");}

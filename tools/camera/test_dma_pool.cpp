#include <cassert>
#include <cstdint>
#include <cstdio>
#define POOL_SLOT (14U<<20)
static uint64_t gPoolBase=0xd4000000;
static unsigned gPoolSlots=13;static volatile uint32_t gPoolUsed;
static bool failDescriptor,failMap;static unsigned objects;
static int lock,depth;static int *gSurfaceLock=&lock;
static void IOLockLock(int*){assert(depth++==0);}
static void IOLockUnlock(int*){assert(--depth==0);}
static bool OSCompareAndSwap(uint32_t old,uint32_t value,volatile uint32_t*p){if(*p!=old)return false;*p=value;return true;}
static int poolTake(uint64_t,uint64_t*address){for(unsigned i=0;i<gPoolSlots;i++)if(!(gPoolUsed&(1U<<i))){gPoolUsed|=1U<<i;*address=gPoolBase+i*(uint64_t)POOL_SLOT;return (int)i;}return -1;}
static void poolGive(int n){if(n<0)return;assert((unsigned)n<gPoolSlots);gPoolUsed&=~(1U<<n);}
static const int kernel_task=0,kIODirectionInOut=0,kIOMapAnywhere=1,kIOMapWriteCombineCache=2;
struct IOMemoryMap{IOMemoryMap(){objects++;}uintptr_t getVirtualAddress(){return 0x1234;}void release(){objects--;delete this;}};
struct IOMemoryDescriptor{
 IOMemoryDescriptor(){objects++;}
 static IOMemoryDescriptor*withPhysicalAddress(uint64_t,uint64_t,int){return failDescriptor?nullptr:new IOMemoryDescriptor;}
 IOMemoryMap*createMappingInTask(int,int,int){return failMap?nullptr:new IOMemoryMap;}
 void release(){objects--;delete this;}
};
#define OSSafeReleaseNULL(p) do{if(p){p->release();p=nullptr;}}while(0)
#include "actual-pool.h"
int main(){uint64_t physical;void*virt;
 for(unsigned slots=1;slots<=4;slots++){
  gPoolUsed=1;uint64_t bytes=(slots-1)*(uint64_t)POOL_SLOT+1;
  assert(redwood_camera_dma_get(bytes,&physical,&virt));assert(physical==gPoolBase+POOL_SLOT&&virt==(void*)0x1234);
  assert(gPoolUsed==((1U<<(slots+1))-1));assert(!redwood_camera_dma_get(1,&physical,&virt));
  gCameraPoolCached=new IOMemoryMap;redwood_camera_dma_put();assert(gPoolUsed==1&&objects==0&&gCameraPoolSlot4==-1);
 }
 for(unsigned failure=0;failure<2;failure++){
  failDescriptor=failure==0;failMap=failure==1;gPoolUsed=1;
  assert(!redwood_camera_dma_get(44425216,&physical,&virt));assert(gPoolUsed==1&&objects==0&&gCameraPoolSlot4==-1);
 }failDescriptor=failMap=false;
 gPoolUsed=(1U<<gPoolSlots)-1;assert(!redwood_camera_dma_get(44425216,&physical,&virt));assert(gPoolUsed==(1U<<gPoolSlots)-1);
 gPoolUsed=0x1555;uint32_t before=gPoolUsed;assert(!redwood_camera_dma_get(44425216,&physical,&virt)&&gPoolUsed==before);
 gPoolUsed=1;gPoolBase=0xffff0000;assert(!redwood_camera_dma_get(44425216,&physical,&virt)&&gPoolUsed==1&&objects==0);
 assert(!redwood_camera_dma_get(4ULL*POOL_SLOT+1,&physical,&virt));assert(!redwood_camera_dma_get(0,&physical,&virt));
 assert(!depth);puts("PASS: actual camera allocator1..4slots, busy/full/fragmented pool, map/descriptor failures,4GB bound and complete release");
}

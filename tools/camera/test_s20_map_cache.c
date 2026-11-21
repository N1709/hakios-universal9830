#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../xnu/files/bsd/dev/redwood_camera_s20_map_cache.h"
static unsigned allocations;static int fail;
static uintptr_t map(void *context,uint64_t physical,uint64_t bytes){(void)context;(void)bytes;allocations++;return fail?0:(uintptr_t)(physical+0x100000000ULL);}
int main(void){
 struct s20_map_cache cache={0};uintptr_t first=s20_map_get(&cache,0x17070000,0x1000,map,NULL);assert(first&&allocations==1);
 for(unsigned i=0;i<10000;i++){assert(s20_map_get(&cache,0x17070000,0x1000,map,NULL)==first);assert(s20_map_get(&cache,0x17070400,0x100,map,NULL)==first+0x400);}
 assert(allocations==1&&cache.count==1);
 fail=1;assert(!s20_map_get(&cache,0x170c0000,0x5000,map,NULL)&&cache.count==1);fail=0;
 assert(s20_map_get(&cache,0x170c0000,0x5000,map,NULL)&&cache.count==2);
 unsigned before=allocations;assert(!s20_map_get(&cache,UINT64_MAX-3,8,map,NULL));assert(!s20_map_get(&cache,1,0,map,NULL));assert(allocations==before);
 for(unsigned i=cache.count;i<S20_MAP_CAPACITY;i++)assert(s20_map_get(&cache,0x20000000+i*0x10000,0x1000,map,NULL));
 before=allocations;assert(!s20_map_get(&cache,0x30000000,0x1000,map,NULL));assert(allocations==before);
 assert(s20_map_get(&cache,0x17070000,0x1000,map,NULL)==first);
 puts("PASS: 10000 repeated opens reuse one mapping; subranges, retry, overflow and bounded exhaustion");
}

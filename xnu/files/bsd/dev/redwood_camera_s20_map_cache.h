/* Fixed, caller-serialized cache for permanent ml_io_map mappings.
 * Restarting a camera must not consume another io_submap allocation. */
#ifndef REDWOOD_CAMERA_S20_MAP_CACHE_H
#define REDWOOD_CAMERA_S20_MAP_CACHE_H
#define S20_MAP_CAPACITY 64
struct s20_map_entry {uint64_t physical,bytes;uintptr_t virtual;};
struct s20_map_cache {struct s20_map_entry entries[S20_MAP_CAPACITY];unsigned count;};
static uintptr_t s20_map_get(struct s20_map_cache *cache,uint64_t physical,uint64_t bytes,
    uintptr_t (*map)(void *,uint64_t,uint64_t),void *context) {
    if(!bytes||physical>UINT64_MAX-bytes)return 0;
    for(unsigned i=0;i<cache->count;i++){
        struct s20_map_entry *entry=&cache->entries[i];
        if(physical>=entry->physical&&physical-entry->physical<=entry->bytes&&
           bytes<=entry->bytes-(physical-entry->physical))return entry->virtual+(uintptr_t)(physical-entry->physical);
    }
    if(cache->count==S20_MAP_CAPACITY)return 0;
    uintptr_t address=map(context,physical,bytes);
    if(address)cache->entries[cache->count++]=(struct s20_map_entry){physical,bytes,address};
    return address;
}
#endif

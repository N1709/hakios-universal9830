#ifndef REDWOOD_GRAPHICS_POLICY_H
#define REDWOOD_GRAPHICS_POLICY_H
#ifndef __cplusplus
#include <stdbool.h>
#endif
/* These core services retain their established renderer. Ordinary and newly
 * installed applications require no per-app enable rule. proc_selfname may
 * return the kernel's 15-character comm alias for long executable names. */
static inline bool rw_graphics_legacy_process(const char *name)
{
    if (!name) return false;
    const char *const legacy[]={"backboardd","SpringBoard","mediaserverd",
        "PosterBoard","CollectionsPoster","CollectionsPost","WeatherPoster",
        "SBRendererService","SBRendererServic", "MobileSlideShow"};
    for(unsigned i=0;i<sizeof legacy/sizeof *legacy;i++)
        if(!__builtin_strcmp(name,legacy[i]))return true;
    return false;
}
static inline bool rw_graphics_discovery_key(const char *key)
{
    if (!key) return false;
    const char *const keys[]={"MetalPluginName","MetalPluginClassName",
        "IOGLESBundleName","IOGLESMetalBundleName","IOGLESDefaultUseMetal"};
    for(unsigned i=0;i<sizeof keys/sizeof *keys;i++)
        if(!__builtin_strcmp(key,keys[i]))return true;
    return false;
}
#endif

#include "../../xnu/files/iokit/Kernel/redwood_graphics_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 const char *legacy[]={"backboardd","SpringBoard","mediaserverd","PosterBoard","CollectionsPoster","CollectionsPost","WeatherPoster","SBRendererService","SBRendererServic","MobileSlideShow"};
 for(unsigned i=0;i<sizeof legacy/sizeof *legacy;i++)assert(rw_graphics_legacy_process(legacy[i]));
 const char *normal[]={"Maps","maps","FindMy","Camera","backboardd-helper","",NULL};
 for(unsigned i=0;i<sizeof normal/sizeof *normal;i++)assert(!rw_graphics_legacy_process(normal[i]));
 const char *keys[]={"MetalPluginName","MetalPluginClassName","IOGLESBundleName","IOGLESMetalBundleName","IOGLESDefaultUseMetal"};
 for(unsigned i=0;i<5;i++)assert(rw_graphics_discovery_key(keys[i]));
 assert(!rw_graphics_discovery_key("RedwoodGraphicsPolicyVersion"));
 assert(!rw_graphics_discovery_key("MetalPluginNameExtra"));
 assert(!rw_graphics_discovery_key(NULL));
 puts("PASS: exact core/Photos compatibility policy, normal apps and unrelated registry properties");
}

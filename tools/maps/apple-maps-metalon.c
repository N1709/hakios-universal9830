/* Reviewed Apple Maps-only runtime helper. Inject only into process Maps.
 * Existing metalon implementation with a dedicated paired test-bundle path. */
/* libredwood-metalon: Metal for the processes named in kern.exec_inject only.
 * Metal finds a driver through two properties of the IOAcceleratorES service; system-wide
 * (metal_props on) they made every new mediaserverd abort. Here the two are answered inside this
 * process alone, by interposing the IORegistry property reads, so Metal loads
 * /System/Library/Extensions/RedwoodMetal.bundle and MTLCreateSystemDefaultDevice() has a device. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
static CFTypeRef (*o_create)(io_registry_entry_t,CFStringRef,CFAllocatorRef,IOOptionBits);
static CFTypeRef (*o_search)(io_registry_entry_t,const io_name_t,CFStringRef,CFAllocatorRef,IOOptionBits);
static kern_return_t (*o_props)(io_registry_entry_t,CFMutableDictionaryRef*,CFAllocatorRef,IOOptionBits);
/* A driver under test in /var/jb/Library/Extensions wins over the system one: Metal builds
 * /System/Library/Extensions/<name>.bundle, so the name climbs out of there. */
#include <unistd.h>
static CFStringRef plugin_name(void)
{
    static CFStringRef n;
    if(!n) n=access("/var/jb/Library/Extensions/RedwoodMapsMetal.bundle/RedwoodMetal",R_OK)==0
        ? CFSTR("../../../../var/jb/Library/Extensions/RedwoodMapsMetal") : CFSTR("RedwoodMetal");
    return n;
}
static CFTypeRef answer(CFStringRef key)
{
    if(!key || CFGetTypeID(key)!=CFStringGetTypeID()) return NULL;
    if(CFEqual(key,CFSTR("MetalPluginName"))) return CFRetain(plugin_name());
    if(CFEqual(key,CFSTR("MetalPluginClassName"))) return CFRetain(CFSTR("RedwoodMetalDevice"));
    return NULL;
}
static bool is_accel(io_registry_entry_t e)
{
    return IOObjectConformsTo(e,"IOAcceleratorES") || IOObjectConformsTo(e,"IOAccelerator");
}
static CFTypeRef my_create(io_registry_entry_t e,CFStringRef key,CFAllocatorRef a,IOOptionBits o)
{
    CFTypeRef r=o_create(e,key,a,o);
    if(!r && is_accel(e)) r=answer(key);
    return r;
}
static CFTypeRef my_search(io_registry_entry_t e,const io_name_t plane,CFStringRef key,CFAllocatorRef a,IOOptionBits o)
{
    CFTypeRef r=o_search(e,plane,key,a,o);
    if(!r && is_accel(e)) r=answer(key);
    return r;
}
static kern_return_t my_props(io_registry_entry_t e,CFMutableDictionaryRef *p,CFAllocatorRef a,IOOptionBits o)
{
    kern_return_t k=o_props(e,p,a,o);
    if(k==KERN_SUCCESS && p && *p && is_accel(e)) {
        if(!CFDictionaryContainsKey(*p,CFSTR("MetalPluginName"))) CFDictionarySetValue(*p,CFSTR("MetalPluginName"),plugin_name());
        if(!CFDictionaryContainsKey(*p,CFSTR("MetalPluginClassName"))) CFDictionarySetValue(*p,CFSTR("MetalPluginClassName"),CFSTR("RedwoodMetalDevice"));
    }
    return k;
}
__attribute__((constructor)) static void start(void)
{
    void *h=dlopen("/var/jb/usr/lib/libsubstrate.dylib",RTLD_NOW);
    void (*hook)(void*,void*,void**)=h?(void(*)(void*,void*,void**))dlsym(h,"MSHookFunction"):NULL;
    if(!hook) { fprintf(stderr,"redwood: metalon: no MSHookFunction (%s)\n",dlerror()); return; }
    hook((void*)IORegistryEntryCreateCFProperty,(void*)my_create,(void**)&o_create);
    hook((void*)IORegistryEntrySearchCFProperty,(void*)my_search,(void**)&o_search);
    hook((void*)IORegistryEntryCreateCFProperties,(void*)my_props,(void**)&o_props);
    /* MPS has no kernels for our GPU: make them nil instead of an abort (as inject_sb.c does), so
     * CoreImage falls back to its own Metal kernels. */
    void *metal=dlopen("/System/Library/Frameworks/Metal.framework/Metal",RTLD_NOW|RTLD_GLOBAL);
    void (*set_mode)(uint64_t,uint32_t)=metal?(void(*)(uint64_t,uint32_t))dlsym(metal,"MTLFailureTypeSetErrorModeType"):NULL;
    if(set_mode) for(uint64_t t=0;t<5;t++) set_mode(t,4);
    fprintf(stderr,"redwood: metalon: Metal on in %s (MPS failures %s)\n",getprogname(),set_mode?"nil":"still abort");
}

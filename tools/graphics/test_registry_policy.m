/* Run without any injected library. The executable basename selects only the
 * kernel compatibility rule under test; it never starts/stops a real service. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) { @autoreleasepool {
 if(argc!=2 || (strcmp(argv[1],"legacy") && strcmp(argv[1],"normal")))return 2;
 BOOL legacy=!strcmp(argv[1],"legacy");
 io_service_t service=IOServiceGetMatchingService(MACH_PORT_NULL,IOServiceMatching("IOAcceleratorES"));
 if(!service)return 1;
 CFTypeRef marker=IORegistryEntryCreateCFProperty(service,CFSTR("RedwoodGraphicsPolicyVersion"),NULL,0);
 int version=0;if(marker && CFGetTypeID(marker)==CFNumberGetTypeID())CFNumberGetValue(marker,kCFNumberIntType,&version);
 if(marker)CFRelease(marker);if(version!=1){fprintf(stderr,"Kernel policy missing\n");return 1;}
 CFMutableDictionaryRef props=NULL;
 if(IORegistryEntryCreateCFProperties(service,&props,NULL,0)||!props)return 1;
 CFStringRef keys[]={CFSTR("MetalPluginName"),CFSTR("MetalPluginClassName"),CFSTR("IOGLESBundleName"),CFSTR("IOGLESMetalBundleName"),CFSTR("IOGLESDefaultUseMetal")};
 for(unsigned i=0;i<5;i++){
  CFTypeRef value=IORegistryEntryCreateCFProperty(service,keys[i],NULL,0);
  CFTypeRef search=IORegistryEntrySearchCFProperty(service,kIOServicePlane,keys[i],NULL,0);
  BOOL found=value&&search&&CFDictionaryContainsKey(props,keys[i]);
  BOOL absent=!value&&!search&&!CFDictionaryContainsKey(props,keys[i]);
  if(value)CFRelease(value);if(search)CFRelease(search);
  if(!(legacy?absent:found)){fprintf(stderr,"Wrong visibility for key %u\n",i);return 1;}
 }
 if(!CFDictionaryContainsKey(props,CFSTR("RedwoodGraphicsPolicyVersion")))return 1;
 CFRelease(props);IOObjectRelease(service);
 id<MTLDevice> device=MTLCreateSystemDefaultDevice();
 if((device==nil)!=legacy){fprintf(stderr,"Wrong Metal discovery result\n");return 1;}
 printf("PASS: %s discovery, all three registry APIs, unrelated metadata and Metal device\n",legacy?"legacy":"normal");
 return 0;
}}

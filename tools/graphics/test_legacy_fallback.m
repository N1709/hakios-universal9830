#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#import <IOKit/IOKitLib.h>
int main(void){@autoreleasepool{
 if(MTLCreateSystemDefaultDevice()!=nil){fprintf(stderr,"Legacy process still discovers Metal\n");return 1;}
 io_service_t service=IOServiceGetMatchingService(MACH_PORT_NULL,IOServiceMatching("IOAcceleratorES"));if(!service)return 1;
 CFStringRef keys[]={CFSTR("MetalPluginName"),CFSTR("MetalPluginClassName"),CFSTR("IOGLESBundleName"),CFSTR("IOGLESMetalBundleName"),CFSTR("IOGLESDefaultUseMetal")};
 CFMutableDictionaryRef props=NULL;if(IORegistryEntryCreateCFProperties(service,&props,kCFAllocatorDefault,0)||!props||!CFDictionaryGetCount(props))return 1;
 for(unsigned i=0;i<5;i++){
  CFTypeRef value=IORegistryEntryCreateCFProperty(service,keys[i],kCFAllocatorDefault,0);
  CFTypeRef search=IORegistryEntrySearchCFProperty(service,kIOServicePlane,keys[i],kCFAllocatorDefault,0);
  if(value||search||CFDictionaryContainsKey(props,keys[i])){fprintf(stderr,"Legacy discovery property %u still visible\n",i);return 1;}
 }
 CFRelease(props);IOObjectRelease(service);
 puts("PASS: legacy service sees no Metal/GLES discovery through all three property APIs while other metadata survives");return 0;
}}

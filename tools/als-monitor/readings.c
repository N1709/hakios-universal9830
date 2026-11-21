#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
typedef void *Client;
typedef void *Service;
extern Client IOHIDEventSystemClientCreateWithType(CFAllocatorRef,int,CFDictionaryRef);
extern CFArrayRef IOHIDEventSystemClientCopyServices(Client);
extern CFTypeRef IOHIDServiceClientCopyProperty(Service,CFStringRef);
extern CFTypeRef IOHIDServiceClientCopyEvent(Service,int,CFTypeRef,int);
extern double IOHIDEventGetFloatValue(CFTypeRef,int);
extern uint64_t IOHIDEventGetTimeStamp(CFTypeRef);
static CFTypeRef property(id c,CFStringRef k){return ((CFTypeRef(*)(id,SEL,CFStringRef))objc_msgSend)(c,sel_registerName("copyPropertyForKey:"),k);}
static void number(CFTypeRef v){double n;if(v&&CFGetTypeID(v)==CFNumberGetTypeID()&&CFNumberGetValue(v,kCFNumberDoubleType,&n))printf("%.6f",n);else fputs("null",stdout);}
static void boolean(CFTypeRef v){if(v&&CFGetTypeID(v)==CFBooleanGetTypeID())fputs(CFBooleanGetValue(v)?"true":"false",stdout);else number(v);}
int main(int argc,char **argv){
 int stream=argc>1&&!strcmp(argv[1],"--stream");
 Client h=IOHIDEventSystemClientCreateWithType(0,1,0);
 dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness",RTLD_NOW);
 Class cl=objc_getClass("BrightnessSystemClient");
 id c=((id(*)(id,SEL))objc_msgSend)(((id(*)(Class,SEL))objc_msgSend)(cl,sel_registerName("alloc")),sel_registerName("init"));
 if(!h||!c)return 2;
 do {
  id pool=((id(*)(id,SEL))objc_msgSend)(((id(*)(Class,SEL))objc_msgSend)(objc_getClass("NSAutoreleasePool"),sel_registerName("alloc")),sel_registerName("init"));
  CFArrayRef a=IOHIDEventSystemClientCopyServices(h);CFTypeRef e=0;
  if(a)for(CFIndex i=0;i<CFArrayGetCount(a);i++){
   Service s=(Service)CFArrayGetValueAtIndex(a,i);CFTypeRef p=IOHIDServiceClientCopyProperty(s,CFSTR("Product"));
   int ours=p&&CFGetTypeID(p)==CFStringGetTypeID()&&CFEqual(p,CFSTR("Redwood ambient light sensor"));
   if(p)CFRelease(p);if(ours){e=IOHIDServiceClientCopyEvent(s,12,0,0);break;}
  }
  fputs("{\"lux\":",stdout);if(e)printf("%.3f",IOHIDEventGetFloatValue(e,0xc000b));else fputs("null",stdout);
  fputs(",\"sensor_age_s\":",stdout);
  if(e){mach_timebase_info_data_t tb;mach_timebase_info(&tb);uint64_t now=mach_absolute_time(),t=IOHIDEventGetTimeStamp(e);printf("%.3f",now>=t?(double)(now-t)*tb.numer/tb.denom/1e9:0);}else fputs("null",stdout);
  CFTypeRef d=property(c,CFSTR("DisplayBrightness"));
  fputs(",\"ios_nits\":",stdout);number(d&&CFGetTypeID(d)==CFDictionaryGetTypeID()?CFDictionaryGetValue(d,CFSTR("Nits")):0);
  fputs(",\"ios_brightness\":",stdout);number(d&&CFGetTypeID(d)==CFDictionaryGetTypeID()?CFDictionaryGetValue(d,CFSTR("Brightness")):0);
  const CFStringRef keys[]={CFSTR("DisplayBrightnessAuto"),CFSTR("DisplayOn"),CFSTR("ColorAdaptationEnabled")};
  const char *names[]={"auto_brightness","display_on","true_tone"};
  for(unsigned i=0;i<3;i++){CFTypeRef b=property(c,keys[i]);printf(",\"%s\":",names[i]);boolean(b);if(b)CFRelease(b);}
  char reply[512]={0};size_t size=sizeof(reply);const char *cmd="fb.factory";double target;
  fputs(",\"panel_target\":",stdout);
  if(sysctlbyname("kern.redwood.poke",reply,&size,(void*)cmd,strlen(cmd))==0){char *p=strstr(reply,"target ");if(p&&sscanf(p+7,"%lf",&target)==1)printf("%.3f",target);else fputs("null",stdout);}else fputs("null",stdout);
  puts("}");fflush(stdout);
  if(d)CFRelease(d);if(e)CFRelease(e);if(a)CFRelease(a);
  ((void(*)(id,SEL))objc_msgSend)(pool,sel_registerName("drain"));
  if(stream)usleep(1000000);
 }while(stream);
 return 0;
}

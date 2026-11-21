/* Read-only NSError code observer for native media clients. No descriptions logged. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#include <dlfcn.h>
#include "shared-layer-scope.h"
static int fd=-1;
static __thread BOOL inside;
static id (*nativeCreate)(id,SEL,id,NSInteger,id);
static NSMutableDictionary *lastSeen;
/* Static framework code offsets only. Never inspect stack bytes/arguments. */
static NSArray *mediaCodeOrigins(void){
 void *frames[24];int count=backtrace(frames,24);
 NSMutableArray *origins=[NSMutableArray array];
 for(int i=0;i<count && origins.count<8;i++){
  Dl_info info;if(!dladdr(frames[i],&info) || !info.dli_fname || !info.dli_fbase)continue;
  NSString *image=[[NSString stringWithUTF8String:info.dli_fname] lastPathComponent];
  if(![@[@"AppleMediaServices",@"AppleMediaServicesCore",@"AppleMediaServicesUI"] containsObject:image])continue;
  uintptr_t address=(uintptr_t)frames[i],base=(uintptr_t)info.dli_fbase;
  if(address<base || address-base>128*1024*1024)continue;
  [origins addObject:@{@"image":image,@"offset":@(address-base)}];
 }
 return origins;
}
static void observe(id value,const char *phase){
 if(inside || ![value isKindOfClass:NSError.class])return;
 inside=YES;
 @try {
  NSMutableArray *chain=[NSMutableArray array];NSError *error=value;
  for(unsigned i=0;i<6 && [error isKindOfClass:NSError.class];i++){
   NSString *domain=error.domain;
   if(domain.length>128 || (![domain hasPrefix:@"com.apple."] && ![domain hasSuffix:@"ErrorDomain"] && ![domain hasPrefix:@"AK"] && ![domain hasPrefix:@"AA"] && ![domain hasPrefix:@"CDP"]))domain=@"other";
   [chain addObject:@{@"domain":domain,@"code":@(error.code)}];error=error.userInfo[NSUnderlyingErrorKey];
  }
  NSData *keyData=[NSJSONSerialization dataWithJSONObject:chain options:0 error:NULL];NSString *key=keyData?[[NSString alloc] initWithData:keyData encoding:NSUTF8StringEncoding]:@"other";
  NSTimeInterval now=NSDate.date.timeIntervalSince1970;
  @synchronized(NSError.class){
   if(!lastSeen)lastSeen=[NSMutableDictionary dictionary];
   if(lastSeen.count>256)[lastSeen removeAllObjects];
   if(!lastSeen[key] || now-[lastSeen[key] doubleValue]>=5){
    lastSeen[key]=@(now);
    NSMutableDictionary *entry=[@{@"event":@"media_native_error",@"pid":@(getpid()),@"process":NSProcessInfo.processInfo.processName,@"phase":[NSString stringWithUTF8String:phase],@"time":@(now),@"errors":chain} mutableCopy];
    if([[(NSError *)value domain] isEqual:@"AMSErrorDomain"])entry[@"code_origins"]=mediaCodeOrigins();
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:entry options:0 error:NULL];
    if(fd>=0 && bytes){write(fd,bytes.bytes,bytes.length);write(fd,"\n",1);}
   }
  }
 } @finally {inside=NO;}
}
static id create(id self,SEL cmd,id domain,NSInteger code,id info){
 id result=nativeCreate(self,cmd,domain,code,info);observe(result,"create");return result;
}
__attribute__((visibility("default"))) BOOL rw_media_error_boundary_install(void){
 Method method=class_getClassMethod(NSError.class,@selector(errorWithDomain:code:userInfo:));
 if(!method || strcmp(method_getTypeEncoding(method),"@40@0:8@16q24@32"))return NO;
 nativeCreate=(void *)method_getImplementation(method);method_setImplementation(method,(IMP)create);
 return YES;
}
__attribute__((visibility("default"))) void rw_media_descriptions_install(void){
 unsigned count=0;Class *classes=objc_copyClassList(&count);
 for(unsigned i=0;i<count;i++){
  Class cls=classes[i];BOOL isError=NO;
  for(Class parent=cls;parent;parent=class_getSuperclass(parent))if(parent==NSError.class){isError=YES;break;}
  if(!isError)continue;
  unsigned n=0;Method *methods=class_copyMethodList(cls,&n);
  for(unsigned j=0;j<n;j++){
   Method method=methods[j];SEL selector=method_getName(method);
   if(selector!=@selector(localizedDescription) || strcmp(method_getTypeEncoding(method),"@16@0:8"))continue;
   IMP original=method_getImplementation(method);
   IMP replacement=imp_implementationWithBlock(^id(id self){observe(self,"localized");return ((id(*)(id,SEL))original)(self,selector);});
   method_setImplementation(method,replacement);
  }free(methods);
 }free(classes);
}
__attribute__((constructor)) static void start(void){
 if(!rw_shared_media_process())return;
 dispatch_async(dispatch_get_main_queue(),^{
  fd=open("/var/mobile/Library/RedwoodAppleAccountLab/media-error-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  BOOL installed=rw_media_error_boundary_install();rw_media_descriptions_install();
  if(fd>=0)dprintf(fd,"{\"event\":\"media_error_boundary_installed\",\"pid\":%d,\"installed\":%s}\n",getpid(),installed?"true":"false");
 });
}

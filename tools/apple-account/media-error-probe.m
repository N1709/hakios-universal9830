/* Passive promise-failure observation; preserves native return/arguments. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include "shared-layer-scope.h"
static int fd=-1;
static BOOL (*nativeFinish)(id,SEL,id,id,BOOL);
static BOOL finish(id self,SEL cmd,id result,NSError *error,BOOL duplicate){
 NSError *originalError=error;
 if([error isKindOfClass:NSError.class]){
  NSMutableArray *chain=[NSMutableArray array];
  for(unsigned i=0;i<6 && [error isKindOfClass:NSError.class];i++){
   NSString *domain=error.domain;
   if(domain.length>128 || (![domain hasPrefix:@"com.apple."] && ![domain hasSuffix:@"ErrorDomain"] && ![domain hasPrefix:@"AK"]))domain=@"other";
   [chain addObject:@{@"domain":domain,@"code":@(error.code)}];error=error.userInfo[NSUnderlyingErrorKey];
  }
  NSDictionary *record=@{@"event":@"media_promise_error",@"process":NSProcessInfo.processInfo.processName,@"pid":@(getpid()),@"errors":chain};
  NSData *bytes=[NSJSONSerialization dataWithJSONObject:record options:0 error:NULL];
  if(fd>=0 && bytes){write(fd,bytes.bytes,bytes.length);write(fd,"\n",1);}
 }
 /* Do not replace the original error variable while walking its chain. */
 return nativeFinish(self,cmd,result,originalError,duplicate);
}

__attribute__((visibility("default"))) BOOL rw_media_error_probe_install(void){
 Class cls=objc_getClass("AMSMutablePromise");SEL sel=sel_registerName("_finishWithResult:error:logDuplicateFinishes:");
 Method method=class_getInstanceMethod(cls,sel);
 if(!method || strcmp(method_getTypeEncoding(method),"B36@0:8@16@24B32"))return NO;
 nativeFinish=(void *)method_getImplementation(method);method_setImplementation(method,(IMP)finish);return YES;
}
__attribute__((constructor)) static void start(void){
 if(!rw_shared_media_process())return;
 dispatch_async(dispatch_get_main_queue(),^{
  dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
  fd=open("/var/mobile/Library/RedwoodAppleAccountLab/media-error-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  BOOL installed=rw_media_error_probe_install();
  if(fd>=0)dprintf(fd,"{\"event\":\"media_error_probe_installed\",\"pid\":%d,\"installed\":%s}\n",getpid(),installed?"true":"false");
 });
}

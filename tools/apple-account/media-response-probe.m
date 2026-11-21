/* Passive native Media Services completion metadata, no request-body access. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include "shared-layer-scope.h"
#include "media-response-summary.h"
@interface NSObject (RWMediaTask)
- (NSURLRequest *)currentRequest;
- (NSURLResponse *)response;
@end
static int fd=-1;
static void (*nativeComplete)(id,SEL,id,id,id);
static NSArray *codes(NSError *error){
 NSMutableArray *result=[NSMutableArray array];
 for(unsigned i=0;i<6 && [error isKindOfClass:NSError.class];i++){
  NSString *domain=error.domain;
  if(domain.length>128 || (![domain hasPrefix:@"com.apple."] && ![domain hasSuffix:@"ErrorDomain"] && ![domain hasPrefix:@"AK"]))domain=@"other";
  [result addObject:@{@"domain":domain,@"code":@(error.code)}];error=error.userInfo[NSUnderlyingErrorKey];
 }return result;
}
static void complete(id self,SEL cmd,id task,id object,id error){
 NSURLRequest *request=[task respondsToSelector:@selector(currentRequest)]?[task currentRequest]:nil;
 NSString *host=[request isKindOfClass:NSURLRequest.class]?request.URL.host.lowercaseString:nil;
 {
  id response=[task respondsToSelector:@selector(response)]?[task response]:nil;
  NSInteger status=[response isKindOfClass:NSHTTPURLResponse.class]?[response statusCode]:0;
  NSString *role=@"other",*path=request.URL.path;
  if([path isEqual:@"/auth/v1/cert"])role=@"machine_certificate";
  else if([path isEqual:@"/auth/v1/session"])role=@"machine_session";
  else if([path isEqual:@"/grandslam/GsService2/validate"])role=@"identity_validation";
  else if([path isEqual:@"/grandslam/GsService2"])role=@"srp_authentication";
  else if([path hasSuffix:@"/authenticate"])role=@"store_authentication";
  NSString *kind=object==nil?@"nil":([object isKindOfClass:NSDictionary.class]?@"dictionary":([object isKindOfClass:NSData.class]?@"data":([object isKindOfClass:NSString.class]?@"string":([object isKindOfClass:NSArray.class]?@"array":@"other"))));
  NSDictionary *entry=@{@"event":@"media_decoded_response",@"pid":@(getpid()),@"process":NSProcessInfo.processInfo.processName,
   @"host":[host hasSuffix:@".apple.com"]?host:@"other",@"http_status":@(status),@"endpoint_role":role,@"decoded_type":kind,@"summary":rw_media_response_summary(object),@"errors":codes(error)};
  NSData *bytes=[NSJSONSerialization dataWithJSONObject:entry options:0 error:NULL];
  if(fd>=0 && bytes){write(fd,bytes.bytes,bytes.length);write(fd,"\n",1);}
 }
 nativeComplete(self,cmd,task,object,error);
}
__attribute__((visibility("default"))) BOOL rw_media_response_probe_install(void){
 Method method=class_getInstanceMethod(objc_getClass("AMSURLSession"),sel_registerName("_completeTask:decodedObject:error:"));
 if(!method || strcmp(method_getTypeEncoding(method),"v40@0:8@16@24@32"))return NO;
 nativeComplete=(void *)method_getImplementation(method);method_setImplementation(method,(IMP)complete);return YES;
}
__attribute__((constructor)) static void start(void){
 if(!rw_shared_media_process())return;
 dispatch_async(dispatch_get_main_queue(),^{
  dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
  fd=open("/var/mobile/Library/RedwoodAppleAccountLab/media-error-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  BOOL installed=rw_media_response_probe_install();if(fd>=0)dprintf(fd,"{\"event\":\"media_response_probe_installed\",\"pid\":%d,\"installed\":%s}\n",getpid(),installed?"true":"false");
 });
}

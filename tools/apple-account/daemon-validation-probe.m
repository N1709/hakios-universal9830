/* Passive background AppleAccount response summary. No request/body/token logs. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include "icloud-response-summary.h"
static int output=-1;
static void (*original)(id,SEL,id,id,id);
static void record(NSDictionary *value){
 NSData *bytes=[NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
 if(output>=0 && bytes){write(output,bytes.bytes,bytes.length);write(output,"\n",1);}
}
static void completion(id self,SEL cmd,id data,id response,id error){
 NSInteger status=[response isKindOfClass:NSHTTPURLResponse.class]?[(NSHTTPURLResponse *)response statusCode]:0;
 NSMutableDictionary *entry=[@{@"event":@"background_account_response",@"pid":@(getpid()),
  @"request_class":NSStringFromClass([self class]),@"http_status":@(status),@"application_summary":rw_icloud_response_summary(data)} mutableCopy];
 if([error isKindOfClass:NSError.class])entry[@"error_code"]=@([error code]);
 record(entry);original(self,cmd,data,response,error);
}
__attribute__((visibility("default"))) BOOL rw_daemon_validation_install(void){
 Method method=class_getInstanceMethod(objc_getClass("AARequest"),sel_registerName("_handleDataTaskCompletionWithData:response:error:"));
 if(!method || strcmp(method_getTypeEncoding(method),"v40@0:8@16@24@32"))return NO;
 original=(void *)method_getImplementation(method);method_setImplementation(method,(IMP)completion);return YES;
}
__attribute__((constructor)) static void start(void){
 if(![NSProcessInfo.processInfo.processName isEqual:@"accountsd"])return;
 dispatch_async(dispatch_get_main_queue(),^{
  dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
  output=open("/var/mobile/Library/RedwoodAppleAccountLab/daemon-validation-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  BOOL installed=rw_daemon_validation_install();record(@{@"event":@"background_validation_probe_installed",@"installed":@(installed),@"pid":@(getpid())});
 });
}

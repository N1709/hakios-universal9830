/* Isolate service verification from shared HTTP auth/cookies. Native request,
 * TLS/authentication delegate, queue, protocols and completion remain intact. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
static id(*originalTask)(id,SEL,id,id);
static __thread BOOL creating;
static int output=-1;
static id task(NSURLSession *self,SEL cmd,NSURLRequest *request,id completion){
 BOOL fixture=getenv("RW_MEDIA_SESSION_SELFTEST")!=NULL;
 BOOL scope=[request.URL.scheme isEqual:@"https"] && [request.URL.host.lowercaseString isEqual:@"gsas.apple.com"];
 if(fixture)scope=[request.URL.scheme isEqual:@"http"] && [request.URL.host isEqual:@"127.0.0.1"];
 if(creating || !scope || !completion)return originalTask(self,cmd,request,completion);
 NSURLSessionConfiguration *config=self.configuration.copy;
 if(config.identifier.length)return originalTask(self,cmd,request,completion);
 config.URLCredentialStorage=nil;config.HTTPCookieStorage=nil;config.HTTPShouldSetCookies=NO;
 creating=YES;
 NSURLSession *isolated=[NSURLSession sessionWithConfiguration:config delegate:self.delegate delegateQueue:self.delegateQueue];
 creating=NO;
 void(^native)(NSData *,NSURLResponse *,NSError *)=completion;
 if(output>=0)dprintf(output,"{\"event\":\"media_verification_session_isolated\",\"delegate_preserved\":%s,\"pid\":%d}\n",isolated.delegate==self.delegate?"true":"false",getpid());
 return originalTask(isolated,cmd,request,^(NSData *data,NSURLResponse *response,NSError *error){
  native(data,response,error);[isolated finishTasksAndInvalidate];
 });
}
__attribute__((visibility("default"))) BOOL rw_media_session_isolation_install(void){
 static BOOL installed=NO;if(installed)return YES;
 Method method=class_getInstanceMethod(NSClassFromString(@"__NSURLSessionLocal"),@selector(dataTaskWithRequest:completionHandler:));
 if(!method || strcmp(method_getTypeEncoding(method),"@32@0:8@16@?24"))return NO;
 originalTask=(void *)method_getImplementation(method);method_setImplementation(method,(IMP)task);installed=YES;return YES;
}
__attribute__((constructor)) static void start(void){
 if(![NSProcessInfo.processInfo.processName isEqual:@"akd"])return;
 dispatch_async(dispatch_get_main_queue(),^{
  output=open("/var/mobile/Library/RedwoodAppleAccountLab/request-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  BOOL installed=rw_media_session_isolation_install();if(output>=0)dprintf(output,"{\"event\":\"media_session_isolation_installed\",\"installed\":%s,\"pid\":%d}\n",installed?"true":"false",getpid());
 });
}

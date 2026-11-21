/* Native delegate verification transport. Credential-bearing body stays on phone. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include "icloud-request-identity.h"
#include "icloud-response-summary.h"
#include "shared-layer-scope.h"
static int logFD=-1;
static void record(NSDictionary *entry){
 NSData *bytes=[NSJSONSerialization dataWithJSONObject:entry options:0 error:NULL];
 if(logFD>=0 && bytes){write(logFD,bytes.bytes,bytes.length);write(logFD,"\n",1);}
}
static NSDictionary *freshIdentity(void){
 NSString *path=@"/var/mobile/Library/RedwoodAppleAccountLab/connection.json";
 id settings=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path]?:NSData.data options:0 error:NULL];
 if(![settings isKindOfClass:NSDictionary.class] || ![settings[@"enabled"] boolValue] || ![settings[@"token"] isKindOfClass:NSString.class] || ![settings[@"port"] isKindOfClass:NSNumber.class])return nil;
 NSInteger port=[settings[@"port"] integerValue];if(port<1024 || port>65535)return nil;
 NSURL *url=[NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:%ld/v1/fresh-icloud-metadata",(long)port]];
 NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:url];request.timeoutInterval=4;
 [request setValue:[@"Bearer " stringByAppendingString:settings[@"token"]] forHTTPHeaderField:@"Authorization"];
 NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
 dispatch_semaphore_t done=dispatch_semaphore_create(0);__block NSData *bytes;__block NSInteger status=0;
 [[session dataTaskWithRequest:request completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
  if(!error && [response isKindOfClass:NSHTTPURLResponse.class]){bytes=data;status=[(NSHTTPURLResponse *)response statusCode];}dispatch_semaphore_signal(done);
 }] resume];
 BOOL timeout=dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC))!=0;[session invalidateAndCancel];
 if(timeout || status!=200 || bytes.length>32768)return nil;
 id reply=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL];
 if(![reply isKindOfClass:NSDictionary.class] || ![reply[@"issued_at"] isKindOfClass:NSNumber.class])return nil;
 NSTimeInterval age=NSDate.date.timeIntervalSince1970-[reply[@"issued_at"] doubleValue];
 if(age< -2 || age>25 || ![reply[@"machine_headers"] isKindOfClass:NSDictionary.class])return nil;
 if(![reply[@"expires_at"] isKindOfClass:NSNumber.class] || [reply[@"expires_at"] doubleValue]<=NSDate.date.timeIntervalSince1970)return nil;
 NSMutableDictionary *headers=[reply[@"machine_headers"] mutableCopy];
 /* Credential-free endpoint returns fresh machine metadata, no body signature.
  * Never send this request body to the helper. */
 [headers removeObjectForKey:@"X-Mme-Nas-Qualify"];
 return headers;
}
@interface NSObject (RWDelegateTransport)
- (id)dataTaskWithRequest:(NSURLRequest *)request completion:(void (^)(NSData *,NSURLResponse *,NSError *))reply;
@end
@interface RWDelegateSession : NSObject
@property(nonatomic,strong) id fallback;
@property(nonatomic) BOOL fixture;
@end
@implementation RWDelegateSession
- (id)dataTaskWithRequest:(NSURLRequest *)native completion:(void (^)(NSData *,NSURLResponse *,NSError *))reply {
 BOOL scope=[native.URL.scheme isEqual:@"https"] && [native.URL.host isEqual:@"setup.icloud.com"];
 BOOL local=self.fixture && [native.URL.scheme isEqual:@"http"] && [native.URL.host isEqual:@"127.0.0.1"];
 if(!scope && !local)return [self.fallback dataTaskWithRequest:native completion:reply];
 NSURLRequest *request=native;
 if(!self.fixture){
  request=rw_auth_lab_align_icloud_request(native,freshIdentity());
  if(!request){record(@{@"event":@"delegate_identity_unavailable"});return [self.fallback dataTaskWithRequest:native completion:reply];}
  record(@{@"event":@"delegate_request_identity_aligned",@"legacy_proof_present":@([request valueForHTTPHeaderField:@"X-Apple-MD"].length>0)});
 }
 NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
 return [session dataTaskWithRequest:request completionHandler:^(NSData *bytes,NSURLResponse *response,NSError *error){
  NSInteger status=[response isKindOfClass:NSHTTPURLResponse.class]?[(NSHTTPURLResponse *)response statusCode]:0;
  record(@{@"event":@"delegate_public_response",@"http_status":@(status),@"application_summary":rw_icloud_response_summary(bytes)});
  reply(bytes,response,error);[session finishTasksAndInvalidate];
 }];
}
@end
static void (*original)(id,SEL,id,id);
static void perform(id self,SEL cmd,id session,id completion){
 if(![NSStringFromClass([self class]) isEqual:@"AALoginDelegatesRequest"]){original(self,cmd,session,completion);return;}
 RWDelegateSession *proxy=[RWDelegateSession new];proxy.fallback=session;
 original(self,cmd,proxy,completion);
}
__attribute__((visibility("default"))) id rw_delegate_session_fixture_proxy(void){RWDelegateSession *proxy=[RWDelegateSession new];proxy.fixture=YES;return proxy;}
__attribute__((constructor)) static void start(void){
 BOOL supported=[NSProcessInfo.processInfo.processName isEqual:@"accountsd"];
#if RW_ACCOUNT_LAYER_BUNDLE
 supported=rw_shared_account_process();
#endif
 if(!supported)return;
 dispatch_async(dispatch_get_main_queue(),^{
  dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
  Method method=class_getInstanceMethod(objc_getClass("AARequest"),sel_registerName("performRequestWithSession:withHandler:"));
  if(!method || strcmp(method_getTypeEncoding(method),"v32@0:8@16@?24"))return;
  logFD=open("/var/mobile/Library/RedwoodAppleAccountLab/daemon-validation-events.log",O_WRONLY|O_APPEND|O_NOFOLLOW);
  original=(void *)method_getImplementation(method);method_setImplementation(method,(IMP)perform);
  record(@{@"event":@"delegate_public_transport_installed",@"pid":@(getpid())});
 });
}

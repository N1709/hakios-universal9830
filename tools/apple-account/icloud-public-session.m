/* Scoped laboratory transport substitution. Native authentication/parser unchanged. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include "icloud-response-summary.h"
#include "shared-layer-scope.h"
static int events=-1;
static void event(const char *label,long code){if(events>=0)dprintf(events,"pid=%d event=%s code=%ld\n",getpid(),label,code);}
static void detail(const char *label,NSDictionary *value) {
 NSData *data=[NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
 if(events>=0 && data)dprintf(events,"pid=%d event=%s summary=%.*s\n",getpid(),label,(int)data.length,(const char *)data.bytes);
}
static NSArray *errorCodes(NSError *error){
 NSMutableArray *codes=[NSMutableArray array];
 for(unsigned i=0;i<5 && [error isKindOfClass:NSError.class];i++) {
  NSString *domain=error.domain;
  if(domain.length>128 || (![domain hasPrefix:@"com.apple."] && ![domain hasPrefix:@"NS"] && ![domain hasPrefix:@"AK"] && ![domain hasPrefix:@"AC"] && ![domain hasPrefix:@"AA"] && ![domain hasPrefix:@"CDP"]))domain=@"other";
  [codes addObject:@{@"domain":domain,@"code":@(error.code)}];error=error.userInfo[NSUnderlyingErrorKey];
 }return codes;
}
static NSMethodSignature *callbackSignature(id completion){
 if(!completion)return nil;
 struct Block{void *isa;int flags,reserved;void *invoke,*descriptor;};
 const struct Block *block=(__bridge const void *)completion;
 if(!(block->flags&(1<<30)))return nil;
 const uint8_t *d=block->descriptor;d+=2*sizeof(uintptr_t);
 if(block->flags&(1<<25))d+=2*sizeof(void *);
 const char *encoding=*(const char *const *)d;
 return encoding?[NSMethodSignature signatureWithObjCTypes:encoding]:nil;
}
__attribute__((visibility("default"))) id rw_public_trace_completion(id completion,const char *label){
 NSMethodSignature *sig=callbackSignature(completion);
 if(!sig || strcmp(sig.methodReturnType,"v"))return completion;
 if(sig.numberOfArguments==3 && [sig getArgumentTypeAtIndex:2][0]=='@'){
  char first=[sig getArgumentTypeAtIndex:1][0];
  if(first=='B' || first=='c'){
   void (^native)(BOOL,id)=completion;
   return [^(BOOL success,id error){detail(label,@{@"success":@(success),@"errors":errorCodes(error)});native(success,error);} copy];
  }
  if(first=='@'){
   void (^native)(id,id)=completion;
   return [^(id value,id error){detail(label,@{@"result_present":@(value!=nil),@"errors":errorCodes(error)});native(value,error);} copy];
  }
 }
 if(sig.numberOfArguments==3 && [sig getArgumentTypeAtIndex:2][0]=='@' && ([sig getArgumentTypeAtIndex:1][0]=='q' || [sig getArgumentTypeAtIndex:1][0]=='Q')){
  void (^native)(NSInteger,id)=completion;
  return [^(NSInteger value,id error){detail(label,@{@"result_code":@(value),@"errors":errorCodes(error)});native(value,error);} copy];
 }
 if(sig.numberOfArguments==4 && [sig getArgumentTypeAtIndex:2][0]=='@' && [sig getArgumentTypeAtIndex:3][0]=='@'){
  char first=[sig getArgumentTypeAtIndex:1][0];
  if(first=='B' || first=='c'){
   void (^native)(BOOL,id,id)=completion;
   return [^(BOOL success,id value,id error){detail(label,@{@"success":@(success),@"result_present":@(value!=nil),@"errors":errorCodes(error)});native(success,value,error);} copy];
  }
  if(first=='@'){
   void (^native)(id,id,id)=completion;
   return [^(id firstValue,id secondValue,id error){detail(label,@{@"result_present":@(firstValue!=nil),@"errors":errorCodes(error)});native(firstValue,secondValue,error);} copy];
  }
 }
 if(sig.numberOfArguments==2 && [sig getArgumentTypeAtIndex:1][0]=='@'){
  void (^native)(id)=completion;
  return [^(id value){detail(label,@{@"errors":errorCodes(value),@"argument_present":@(value!=nil)});native(value);} copy];
 }
 event("validation_callback_unsupported_abi",sig.numberOfArguments);return completion;
}
static void (*originalCDP)(id,SEL,id,id,id);
static void (*originalSaveFlow)(id,SEL,id,id);
static void (*originalVerifyStore)(id,SEL,id,id);
static void (*originalSaveStore)(id,SEL,id,id);
static void (*originalPreflightSave)(id,SEL,id,id);
static void (*originalVerifyOptions)(id,SEL,id,id,id);
static void (*originalRenew)(id,SEL,id,id);
static void cdp(id self,SEL cmd,id account,id context,id completion){
 event("native_cdp_validation_entered",0);
 originalCDP(self,cmd,account,context,rw_public_trace_completion(completion,"native_cdp_validation_completed"));
}
static void saveFlow(id self,SEL cmd,id account,id completion){
 event("native_account_save_entered",0);
 originalSaveFlow(self,cmd,account,rw_public_trace_completion(completion,"native_account_save_completed"));
}
static void verifyStore(id self,SEL cmd,id account,id completion){
 event("native_store_verify_entered",0);
 originalVerifyStore(self,cmd,account,rw_public_trace_completion(completion,"native_store_verify_completed"));
}
static void saveStore(id self,SEL cmd,id account,id completion){
 event("native_store_save_entered",0);
 originalSaveStore(self,cmd,account,rw_public_trace_completion(completion,"native_store_save_completed"));
}
static void preflightSave(id self,SEL cmd,id account,id completion){
 event("native_preflight_save_entered",0);
 originalPreflightSave(self,cmd,account,rw_public_trace_completion(completion,"native_preflight_save_completed"));
}
static void verifyOptions(id self,SEL cmd,id account,id options,id completion){
 event("native_store_verify_options_entered",0);
 originalVerifyOptions(self,cmd,account,options,rw_public_trace_completion(completion,"native_store_verify_options_completed"));
}
static void renew(id self,SEL cmd,id account,id completion){
 event("native_store_renew_entered",0);
 originalRenew(self,cmd,account,rw_public_trace_completion(completion,"native_store_renew_completed"));
}
static void installLaterValidation(void){
 Class flow=objc_getClass("AASignInFlowController"),store=objc_getClass("ACAccountStore");
 Method c=class_getInstanceMethod(flow,sel_registerName("_validateAndEnrollCDPStateForAccount:withCDPContext:completion:"));
 Method f=class_getInstanceMethod(flow,sel_registerName("_saveAccount:withAllDataclassesEnabledIfPossibleWithCompletion:"));
 Method v=class_getInstanceMethod(store,sel_registerName("verifyCredentialsForAccount:withHandler:"));
 Method s=class_getInstanceMethod(store,sel_registerName("saveVerifiedAccount:withCompletionHandler:"));
 Method p=class_getInstanceMethod(store,sel_registerName("saveAccount:withCompletionHandler:"));
 Method o=class_getInstanceMethod(store,sel_registerName("verifyCredentialsForAccount:options:completion:"));
 Method r=class_getInstanceMethod(store,sel_registerName("renewCredentialsForAccount:completion:"));
 if(c && !strcmp(method_getTypeEncoding(c),"v40@0:8@16@24@?32")){originalCDP=(void *)method_getImplementation(c);method_setImplementation(c,(IMP)cdp);}
 if(f && !strcmp(method_getTypeEncoding(f),"v32@0:8@16@?24")){originalSaveFlow=(void *)method_getImplementation(f);method_setImplementation(f,(IMP)saveFlow);}
 if(v && !strcmp(method_getTypeEncoding(v),"v32@0:8@16@?24")){originalVerifyStore=(void *)method_getImplementation(v);method_setImplementation(v,(IMP)verifyStore);}
 if(s && !strcmp(method_getTypeEncoding(s),"v32@0:8@16@?24")){originalSaveStore=(void *)method_getImplementation(s);method_setImplementation(s,(IMP)saveStore);}
 if(p && !strcmp(method_getTypeEncoding(p),"v32@0:8@16@?24")){originalPreflightSave=(void *)method_getImplementation(p);method_setImplementation(p,(IMP)preflightSave);}
 if(o && !strcmp(method_getTypeEncoding(o),"v40@0:8@16@24@?32")){originalVerifyOptions=(void *)method_getImplementation(o);method_setImplementation(o,(IMP)verifyOptions);}
 if(r && !strcmp(method_getTypeEncoding(r),"v32@0:8@16@?24")){originalRenew=(void *)method_getImplementation(r);method_setImplementation(r,(IMP)renew);}

}
static void (*originalVerify)(id,SEL,id,BOOL,id,id);
static void (*originalAlert)(id,SEL,id,id,id);
static void verifyResult(id self,SEL cmd,id account,BOOL success,id error,id completion){
 detail("native_account_validation",@{@"success":@(success),@"errors":errorCodes(error)});
 originalVerify(self,cmd,account,success,error,completion);
}
static void validationAlert(id self,SEL cmd,id error,id account,id completion){
 detail("native_validation_alert",@{@"errors":errorCodes(error)});
 originalAlert(self,cmd,error,account,completion);
}
__attribute__((visibility("default"))) BOOL rw_public_validation_install(void){
 Class cls=objc_getClass("AASignInFlowController");
 Method verify=class_getInstanceMethod(cls,sel_registerName("_verifyLoginResponseForiCloudAccount:withSuccess:error:completion:"));
 Method alert=class_getInstanceMethod(cls,sel_registerName("_delegate_presentValidationAlertForError:forAccount:completion:"));
 if(!verify || !alert || strcmp(method_getTypeEncoding(verify),"v44@0:8@16B24@28@?36") || strcmp(method_getTypeEncoding(alert),"v40@0:8@16@24@?32"))return NO;
 originalVerify=(void *)method_getImplementation(verify);method_setImplementation(verify,(IMP)verifyResult);
 originalAlert=(void *)method_getImplementation(alert);method_setImplementation(alert,(IMP)validationAlert);
 return YES;
}

@interface NSObject (RWTransport)
- (id)dataTaskWithRequest:(NSURLRequest *)request completion:(void (^)(NSData *,NSURLResponse *,NSError *))reply;
@end
@interface RWICloudPublicSession : NSObject
@property(nonatomic,strong) id fallback;
@property(nonatomic) BOOL fixture;
@end
@implementation RWICloudPublicSession
- (id)dataTaskWithRequest:(NSURLRequest *)request completion:(void (^)(NSData *,NSURLResponse *,NSError *))reply {
 BOOL production=[request.URL.scheme isEqual:@"https"] && [request.URL.host isEqual:@"setup.icloud.com"] && [request.URL.path isEqual:@"/setup/signin/v2/login"];
 BOOL local=self.fixture && [request.URL.scheme isEqual:@"http"] && [request.URL.host isEqual:@"127.0.0.1"];
 if(!production && !local)return [self.fallback dataTaskWithRequest:request completion:reply];
 if(!self.fixture && (![request valueForHTTPHeaderField:@"X-MMe-Nas-Qualify"].length || !request.HTTPBody.length)) {
  event("public_session_missing_bound_signature",0);return [self.fallback dataTaskWithRequest:request completion:reply];
 }
 NSURLSessionConfiguration *config=NSURLSessionConfiguration.ephemeralSessionConfiguration;
 config.timeoutIntervalForRequest=30;config.timeoutIntervalForResource=45;
 NSURLSession *session=[NSURLSession sessionWithConfiguration:config];
 event("public_session_login_task_created",0);
 return [session dataTaskWithRequest:request completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
  NSInteger status=[response isKindOfClass:NSHTTPURLResponse.class]?[(NSHTTPURLResponse *)response statusCode]:0;
  event("public_session_login_task_completed",status);
  detail("public_session_application_summary",rw_icloud_response_summary(data));
  reply(data,response,error);
  [session finishTasksAndInvalidate];
 }];
}
@end
static void (*originalPerform)(id,SEL,id,id);
static void perform(id self,SEL cmd,id nativeSession,id completion) {
 if(![NSStringFromClass([self class]) isEqual:@"AALoginAccountRequest"]){originalPerform(self,cmd,nativeSession,completion);return;}
 RWICloudPublicSession *proxy=[RWICloudPublicSession new];proxy.fallback=nativeSession;
 originalPerform(self,cmd,proxy,completion);
}
__attribute__((visibility("default"))) id rw_public_session_fixture_proxy(void) {
 RWICloudPublicSession *proxy=[RWICloudPublicSession new];proxy.fixture=YES;return proxy;
}
__attribute__((constructor)) static void start(void) {
 dispatch_async(dispatch_get_main_queue(),^{
  BOOL settingsApp=[NSProcessInfo.processInfo.processName isEqual:@"Preferences"];
  BOOL supported=settingsApp || [NSProcessInfo.processInfo.processName isEqual:@"accountsd"];
#if RW_ACCOUNT_LAYER_BUNDLE
  supported=rw_shared_account_process();
#endif
  if(!supported || access("/var/mobile/Library/RedwoodAppleAccountLab/public-session-enabled",F_OK))return;
  dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
  Class cls=objc_getClass("AARequest");SEL selector=sel_registerName("performRequestWithSession:withHandler:");
  Method m=class_getInstanceMethod(cls,selector);
  if(!m || strcmp(method_getTypeEncoding(m),"v32@0:8@16@?24"))return;
  events=open("/var/mobile/Library/RedwoodAppleAccountLab/status.log",O_WRONLY|O_APPEND|O_NOFOLLOW);
  originalPerform=(void *)method_getImplementation(m);method_setImplementation(m,(IMP)perform);
  event("public_session_transport_installed",0);
  if(settingsApp || [NSProcessInfo.processInfo.processName isEqual:@"accountsd"] || [NSProcessInfo.processInfo.processName isEqual:@"amsaccountsd"]){event("public_validation_observers_installed",rw_public_validation_install());installLaterValidation();}
 });
}

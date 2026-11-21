/* Passive result/transition flags only; no credential values or decision changes. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include "shared-layer-scope.h"
static int output=-1;
static void record(NSString *stage,NSDictionary *flags){
 if(output<0)return;
 NSDictionary *entry=@{@"event":@"media_auth_flow",@"stage":stage,@"pid":@(getpid()),@"process":NSProcessInfo.processInfo.processName,@"flags":flags,@"time":@(NSDate.date.timeIntervalSince1970)};
 NSData *bytes=[NSJSONSerialization dataWithJSONObject:entry options:0 error:NULL];
 if(bytes){write(output,bytes.bytes,bytes.length);write(output,"\n",1);}
}
static NSDictionary *resultFlags(id result){
 NSMutableDictionary *flags=[@{@"result_present":@(result!=nil),@"dictionary":@([result isKindOfClass:NSDictionary.class])} mutableCopy];
 if([result isKindOfClass:NSDictionary.class]){
  flags[@"field_count"]=@([result count]);
  for(NSString *name in @[@"AKAuthenticationUsernameKey",@"AKAuthenticationPasswordKey",@"AKAuthenticationDSIDKey",@"AKAuthenticationAlternateDSIDKey"]){
   NSString *__unsafe_unretained *key=(NSString *__unsafe_unretained *)dlsym(RTLD_DEFAULT,name.UTF8String);
   if(key && [*key isKindOfClass:NSString.class])flags[name]=@(CFDictionaryContainsKey((__bridge CFDictionaryRef)result,(__bridge const void *)*key));
  }
 }
 return flags;
}
static BOOL (*originalContinue)(id,SEL,id,id,id,id);
static BOOL shouldContinue(id self,SEL sel,id controller,id result,id error,id context){
 BOOL value=originalContinue(self,sel,controller,result,error,context);
 NSMutableDictionary *flags=[resultFlags(result) mutableCopy];flags[@"error_present"]=@(error!=nil);flags[@"native_continue"]=@(value);
 record(@"authkit_delegate",flags);return value;
}
static void (*originalProcess)(id,SEL,id);
static void process(id self,SEL sel,id result){
 id account=nil,results=nil;
 SEL a=sel_registerName("account"),r=sel_registerName("authenticationResults");
 Method am=result?class_getInstanceMethod([result class],a):NULL,rm=result?class_getInstanceMethod([result class],r):NULL;
 if(am && !strcmp(method_getTypeEncoding(am),"@16@0:8"))account=((id(*)(id,SEL))method_getImplementation(am))(result,a);
 if(rm && !strcmp(method_getTypeEncoding(rm),"@16@0:8"))results=((id(*)(id,SEL))method_getImplementation(rm))(result,r);
 NSMutableDictionary *flags=[resultFlags(results) mutableCopy];flags[@"update_result_present"]=@(result!=nil);flags[@"account_present"]=@(account!=nil);
 record(@"process_authkit_update",flags);originalProcess(self,sel,result);
}
static void (*originalFinish)(id,SEL,id,id);
static void finish(id self,SEL sel,id account,id update){
 record(@"finish_verified_account",@{@"account_present":@(account!=nil),@"update_result_present":@(update!=nil)});
 originalFinish(self,sel,account,update);
}
__attribute__((visibility("default"))) BOOL rw_media_auth_flow_install(void){
 static BOOL installed=NO;if(installed)return YES;
 Class update=NSClassFromString(@"AMSAuthKitUpdateTask"),task=NSClassFromString(@"AMSAuthenticateTask");
 Method c=class_getInstanceMethod(update,sel_registerName("authenticationController:shouldContinueWithAuthenticationResults:error:forContext:"));
 Method p=class_getInstanceMethod(task,sel_registerName("_processAuthKitUpdateResult:"));
 Method f=class_getInstanceMethod(task,sel_registerName("_finishWithVerifiedAccount:andAuthKitUpdateResult:"));
 if(!c || strcmp(method_getTypeEncoding(c),"B48@0:8@16@24@32@40") || !p || strcmp(method_getTypeEncoding(p),"v24@0:8@16") || !f || strcmp(method_getTypeEncoding(f),"v32@0:8@16@24"))return NO;
 originalContinue=(void *)method_getImplementation(c);originalProcess=(void *)method_getImplementation(p);originalFinish=(void *)method_getImplementation(f);
 method_setImplementation(c,(IMP)shouldContinue);method_setImplementation(p,(IMP)process);method_setImplementation(f,(IMP)finish);
 installed=YES;return YES;
}
__attribute__((constructor)) static void start(void){
 if(!rw_shared_media_process())return;
 dispatch_async(dispatch_get_main_queue(),^{
  dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
  output=open("/var/mobile/Library/RedwoodAppleAccountLab/media-error-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  BOOL installed=rw_media_auth_flow_install();if(output>=0)dprintf(output,"{\"event\":\"media_auth_flow_installed\",\"installed\":%s,\"pid\":%d}\n",installed?"true":"false",getpid());
 });
}

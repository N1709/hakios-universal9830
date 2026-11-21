#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include "media-response-summary.h"
static id expectedTask,expectedObject,expectedError;static int calls;
static void native(id self,SEL cmd,id task,id object,id error){
 (void)self;(void)cmd;NSCAssert(task==expectedTask && object==expectedObject && error==expectedError,@"media completion arguments changed");calls++;
}
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 Class cls=objc_getClass("AMSURLSession");SEL sel=sel_registerName("_completeTask:decodedObject:error:");Method method=class_getInstanceMethod(cls,sel);if(!method)return 3;
 method_setImplementation(method,(IMP)native);
 void *lib=dlopen(argv[1],RTLD_NOW);BOOL(*install)(void)=lib?(void *)dlsym(lib,"rw_media_response_probe_install"):NULL;if(!install || !install())return 4;
 expectedObject=@{@"failureType":@5000,@"customerMessage":@"This Apple ID has never been used in the store",@"token":@"DO-NOT-EMIT",@"account":@"fixture@example.invalid"};
 NSDictionary *summary=rw_media_response_summary(expectedObject);
 NSCAssert([summary[@"category"] isEqual:@"store_account_review"] && [summary[@"numeric_codes"][@"failureType"] isEqual:@5000],@"summary failed");
 NSData *bytes=[NSJSONSerialization dataWithJSONObject:summary options:0 error:NULL];NSString *text=[[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding];
 NSCAssert(![text containsString:@"DO-NOT-EMIT"] && ![text containsString:@"example.invalid"] && ![text containsString:@"never been used"],@"private values leaked");
 NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
 expectedTask=[session dataTaskWithURL:[NSURL URLWithString:@"https://auth.itunes.apple.com/"]];
 for(NSNumber *failure in @[@YES,@NO]){
  expectedError=failure.boolValue?[NSError errorWithDomain:@"AMSErrorDomain" code:100 userInfo:nil]:nil;
  ((void(*)(id,SEL,id,id,id))method_getImplementation(method))([cls alloc],sel,expectedTask,expectedObject,expectedError);
 }
 [expectedTask cancel];[session invalidateAndCancel];if(calls!=2)return 1;
 puts("PASS: media response observer preserves task/object/error identity; private token/account/text excluded; no request resumed");return 0;
}}

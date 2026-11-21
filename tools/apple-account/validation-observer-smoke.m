/* ABI/argument preservation tests with native method slots and synthetic sentinels. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
static id expectedAccount,expectedError,expectedCompletion;static BOOL expectedSuccess;static int calls;
static void sentinelVerify(id self,SEL cmd,id account,BOOL success,id error,id completion){
 (void)self;(void)cmd;NSCAssert(account==expectedAccount && success==expectedSuccess && error==expectedError && completion==expectedCompletion,@"verify arguments changed");calls++;
}
static void sentinelAlert(id self,SEL cmd,id error,id account,id completion){
 (void)self;(void)cmd;NSCAssert(account==expectedAccount && error==expectedError && completion==expectedCompletion,@"alert arguments changed");calls++;
}
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
 Class cls=objc_getClass("AASignInFlowController");SEL v=sel_registerName("_verifyLoginResponseForiCloudAccount:withSuccess:error:completion:");SEL a=sel_registerName("_delegate_presentValidationAlertForError:forAccount:completion:");
 Method vm=class_getInstanceMethod(cls,v),am=class_getInstanceMethod(cls,a);if(!vm || !am)return 3;
 method_setImplementation(vm,(IMP)sentinelVerify);method_setImplementation(am,(IMP)sentinelAlert);
 void *lib=dlopen(argv[1],RTLD_NOW);BOOL (*install)(void)=lib?(void *)dlsym(lib,"rw_public_validation_install"):NULL;if(!install || !install())return 4;
 id instance=[cls alloc];expectedAccount=[NSObject new];expectedCompletion=[^{} copy];
 for(NSNumber *success in @[@YES,@NO]){
  expectedSuccess=success.boolValue;expectedError=expectedSuccess?nil:[NSError errorWithDomain:@"ACErrorDomain" code:7 userInfo:nil];
  ((void(*)(id,SEL,id,BOOL,id,id))method_getImplementation(vm))(instance,v,expectedAccount,expectedSuccess,expectedError,expectedCompletion);
  ((void(*)(id,SEL,id,id,id))method_getImplementation(am))(instance,a,expectedError,expectedAccount,expectedCompletion);
 }
 if(calls!=4)return 1;
 id (*wrap)(id,const char *)=(void *)dlsym(lib,"rw_public_trace_completion");
 if(wrap){
  NSError *e=[NSError errorWithDomain:@"ACErrorDomain" code:7 userInfo:nil];NSObject *object=[NSObject new];__block int forwarded=0;
  void (^boolean)(BOOL,id)=wrap(^(BOOL ok,id error){NSCAssert(!ok && error==e,@"bool completion changed");forwarded++;},"fixture");boolean(NO,e);
  void (^objects)(id,id)=wrap(^(id value,id error){NSCAssert(value==object && error==e,@"object completion changed");forwarded++;},"fixture");objects(object,e);
  void (^single)(id)=wrap(^(id error){NSCAssert(error==e,@"single completion changed");forwarded++;},"fixture");single(e);
  void (^three)(BOOL,id,id)=wrap(^(BOOL ok,id value,id error){NSCAssert(!ok && value==object && error==e,@"three-argument completion changed");forwarded++;},"fixture");three(NO,object,e);
  void (^threeObjects)(id,id,id)=wrap(^(id value,id second,id error){NSCAssert(value==object && second==object && error==e,@"three object completion changed");forwarded++;},"fixture");threeObjects(object,object,e);
  void (^enumeration)(NSInteger,id)=wrap(^(NSInteger code,id error){NSCAssert(code==3 && error==e,@"renewal enum completion changed");forwarded++;},"fixture");enumeration(3,e);
  NSCAssert(forwarded==6,@"completion not forwarded exactly once");
 }
 puts("PASS: validation/alert observers preserve native method ABI, success/failure, object identities and original completion; no account operation");return 0;
}}

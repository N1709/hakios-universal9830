#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
static id expectedResult,expectedError;static BOOL expectedDuplicate,expectedReturn;static int calls;
static BOOL native(id self,SEL cmd,id result,id error,BOOL duplicate){
 (void)self;(void)cmd;NSCAssert(result==expectedResult && error==expectedError && duplicate==expectedDuplicate,@"native promise arguments changed");calls++;return expectedReturn;
}
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 Class cls=objc_getClass("AMSMutablePromise");SEL sel=sel_registerName("_finishWithResult:error:logDuplicateFinishes:");Method method=class_getInstanceMethod(cls,sel);if(!method)return 3;
 method_setImplementation(method,(IMP)native);
 void *lib=dlopen(argv[1],RTLD_NOW);BOOL(*install)(void)=lib?(void *)dlsym(lib,"rw_media_error_probe_install"):NULL;if(!install || !install())return 4;
 expectedResult=[NSObject new];
 for(NSNumber *value in @[@YES,@NO]){
  expectedReturn=value.boolValue;expectedDuplicate=!value.boolValue;
  expectedError=value.boolValue?[NSError errorWithDomain:@"AMSErrorDomain" code:100 userInfo:@{NSUnderlyingErrorKey:[NSError errorWithDomain:@"ASDErrorDomain" code:500 userInfo:nil]}]:nil;
  BOOL result=((BOOL(*)(id,SEL,id,id,BOOL))method_getImplementation(method))([cls alloc],sel,expectedResult,expectedError,expectedDuplicate);
  NSCAssert(result==expectedReturn,@"native promise return changed");
 }
 if(calls!=2)return 1;puts("PASS: promise observer preserves error/result identities, duplicate flag and native BOOL return; no media authentication");return 0;
}}

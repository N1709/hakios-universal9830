/* Native method-slot forwarding with synthetic error/success; no account calls. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
static id expectedData,expectedResponse,expectedError;static int calls;
static void original(id self,SEL cmd,id data,id response,id error){
 (void)self;(void)cmd;NSCAssert(data==expectedData && response==expectedResponse && error==expectedError,@"native arguments changed");calls++;
}
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
 Class cls=objc_getClass("AARequest");SEL sel=sel_registerName("_handleDataTaskCompletionWithData:response:error:");Method m=class_getInstanceMethod(cls,sel);if(!m)return 3;
 method_setImplementation(m,(IMP)original);
 void *lib=dlopen(argv[1],RTLD_NOW);BOOL (*install)(void)=lib?(void *)dlsym(lib,"rw_daemon_validation_install"):NULL;if(!install || !install())return 4;
 expectedData=[@"synthetic" dataUsingEncoding:NSUTF8StringEncoding];
 for(NSNumber *code in @[@200,@401]){
  expectedResponse=[[NSHTTPURLResponse alloc] initWithURL:[NSURL URLWithString:@"https://fixture.invalid/"] statusCode:code.integerValue HTTPVersion:@"HTTP/1.1" headerFields:@{}];
  expectedError=code.integerValue==200?nil:[NSError errorWithDomain:NSURLErrorDomain code:-1004 userInfo:nil];
  ((void(*)(id,SEL,id,id,id))method_getImplementation(m))([cls alloc],sel,expectedData,expectedResponse,expectedError);
 }
 if(calls!=2)return 1;puts("PASS: passive background observer preserves data/response/error identity on success and failure; no account/network request");return 0;
}}

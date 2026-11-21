#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
static id expectedDomain,expectedInfo,sentinel;static NSInteger expectedCode;static int calls;
static id native(id self,SEL cmd,id domain,NSInteger code,id info){
 (void)self;(void)cmd;NSCAssert(domain==expectedDomain && code==expectedCode && info==expectedInfo,@"factory input changed");calls++;return sentinel;
}
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;
 /* Construct native NSError before replacing factory; its identity must survive. */
 sentinel=[NSError errorWithDomain:@"AMSErrorDomain" code:100 userInfo:@{NSUnderlyingErrorKey:[NSError errorWithDomain:@"ASDErrorDomain" code:500 userInfo:nil],@"password":@"SYNTHETIC-NOT-LOGGED"}];
 expectedDomain=@"AMSErrorDomain";expectedCode=100;expectedInfo=@{@"account":@"fixture@example.invalid"};
 Method method=class_getClassMethod(NSError.class,@selector(errorWithDomain:code:userInfo:));method_setImplementation(method,(IMP)native);
 void *lib=dlopen(argv[1],RTLD_NOW);BOOL(*install)(void)=lib?(void *)dlsym(lib,"rw_media_error_boundary_install"):NULL;if(!install || !install())return 3;
 id result=[NSError errorWithDomain:expectedDomain code:expectedCode userInfo:expectedInfo];
 NSCAssert(result==sentinel && calls==1,@"native error return changed");
 NSString *before=[sentinel localizedDescription];
 void (*descriptions)(void)=(void *)dlsym(lib,"rw_media_descriptions_install");if(!descriptions)return 4;
 descriptions();NSCAssert([[sentinel localizedDescription] isEqual:before],@"native error description changed");

 puts("PASS: native error factory domain/code/userInfo and returned NSError identity preserved; no authentication");return 0;
}}

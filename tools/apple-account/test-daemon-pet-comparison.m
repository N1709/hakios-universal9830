/* Isolated fake getter. No authentication or real token; config must be absent. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <unistd.h>
static id fixture;
static id fakeGetter(id self,SEL selector){(void)self;(void)selector;return fixture;}
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2 || access("/var/mobile/Library/RedwoodAppleAccountLab/native-comparison.json",F_OK)==0)return 2;
    Class cls=objc_getClass("AKAuthenticationServerResponse");
    if(!cls){cls=objc_allocateClassPair(NSObject.class,"AKAuthenticationServerResponse",0);class_addMethod(cls,sel_registerName("passwordEquivalentToken"),(IMP)fakeGetter,"@16@0:8");objc_registerClassPair(cls);}
    else method_setImplementation(class_getInstanceMethod(cls,sel_registerName("passwordEquivalentToken")),(IMP)fakeGetter);
    void *library=dlopen(argv[1],RTLD_NOW);int (*install)(void)=library?(void *)dlsym(library,"rw_daemon_pet_comparison_install"):NULL;
    if(!install || !install())return 3;
    fixture=@"synthetic-fixture-only";id object=[cls new];id (*send)(id,SEL)=(void *)class_getMethodImplementation(cls,sel_registerName("passwordEquivalentToken"));
    if(send(object,sel_registerName("passwordEquivalentToken"))!=fixture)return 4;
    fixture=nil;if(send(object,sel_registerName("passwordEquivalentToken"))!=nil)return 5;
    puts("PASS: native getter observer preserves object/nil identity; absent one-shot config causes no transfer");return 0;
}}

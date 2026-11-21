#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdlib.h>
#include "srp-server-summary.h"
@interface SyntheticSRPServer:NSObject
@end
@implementation SyntheticSRPServer
- (NSInteger)errorCode{return -20101;}
- (BOOL)isAnisetteResyncRequired{return YES;}
- (id)errorMessage{return @"synthetic-secret-never-read";}
@end
static BOOL called;
static id expectedResponse,expectedContext;
static BOOL fakeRequest(id self,SEL selector,id url,id context,CFErrorRef *error){(void)self;(void)selector;(void)url;(void)context;(void)error;return NO;}
static void fakeParse(id self,SEL selector,id response,id error,id context,id completion) {
    (void)self;(void)selector;(void)error;called=response==expectedResponse && context==expectedContext;
    void (^reply)(void)=completion;reply();
}
int main(int argc,const char **argv) { @autoreleasepool {
    id response=[SyntheticSRPServer new];NSDictionary *summary=rw_srp_server_summary(response);
    if(![summary[@"server_error_code"] isEqual:@(-20101)] || ![summary[@"isAnisetteResyncRequired"] boolValue])return 1;
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:summary options:0 error:NULL];
    if([[[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding] containsString:@"synthetic-secret"])return 2;
    if(argc==2) {
        setenv("RW_SRP_PROBE_SELFTEST","1",1);
        Class request=objc_allocateClassPair(NSObject.class,"AKSRPRequest",0);if(!request)return 3;
        class_addMethod(object_getClass(request),sel_registerName("_performRequestWithURL:context:error:"),(IMP)fakeRequest,"B40@0:8@16@24^^{__CFError}32");objc_registerClassPair(request);
        Class service=objc_allocateClassPair(NSObject.class,"AKSRPService",0);if(!service)return 4;
        SEL parser=sel_registerName("_parseFailedServerResponse:withError:context:completion:");
        class_addMethod(service,parser,(IMP)fakeParse,"v48@0:8@16@24@32@?40");objc_registerClassPair(service);
        void *library=dlopen(argv[1],RTLD_NOW);int (*install)(void)=library?(void *)dlsym(library,"rw_srp_probe_install"):NULL;if(!install || !install())return 5;
        expectedResponse=response;expectedContext=[NSObject new];__block BOOL replied=NO;
        void (*invoke)(id,SEL,id,id,id,id)=(void *)class_getMethodImplementation(service,parser);
        invoke([service new],parser,response,nil,expectedContext,^{replied=YES;});
        if(!called || !replied)return 6;
    }
    puts("PASS: native SRP code/flag summary excludes text; observer preserves original arguments/completion");return 0;
}}

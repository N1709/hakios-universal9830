/* Bounded transport diagnostics only. No request context/body/header inspection. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "srp-error-category.h"
#include "srp-server-summary.h"
#include <stdlib.h>

static BOOL (*originalRequest)(id,SEL,NSURL *,id,CFErrorRef *);
static int output=-1;
static void (*originalParseFailure)(id,SEL,id,id,id,id);
static void record(NSDictionary *entry) {
    NSData *data=[NSJSONSerialization dataWithJSONObject:entry options:0 error:NULL];
    if(output>=0 && data){@synchronized(NSProcessInfo.processInfo){write(output,data.bytes,data.length);write(output,"\n",1);}}
}
__attribute__((visibility("default"))) NSString *rw_srp_probe_category(NSError *error) { return rw_srp_error_category(error); }
static BOOL perform(id self, SEL cmd, NSURL *url, id context, CFErrorRef *outError) {
    BOOL result=originalRequest(self,cmd,url,context,outError);
    NSString *host=[url isKindOfClass:NSURL.class]?url.host.lowercaseString:nil;
    if(![host hasSuffix:@".apple.com"])host=@"other";
    NSMutableDictionary *entry=[@{@"event":@"srp_request_result",@"success":@(result),@"host":host} mutableCopy];
    NSError *error=outError && *outError?(__bridge NSError *)*outError:nil;
    NSMutableArray *chain=[NSMutableArray array];
    for(unsigned level=0;level<4 && [error isKindOfClass:NSError.class];level++) {
        NSString *domain=error.domain;
        if(domain.length>128)domain=@"other";
        [chain addObject:@{@"domain":domain?:@"unknown",@"code":@(error.code),@"category":rw_srp_error_category(error)}];
        error=error.userInfo[NSUnderlyingErrorKey];
    }
    entry[@"errors"]=chain;
    record(entry);
    return result;
}
static void parseFailure(id self,SEL selector,id response,id error,id context,id completion) {
    record(@{@"event":@"srp_server_failure_metadata",@"pid":@(getpid()),@"summary":rw_srp_server_summary(response)});
    originalParseFailure(self,selector,response,error,context,completion);
}
__attribute__((visibility("default"))) int rw_srp_probe_install(void) {
    static dispatch_once_t once;static BOOL installed;
    dispatch_once(&once,^{
        Class cls=objc_getClass("AKSRPRequest");
        Method method=cls?class_getClassMethod(cls,sel_registerName("_performRequestWithURL:context:error:")):NULL;
        if(!method || strcmp(method_getTypeEncoding(method),"B40@0:8@16@24^^{__CFError}32"))return;
        output=open("/var/mobile/Library/RedwoodAppleAccountLab/request-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
        originalRequest=(void *)method_getImplementation(method);method_setImplementation(method,(IMP)perform);
        Class service=objc_getClass("AKSRPService");
        Method parser=class_getInstanceMethod(service,sel_registerName("_parseFailedServerResponse:withError:context:completion:"));
        if(parser && !strcmp(method_getTypeEncoding(parser),"v48@0:8@16@24@32@?40")) {
            originalParseFailure=(void *)method_getImplementation(parser);method_setImplementation(parser,(IMP)parseFailure);
        }
        installed=YES;record(@{@"event":@"srp_error_probe_installed",@"pid":@(getpid()),@"server_failure_hooked":@(originalParseFailure!=NULL)});
    });return installed;
}
__attribute__((constructor)) static void start(void) {
    if(getenv("RW_SRP_PROBE_SELFTEST"))return;
    dispatch_async(dispatch_get_main_queue(),^{rw_srp_probe_install();});
}

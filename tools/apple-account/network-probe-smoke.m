/* Loopback status/error checks only. No Apple connection or account operation. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <objc/runtime.h>
@interface NSObject (AccountErrorSmoke)
- (void)setError:(NSError *)error;
- (NSError *)error;
- (void)authenticateWithContext:(id)context completion:(void (^)(id,NSError *))reply;
@end
static NSDictionary *fixtureAuthResult;
static NSError *fixtureAuthError;
static void fixtureAuthenticate(id self,SEL selector,id context,id completion) {
    (void)self;(void)selector;(void)context;
    void (^reply)(id,NSError *)=completion;reply(fixtureAuthResult,fixtureAuthError);
}
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2)return 2;
    setenv("RW_NETWORK_PROBE_SELFTEST","1",1);
    dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
    Class controller=objc_getClass("AKAppleIDAuthenticationController");
    Method auth=class_getInstanceMethod(controller,@selector(authenticateWithContext:completion:));
    if(!auth || strcmp(method_getTypeEncoding(auth),"v32@0:8@16@?24"))return 3;
    method_setImplementation(auth,(IMP)fixtureAuthenticate);
    NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    void *library=dlopen(argv[1],RTLD_NOW);
    int (*install)(void)=library?(void *)dlsym(library,"rw_network_probe_install"):NULL;
    if(!install || !install()){puts("FAIL: install");return 1;}
    NSDictionary *(*summarize)(NSData *,NSInteger)=(void *)dlsym(library,"rw_network_probe_failed_response");
    if(!summarize)return 1;
    NSDictionary *fixture=@{@"status":@1,@"statusMessage":@"The password is incorrect",
        @"token":@"SYNTHETIC-DO-NOT-EMIT",@"account":@"synthetic@example.invalid"};
    NSData *bytes=[NSPropertyListSerialization dataWithPropertyList:fixture format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
    NSDictionary *summary=summarize(bytes,401);
    if(![summary[@"message_category"] isEqual:@"credentials_rejected"] || ![summary[@"numeric_codes"][@"0.status"] isEqual:@1])return 1;
    NSData *encoded=[NSJSONSerialization dataWithJSONObject:summary options:0 error:NULL];
    NSString *text=[[NSString alloc] initWithData:encoded encoding:NSUTF8StringEncoding];
    if([text containsString:@"SYNTHETIC"] || [text containsString:@"example.invalid"] || [text containsString:@"password is incorrect"])return 1;
    if([summarize(bytes,200)[@"response_inspected"] boolValue])return 1;
    if(![summarize(NSData.data,401)[@"format"] isEqual:@"empty"])return 1;
    puts("PASS: failed response numeric-code classification excludes tokens/account/text; successful bodies ignored");
    fixtureAuthResult=@{@"synthetic":@YES};
    __block BOOL authPreserved=NO;
    [[controller new] authenticateWithContext:[NSObject new] completion:^(id result,NSError *error){authPreserved=result==fixtureAuthResult && error==nil;}];
    if(!authPreserved){puts("FAIL: synthetic authentication result changed");return 4;}
    fixtureAuthResult=nil;fixtureAuthError=[NSError errorWithDomain:@"com.redwood.SyntheticAuth" code:7 userInfo:nil];authPreserved=NO;
    [[controller new] authenticateWithContext:[NSObject new] completion:^(id result,NSError *error){authPreserved=result==nil && error==fixtureAuthError;}];
    if(!authPreserved){puts("FAIL: synthetic authentication error changed");return 5;}
    puts("PASS: native authentication callback hook preserves synthetic success/error identity; no native authentication invoked");
    id response=[objc_getClass("AAResponse") new];
    NSError *synthetic=[NSError errorWithDomain:@"com.redwood.SyntheticDiagnostic" code:42 userInfo:nil];
    [response setError:synthetic];
    if([response error]!=synthetic){puts("FAIL: native account error changed");return 1;}
    for(NSString *url in @[@"http://127.0.0.1:49732/__diagnostic__",@"http://127.0.0.1:9/"]) {
        dispatch_semaphore_t done=dispatch_semaphore_create(0);
        __block BOOL checked=NO;
        NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];request.timeoutInterval=3;
        [[session dataTaskWithRequest:request completionHandler:^(__unused NSData *data,NSURLResponse *response,NSError *error){
            checked=[url containsString:@":9/"] ? error!=nil : [response isKindOfClass:NSHTTPURLResponse.class] && [(NSHTTPURLResponse *)response statusCode]==501;
            dispatch_semaphore_signal(done);
        }] resume];
        if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,6*NSEC_PER_SEC)) || !checked){puts("FAIL: loopback completion");return 1;}
    }
    [session invalidateAndCancel];
    puts("PASS: loopback HTTP status and connection error preserved; no Apple/account request");
    return 0;
}}

/* Exercise both native callback paths with the laboratory bridge. No login. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <string.h>
@interface NSObject (LabSmoke)
- (id)fetchAnisetteDataAndProvisionIfNecessary:(BOOL)value error:(NSError **)error;
- (void)fetchAnisetteDataAndProvisionIfNecessary:(BOOL)value withCompletion:(void (^)(id,NSError *))reply;
- (id)machineID;
- (id)oneTimePassword;
- (id)_genericAppleIDHeadersDictionaryForRequest:(NSURLRequest *)request;
- (id)_generateAppleIDHeadersForRequest:(NSURLRequest *)request error:(NSError **)error;
@end
static BOOL valid(id data) {
    return [data respondsToSelector:@selector(machineID)] && [data respondsToSelector:@selector(oneTimePassword)] &&
           [[data machineID] isKindOfClass:NSString.class] && [[data machineID] length] &&
           [[data oneTimePassword] isKindOfClass:NSString.class] && [[data oneTimePassword] length];
}
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2 && (argc!=3 || strcmp(argv[2],"--expect-unavailable")))return 2;
    BOOL unavailable=argc==3;
    void *library=dlopen(argv[1],RTLD_NOW);
    int (*install)(void)=library?dlsym(library,"rw_auth_lab_install"):NULL;
    if(!install || !install()){puts("FAIL: bridge not installed");return 1;}
    id controller=[objc_getClass("AKAnisetteProvisioningController") new];
    NSError *error=nil;
    id sync=[controller fetchAnisetteDataAndProvisionIfNecessary:NO error:&error];
    if(unavailable) {
        if(sync || ![error.domain isEqual:@"com.redwood.AppleAccountLab"] || error.code!=2){puts("FAIL: sync failure contract");return 1;}
        dispatch_semaphore_t done=dispatch_semaphore_create(0);
        __block BOOL handled=NO;
        [controller fetchAnisetteDataAndProvisionIfNecessary:NO withCompletion:^(id data,NSError *replyError){
            handled=!data && [replyError.domain isEqual:@"com.redwood.AppleAccountLab"] && replyError.code==2;
            dispatch_semaphore_signal(done);
        }];
        if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,8*NSEC_PER_SEC)) || !handled){puts("FAIL: async failure contract");return 1;}
        id session=[objc_getClass("AKAppleIDSession") new];
        NSURLRequest *request=[NSURLRequest requestWithURL:[NSURL URLWithString:@"https://gsa.apple.com/grandslam/GsService2"]];
        NSError *formatError=nil;
        [session _generateAppleIDHeadersForRequest:request error:&formatError];
        if(!formatError || !formatError.domain.length){puts("FAIL: native formatter did not report helper failure");return 1;}
        puts("PASS: disconnected helper returns valid errors through native sync/async calls and AuthKit formatter, without crashing or fabricating data");
        return 0;
    }
    if(!valid(sync)||error){printf("FAIL: sync error code %ld\n",(long)error.code);return 1;}
    dispatch_semaphore_t done=dispatch_semaphore_create(0);
    __block BOOL asyncOK=NO;
    [controller fetchAnisetteDataAndProvisionIfNecessary:NO withCompletion:^(id data,NSError *replyError){
        asyncOK=valid(data)&&!replyError;dispatch_semaphore_signal(done);
    }];
    if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,8*NSEC_PER_SEC))||!asyncOK){puts("FAIL: async callback");return 1;}
    id session=[objc_getClass("AKAppleIDSession") new];
    NSURLRequest *request=[NSURLRequest requestWithURL:[NSURL URLWithString:@"https://gsa.apple.com/grandslam/GsService2"]];
    NSDictionary *headers=[session _genericAppleIDHeadersDictionaryForRequest:request];
    if(![headers isKindOfClass:NSDictionary.class] || ![headers[@"X-MMe-Client-Info"] isKindOfClass:NSString.class] || ![headers[@"X-MMe-Client-Info"] length] ||
       ![headers[@"X-Apple-I-MD-M"] isEqual:[sync machineID]]){puts("FAIL: virtual identity header consistency");return 1;}
    NSError *formatError=nil;
    NSDictionary *formatted=[session _generateAppleIDHeadersForRequest:request error:&formatError];
    if(formatError || ![formatted isKindOfClass:NSDictionary.class] ||
       ![formatted[@"X-Apple-I-MD-M"] isEqual:[sync machineID]]){puts("FAIL: native AuthKit header formatter");return 1;}
    puts("PASS: native sync/async data, callback ABI, broker transport and native AuthKit header formatter; no account login");
    return 0;
}}

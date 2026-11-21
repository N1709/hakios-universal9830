/* Request a signature for public fixture bytes. No account, password or login. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface NSObject (ValidationProbe)
+ (id)sharedSigner;
- (void)signatureForData:(NSData *)data completion:(void (^)(id,NSError *))completion;
@end
int main(void) { @autoreleasepool {
    dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
    Class cls=objc_getClass("AAAbsintheSigner");
    Method method=cls?class_getInstanceMethod(cls,@selector(signatureForData:completion:)):NULL;
    if(!method || strcmp(method_getTypeEncoding(method),"v32@0:8@16@?24"))return 2;
    dispatch_semaphore_t done=dispatch_semaphore_create(0);
    __block NSDictionary *report;
    [[cls sharedSigner] signatureForData:[@"Redwood credential-free validation probe" dataUsingEncoding:NSUTF8StringEncoding]
        completion:^(id signature,NSError *error){
        NSMutableArray *chain=[NSMutableArray array];
        for(unsigned n=0;n<4 && [error isKindOfClass:NSError.class];n++) {
            [chain addObject:@{@"domain":error.domain,@"code":@(error.code)}];
            error=error.userInfo[NSUnderlyingErrorKey];
        }
        report=@{@"signature_present":@(signature!=nil),@"errors":chain,@"account_login_attempted":@NO};
        dispatch_semaphore_signal(done);
    }];
    if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,20*NSEC_PER_SEC)))
        report=@{@"timed_out":@YES,@"account_login_attempted":@NO};
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
    if(bytes)fwrite(bytes.bytes,1,bytes.length,stdout);
    return bytes?0:1;
}}

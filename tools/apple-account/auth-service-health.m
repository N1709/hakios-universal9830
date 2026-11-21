/* Read-only native AuthKit RPC. No authentication, accounts or credential access.
 * Intentionally discards the passcode-state result; reports RPC error only. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface NSObject (AuthServiceHealth)
- (BOOL)isDevicePasscodeProtected:(NSError **)error;
@end
int main(void) { @autoreleasepool {
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
    Class cls=objc_getClass("AKAppleIDAuthenticationController");
    Method method=class_getInstanceMethod(cls,@selector(isDevicePasscodeProtected:));
    if(!method || strcmp(method_getTypeEncoding(method),"B24@0:8^@16"))return 2;
    NSError *error=nil;
    (void)[[cls new] isDevicePasscodeProtected:&error];
    NSMutableDictionary *report=[@{@"rpc_error_present":@(error!=nil),@"authentication_called":@NO,@"state_changed":@NO} mutableCopy];
    if(error){report[@"error_domain"]=error.domain;report[@"error_code"]=@(error.code);}
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:0 error:NULL];
    fwrite(json.bytes,1,json.length,stdout);putchar('\n');return error?1:0;
}}

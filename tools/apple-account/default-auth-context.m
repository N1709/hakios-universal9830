/* Construct an empty native context only. Never calls authenticate. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include "auth-context-summary.h"
int main(void) { @autoreleasepool {
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
    Class cls=objc_getClass("AKAppleIDAuthenticationContext");if(!cls)return 1;
    NSDictionary *report=@{@"authentication_called":@NO,@"empty_native_context":rw_context_summary([cls new])};
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:report options:0 error:NULL];
    if(!bytes)return 2;fwrite(bytes.bytes,1,bytes.length,stdout);putchar('\n');return 0;
}}

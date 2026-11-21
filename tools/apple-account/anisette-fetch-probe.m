/* Request Anisette data without account login. Provisioning requires an
 * explicit --provision-if-needed invocation; no identity resets are called.
 * Output is limited to availability booleans and error domain/code. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>
@interface NSObject (AnisetteReadProbe)
- (id)fetchAnisetteDataAndProvisionIfNecessary:(BOOL)provision error:(NSError **)error;
- (id)machineID;
- (id)oneTimePassword;
- (unsigned long long)routingInfo;
@end
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2 || (strcmp(argv[1], "--fetch-no-provision") &&
                     strcmp(argv[1], "--provision-if-needed"))) return 2;
    BOOL provision = !strcmp(argv[1], "--provision-if-needed");
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit", RTLD_NOW);
    Class cls = objc_getClass("AKAnisetteProvisioningController");
    Method method = cls ? class_getInstanceMethod(cls,
        @selector(fetchAnisetteDataAndProvisionIfNecessary:error:)) : NULL;
    NSMutableDictionary *report = [@{@"provisioning_allowed": @(provision),
                                      @"method_present": @(method != NULL)} mutableCopy];
    BOOL compatible = NO;
    if (method) {
        NSMethodSignature *s = [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(method)];
        compatible = s.numberOfArguments == 4 && !strcmp(s.methodReturnType, @encode(id)) &&
            !strcmp([s getArgumentTypeAtIndex:2], @encode(BOOL)) &&
            !strcmp([s getArgumentTypeAtIndex:3], "^@");
    }
    report[@"compatible_signature"] = @(compatible);
    if (compatible) @try {
        id controller = [cls new];
        NSError *error = nil;
        id data = [controller fetchAnisetteDataAndProvisionIfNecessary:provision error:&error];
        report[@"data_present"] = @(data != nil);
        if (error) {
            report[@"error_domain"] = error.domain;
            report[@"error_code"] = @(error.code);
            NSMutableArray *chain = [NSMutableArray array];
            NSError *underlying = error.userInfo[NSUnderlyingErrorKey];
            for (unsigned n = 0; n < 4 && [underlying isKindOfClass:NSError.class]; ++n) {
                [chain addObject:@{@"domain": underlying.domain, @"code": @(underlying.code)}];
                underlying = underlying.userInfo[NSUnderlyingErrorKey];
            }
            if (chain.count) report[@"underlying_errors"] = chain;
        }
        if ([data respondsToSelector:@selector(machineID)]) {
            id value = [data machineID];
            report[@"machine_id_nonempty"] = @([value isKindOfClass:NSString.class] && [value length] != 0);
        }
        if ([data respondsToSelector:@selector(oneTimePassword)]) {
            id value = [data oneTimePassword];
            report[@"machine_proof_nonempty"] = @([value isKindOfClass:NSString.class] && [value length] != 0);
        }
        if ([data respondsToSelector:@selector(routingInfo)]) {
            report[@"routing_info_available"]=@YES;
            report[@"routing_info_nonzero"]=@([data routingInfo]!=0);
        }
    } @catch (NSException *exception) {
        report[@"exception_class"] = NSStringFromClass(exception.class);
    }
    NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
    fwrite(json.bytes, 1, json.length, stdout); putchar('\n');
    return 0;
}}

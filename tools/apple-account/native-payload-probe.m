/* One-shot, credential-free parameter construction inside akd. No login call.
 * Never serialize native parameters: they can contain sensitive account data. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

@interface NSObject (PayloadProbe)
- (id)initWithContext:(id)context;
- (void)authenticationParametersWithCompletion:(void (^)(NSDictionary *, NSError *))reply;
- (id)fetchAnisetteDataAndProvisionIfNecessary:(BOOL)provision error:(NSError **)error;
- (id)machineID;
+ (id)ak_anisetteHeadersWithData:(id)data;
@end

static NSString *key(const char *symbol) {
    NSString *__unsafe_unretained *value = (NSString *__unsafe_unretained *)dlsym(RTLD_DEFAULT, symbol);
    return value ? *value : nil;
}

__attribute__((constructor)) static void start(void) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ @autoreleasepool {
        NSString *cpdKey = key("kAppleIDAuthSupportClientProvidedData");
        NSString *extraKey = key("kAppleIDAuthSupportAdditionalHeaders");
        NSString *infoKey = key("kAppleIDAuthSupportClientInfo");
        Class helperClass = objc_getClass("AKSRPContextHelper");
        Method method = class_getInstanceMethod(helperClass,
            @selector(authenticationParametersWithCompletion:));
        if (!cpdKey || !extraKey || !method ||
            strcmp(method_getTypeEncoding(method), "v24@0:8@?16")) return;
        // A fresh empty context has no username, password, account tokens or code.
        id context = [objc_getClass("AKAppleIDAuthenticationContext") new];
        id helper = [[helperClass alloc] initWithContext:context];
        [helper authenticationParametersWithCompletion:^(NSDictionary *parameters, NSError *error) {
            @autoreleasepool {
            NSMutableDictionary *report = [NSMutableDictionary dictionary];
            report[@"parameters_available"] = @([parameters isKindOfClass:NSDictionary.class]);
            report[@"construction_error"] = @(error != nil);
            if (error) {
                report[@"error_domain"] = error.domain;
                report[@"error_code"] = @(error.code);
            }
            NSDictionary *cpd = [parameters isKindOfClass:NSDictionary.class] ? parameters[cpdKey] : nil;
            NSDictionary *extra = [parameters isKindOfClass:NSDictionary.class] ? parameters[extraKey] : nil;
            report[@"cpd_available"] = @([cpd isKindOfClass:NSDictionary.class]);
            report[@"additional_headers_available"] = @([extra isKindOfClass:NSDictionary.class]);
            if ([cpd isKindOfClass:NSDictionary.class]) {
                for (NSString *field in @[@"X-Mme-Device-Id", @"X-Apple-I-MD-LU",
                     @"X-Apple-I-MD-M", @"X-Apple-I-MD-RINFO", @"X-Apple-I-SRL-NO"])
                    report[[field stringByAppendingString:@"_present"]] = @(cpd[field] != nil);
                NSDictionary *(*compare)(NSDictionary *) = (void *)dlsym(RTLD_DEFAULT, "rw_auth_lab_compare_identity");
                if (compare) report[@"cpd_comparison"] = compare(cpd);
                if (compare && [extra isKindOfClass:NSDictionary.class])
                    report[@"additional_headers_comparison"] = compare(extra);
                if (compare && infoKey && [parameters[infoKey] isKindOfClass:NSString.class])
                    report[@"client_info_comparison"] = compare(@{@"X-MMe-Client-Info": parameters[infoKey]});
            }
            // Report only the selected machine metadata's presence, never values.
            report[@"account_login_attempted"] = @NO;
            NSData *bytes = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
            int fd = open("/var/mobile/Library/RedwoodAppleAccountLab/payload-probe.json",
                O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
            if (fd >= 0) { if (bytes) write(fd, bytes.bytes, bytes.length); close(fd); }
            }
        }];
    }});
}

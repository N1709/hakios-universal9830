/* Temporary, manually injected Anisette provider. Account verification remains
 * Apple's native HTTPS flow. Never inspect passwords, codes or account tokens. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "payload-identity.h"
#include "icloud-request-identity.h"
#include "icloud-signing-metadata.h"
#include "shared-layer-scope.h"

static NSString *const LabDirectory = @"/var/mobile/Library/RedwoodAppleAccountLab";
static NSDictionary *configuration, *cachedHeaders, *cachedCloudHeaders;
static NSDate *cachedAt;
static NSLock *cacheLock;
static int statusFD = -1;
static BOOL installed;

/* Boolean-only comparison for the credential-free native parameter probe. */
__attribute__((visibility("default"))) NSDictionary *rw_auth_lab_compare_identity(NSDictionary *candidate) {
    [cacheLock lock];
    NSDictionary *expected = cachedCloudHeaders?:cachedHeaders;
    [cacheLock unlock];
    NSMutableDictionary *report = [NSMutableDictionary dictionary];
    report[@"provider_snapshot_available"] = @(expected != nil);
    for (NSString *field in @[@"X-Mme-Device-Id", @"X-Apple-I-MD-LU",
         @"X-Apple-I-MD-M", @"X-Apple-I-MD-RINFO", @"X-Apple-I-SRL-NO", @"X-MMe-Client-Info"]) {
        id value = [candidate isKindOfClass:NSDictionary.class] ? candidate[field] : nil;
        report[[field stringByAppendingString:@"_matches_provider"]] =
            @(expected[field] != nil && [value isEqual:expected[field]]);
    }
    return report.copy;
}

static void event(const char *name, NSInteger code) {
    if (statusFD >= 0) dprintf(statusFD, "pid=%d event=%s code=%ld\n", getpid(), name, (long)code);
}
static NSError *failure(NSInteger code) {
    return [NSError errorWithDomain:@"com.redwood.AppleAccountLab" code:code
        userInfo:@{NSLocalizedDescriptionKey: @"The temporary authentication helper is unavailable or returned invalid data."}];
}
static BOOL configure(void) {
    NSString *path = [LabDirectory stringByAppendingPathComponent:@"connection.json"];
    struct stat st;
    if (lstat(path.fileSystemRepresentation, &st) || !S_ISREG(st.st_mode) ||
        st.st_uid != 501 || (st.st_mode & 077) || st.st_size > 1024) return NO;
    NSData *bytes = [NSData dataWithContentsOfFile:path];
    id value = bytes ? [NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL] : nil;
    if (![value isKindOfClass:NSDictionary.class] || ![value[@"enabled"] isEqual:@YES] ||
        ![value[@"host"] isEqual:@"127.0.0.1"]) return NO;
    id port = value[@"port"], token = value[@"token"];
    if (![port isKindOfClass:NSNumber.class] || [port integerValue] < 1024 || [port integerValue] > 65535 ||
        ![token isKindOfClass:NSString.class] || [token length] < 32 || [token length] > 128) return NO;
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:
        @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"];
    if ([token rangeOfCharacterFromSet:allowed.invertedSet].location != NSNotFound) return NO;
    configuration = value;
    cacheLock = [NSLock new];
    NSString *log = [LabDirectory stringByAppendingPathComponent:@"status.log"];
    statusFD = open(log.fileSystemRepresentation, O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW, 0600);
    return YES;
}

static NSDictionary *headersForSigningData(NSData *signingData,NSError **outError) {
    /* NSError out-parameters must live in the caller's autorelease pool. */
    {
        if (!configuration) { if (outError) *outError = failure(1); return nil; }
        if(signingData && !rw_auth_lab_valid_signing_metadata(signingData)) {
            if(outError)*outError=failure(7);return nil;
        }
        NSString *nonce = [[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString];
        NSURL *url = [NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:%@/v1/anisette",configuration[@"port"]]];
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        request.HTTPMethod = @"POST";
        request.timeoutInterval = 4;
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        [request setValue:[@"Bearer " stringByAppendingString:configuration[@"token"]] forHTTPHeaderField:@"Authorization"];
        NSMutableDictionary *message=[@{@"version":@1,@"nonce":nonce} mutableCopy];
        if(signingData)message[@"icloud_body"]=[signingData base64EncodedStringWithOptions:0];
        request.HTTPBody = [NSJSONSerialization dataWithJSONObject:message options:0 error:NULL];
        NSURLSessionConfiguration *settings = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        settings.timeoutIntervalForRequest = 4;
        settings.timeoutIntervalForResource = 4;
        settings.connectionProxyDictionary = @{};
        NSURLSession *session = [NSURLSession sessionWithConfiguration:settings];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSData *body;
        __block NSError *networkError;
        __block NSInteger status = 0;
        NSURLSessionDataTask *task = [session dataTaskWithRequest:request completionHandler:
            ^(NSData *data, NSURLResponse *response, NSError *error) {
                body = data; networkError = error;
                if ([response isKindOfClass:NSHTTPURLResponse.class]) status = [(NSHTTPURLResponse *)response statusCode];
                dispatch_semaphore_signal(done);
            }];
        [task resume];
        BOOL timedOut = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC)) != 0;
        [session invalidateAndCancel];
        if (timedOut || networkError || status != 200 || body.length > 32768) {
            event("broker_unavailable", timedOut ? 2 : status);
            if (networkError) event("broker_transport_error",networkError.code);
            if (outError) *outError = failure(2); return nil;
        }
        id reply = [NSJSONSerialization JSONObjectWithData:body options:0 error:NULL];
        NSTimeInterval now = NSDate.date.timeIntervalSince1970;
        if (![reply isKindOfClass:NSDictionary.class] || ![reply[@"version"] isEqual:@1] ||
            ![reply[@"nonce"] isEqual:nonce] || ![reply[@"issued_at"] isKindOfClass:NSNumber.class] ||
            ![reply[@"expires_at"] isKindOfClass:NSNumber.class] ||
            fabs([reply[@"issued_at"] doubleValue]-now)>10 || [reply[@"expires_at"] doubleValue]<=now ||
            [reply[@"expires_at"] doubleValue]-[reply[@"issued_at"] doubleValue]>30) {
            if (outError) *outError = failure(3); return nil;
        }
        id values = reply[@"headers"];
        if(signingData && ![reply[@"signed_metadata_sha256"] isEqual:rw_lab_metadata_digest(signingData)]) {
            if(outError)*outError=failure(8);return nil;
        }
        NSArray *required = @[@"X-Apple-I-MD",@"X-Apple-I-MD-M",@"X-Apple-I-MD-RINFO",@"X-Apple-I-MD-LU",
            @"X-Apple-I-Client-Time",@"X-Mme-Device-Id",@"X-MMe-Client-Info",@"X-Apple-I-SRL-NO"];
        if (![values isKindOfClass:NSDictionary.class] || [values count]>16) {
            if (outError) *outError = failure(4); return nil;
        }
        NSMutableDictionary *clean = [NSMutableDictionary dictionary];
        for (NSString *key in required) {
            id value = values[key];
            if (![value isKindOfClass:NSString.class] || ![value length] || [value length]>8192 ||
                [value rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location!=NSNotFound) {
                if (outError) *outError = failure(4); return nil;
            }
            clean[key] = value;
        }
        for(NSString *key in @[@"X-Mme-Nas-Qualify",@"User-Agent"]) {
            id value=values[key];
            if(!value)continue;
            if(![value isKindOfClass:NSString.class] || ![value length] || [value length]>16384 ||
                [value rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location!=NSNotFound) {
                if(outError)*outError=failure(4);return nil;
            }
            if([key isEqual:@"X-Mme-Nas-Qualify"] && ![[NSData alloc] initWithBase64EncodedString:value options:0]) {
                if(outError)*outError=failure(4);return nil;
            }
            clean[key]=value;
        }
        const char *routing = [clean[@"X-Apple-I-MD-RINFO"] UTF8String];
        char *end = NULL; errno = 0; strtoull(routing,&end,10);
        if (errno || !*routing || !end || *end || *routing=='-') {
            if (outError) *outError = failure(4); return nil;
        }
        if (![[NSData alloc] initWithBase64EncodedString:clean[@"X-Apple-I-MD"] options:0] ||
            ![[NSData alloc] initWithBase64EncodedString:clean[@"X-Apple-I-MD-M"] options:0]) {
            if (outError) *outError = failure(4); return nil;
        }
        [cacheLock lock];
        if(signingData)cachedCloudHeaders=clean.copy;
        else {cachedHeaders=clean.copy;cachedAt=NSDate.date;}
        [cacheLock unlock];
        event("machine_data_received",0);
        return clean;
    }
}
static NSDictionary *headers(NSError **outError) { return headersForSigningData(nil,outError); }
__attribute__((visibility("default"))) BOOL rw_auth_lab_separate_service_metadata(void) {
    [cacheLock lock];
    BOOL valid=cachedHeaders && cachedCloudHeaders &&
        ![cachedHeaders[@"X-MMe-Client-Info"] isEqual:cachedCloudHeaders[@"X-MMe-Client-Info"]] &&
        ![cachedHeaders[@"User-Agent"] isEqual:cachedCloudHeaders[@"User-Agent"]];
    [cacheLock unlock];return valid;
}


@interface NSObject (LabAnisetteData)
- (void)setMachineID:(NSString *)value;
- (void)setOneTimePassword:(NSString *)value;
- (void)setRoutingInfo:(unsigned long long)value;
@end
static id dataObject(NSError **error) {
    NSDictionary *value = headers(error); if (!value) return nil;
    Class cls = objc_getClass("AKAnisetteData");
    if (!cls || ![cls instancesRespondToSelector:@selector(setMachineID:)] ||
        ![cls instancesRespondToSelector:@selector(setOneTimePassword:)] ||
        ![cls instancesRespondToSelector:@selector(setRoutingInfo:)]) {
        if (error) *error = failure(5); return nil;
    }
    id data = [cls new];
    [data setMachineID:value[@"X-Apple-I-MD-M"]];
    [data setOneTimePassword:value[@"X-Apple-I-MD"]];
    [data setRoutingInfo:strtoull([value[@"X-Apple-I-MD-RINFO"] UTF8String],NULL,10)];
    return data;
}

/* Validate the native callback's descriptor before invoking it. */
static BOOL replyABI(id completion) {
    if (!completion) return NO;
    struct Block { void *isa; int flags, reserved; void *invoke; void *descriptor; };
    const struct Block *block = (__bridge const void *)completion;
    if (!(block->flags & (1<<30))) return NO;
    const uint8_t *descriptor = block->descriptor;
    descriptor += 2*sizeof(uintptr_t);
    if (block->flags & (1<<25)) descriptor += 2*sizeof(void *);
    const char *signature = *(const char *const *)descriptor;
    NSMethodSignature *s = signature ? [NSMethodSignature signatureWithObjCTypes:signature] : nil;
    return s.numberOfArguments==3 && !strcmp(s.methodReturnType,"v") &&
        [s getArgumentTypeAtIndex:1][0]=='@' && [s getArgumentTypeAtIndex:2][0]=='@';
}
static void (*originalAsync)(id,SEL,BOOL,id);
static void fetchAsync(id self, SEL cmd, BOOL provision, id completion) {
    if (!replyABI(completion)) { event("unsupported_reply_abi",0); originalAsync(self,cmd,provision,completion); return; }
    void (^reply)(id,NSError *) = completion;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
        NSError *error = nil; id data = dataObject(&error); reply(data,error);
    });
}
static id fetchSync(__unused id self, __unused SEL cmd, __unused BOOL provision, NSError **error) { return dataObject(error); }
/* Replace only the private data read inside the daemon. Its public XPC client
 * authorization and account/password verification stay in Apple's code. */
static id nativeData(__unused id self, __unused SEL cmd, __unused uint64_t dsid, NSError **error) {
    event("native_machine_data_requested",0);
    return dataObject(error);
}
static id (*originalFormatter)(id,SEL,id,NSError **);
static id (*originalCloudRequest)(id,SEL);
static void (*originalSigningHeaders)(id,SEL,id,id);
/* Fixed-label structural diagnostics only; no field values or unknown names. */
static void signingShape(NSData *data,id completion) {
    event("signing_callback_abi_supported",replyABI(completion));
    if(![data isKindOfClass:NSData.class] || !data.length || data.length>16384) {
        event("signing_data_bounded",0);return;
    }
    event("signing_data_bounded",1);
    id root=[NSPropertyListSerialization propertyListWithData:data options:0 format:NULL error:NULL];
    if(![root isKindOfClass:NSDictionary.class]){event("signing_plist_dictionary",0);return;}
    event("signing_plist_dictionary",1);
    event("signing_root_field_count",[root count]);
    event("signing_root_schema_matches",[[NSSet setWithArray:[root allKeys]] isEqual:
        [NSSet setWithArray:@[@"protocolVersion",@"userInfo",@"delegates"]]]);
    event("signing_protocol_matches",[root[@"protocolVersion"] isEqual:@"1.0"]);
    id user=root[@"userInfo"],delegates=root[@"delegates"];
    if([user isKindOfClass:NSDictionary.class]) {
        event("signing_user_field_count",[user count]);
        event("signing_user_schema_matches",[[NSSet setWithArray:[user allKeys]] isEqual:
            [NSSet setWithArray:@[@"client-id",@"language",@"timezone"]]]);
        id identifier=user[@"client-id"];
        event("signing_uuid_valid",[identifier isKindOfClass:NSString.class] && [identifier length]==36 &&
            [[NSUUID alloc] initWithUUIDString:identifier]!=nil);
        event("signing_language_valid",rw_lab_metadata_string(user[@"language"],@"\\A[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8}){0,3}\\z",35));
        event("signing_timezone_valid",rw_lab_metadata_string(user[@"timezone"],@"\\A[A-Za-z0-9_+./:-]+\\z",64));
    } else event("signing_user_dictionary",0);
    if([delegates isKindOfClass:NSDictionary.class]) {
        event("signing_delegate_count",[delegates count]);
        NSUInteger unknown=0;
        for(id key in delegates)if(![@[@"com.apple.mobileme",@"com.apple.private.ids",@"com.apple.gamecenter"] containsObject:key])unknown++;
        event("signing_unknown_delegate_count",unknown);
        id mobile=delegates[@"com.apple.mobileme"],ids=delegates[@"com.apple.private.ids"];
        event("signing_mobileme_schema_matches",!mobile || ([mobile isKindOfClass:NSDictionary.class] && ![mobile count]));
        event("signing_ids_schema_matches",!ids || ([ids isKindOfClass:NSDictionary.class] && [ids count]==1 &&
            [@[@"1",@"2",@"3",@"4"] containsObject:ids[@"protocol-version"]]));
    } else event("signing_delegates_dictionary",0);
}
static void nativeSigningHeaders(id self,SEL cmd,NSData *data,id completion) {
    if(!replyABI(completion) || !rw_auth_lab_valid_signing_metadata(data)) {
        event("native_signing_metadata_not_forwarded",0);
        signingShape(data,completion);
        originalSigningHeaders(self,cmd,data,completion);return;
    }
    void (^reply)(NSDictionary *,NSError *)=completion;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
        NSError *error=nil;NSDictionary *value=headersForSigningData(data,&error);
        NSString *proof=value[@"X-Mme-Nas-Qualify"];
        if(!proof){event("native_signing_helper_failed",error.code);reply(nil,error?:failure(8));return;}
        event("native_request_metadata_signed",0);
        reply(@{@"X-MMe-Nas-Qualify":proof},nil);
    });
}
static id cloudRequest(id self,SEL cmd) {
    NSURLRequest *native=originalCloudRequest(self,cmd);
    if(![native isKindOfClass:NSURLRequest.class] ||
        ![native.URL.host.lowercaseString isEqual:@"setup.icloud.com"] ||
        ![native.URL.scheme.lowercaseString isEqual:@"https"])return native;
    /* Attach the exact-body proof to the NSURLRequest itself. Session-level
     * supplemental headers may not be represented on the task's request. */
    BOOL allowed=rw_auth_lab_valid_signing_metadata(native.HTTPBody);
    NSDictionary *identity=allowed?headersForSigningData(native.HTTPBody,NULL):headers(NULL);
    NSMutableDictionary *metadata=identity.mutableCopy;
    if(!allowed)[metadata removeObjectForKey:@"X-Mme-Nas-Qualify"];
    NSURLRequest *aligned=rw_auth_lab_align_icloud_request(native,metadata);
    if(allowed && aligned && [aligned valueForHTTPHeaderField:@"X-Mme-Nas-Qualify"].length)
        event("icloud_request_bound_signature_attached",0);
    if(!aligned){event("icloud_identity_unavailable",0);return native;}
    event("icloud_identity_headers_aligned",0);
    return aligned;
}
static void (*originalParameters)(id,SEL,id);
static NSString *cpdParameterKey, *extraParameterKey, *infoParameterKey;
static void parametersWithIdentity(id self, SEL cmd, id completion) {
    if (!replyABI(completion)) { event("unsupported_parameter_reply_abi",0); originalParameters(self,cmd,completion); return; }
    void (^reply)(id,NSError *) = completion;
    originalParameters(self,cmd,^(NSDictionary *parameters, NSError *error) {
        if (error || !parameters) { reply(parameters,error); return; }
        [cacheLock lock]; NSDictionary *expected = cachedHeaders; [cacheLock unlock];
        NSDictionary *corrected = rw_auth_lab_align_parameters(parameters, expected,
            cpdParameterKey, extraParameterKey, infoParameterKey);
        if (!corrected) { event("payload_identity_unavailable",0); reply(nil,failure(6)); return; }
        event("payload_identity_aligned",0);
        reply(corrected,nil);
    });
}
static id formatHeaders(id self, SEL cmd, NSURLRequest *request, NSError **error) {
    NSString *host=request.URL.host.lowercaseString;
    if ([host hasSuffix:@".apple.com"] && !headers(error)) return nil;
    return originalFormatter(self,cmd,request,error);
}
static id (*originalGeneric)(id,SEL,id);
static id genericHeaders(id self, SEL cmd, NSURLRequest *request) {
    id original = originalGeneric(self,cmd,request);
    NSString *host = request.URL.host.lowercaseString;
    if (![host hasSuffix:@".apple.com"] || ![original isKindOfClass:NSDictionary.class]) return original;
    [cacheLock lock];
    NSDictionary *value = cachedHeaders;
    BOOL fresh = cachedAt && -cachedAt.timeIntervalSinceNow<5;
    [cacheLock unlock];
    if (!fresh) value = headers(NULL);
    if (!value) return original;
    NSMutableDictionary *result = [original mutableCopy];
    for (NSString *key in value) {
        if([key isEqual:@"X-Mme-Nas-Qualify"])continue;
        for (NSString *old in result.allKeys) if ([old caseInsensitiveCompare:key]==NSOrderedSame) [result removeObjectForKey:old];
        result[key] = value[key];
    }
    if(!value[@"User-Agent"])result[@"User-Agent"] = @"akd/1.0 CFNetwork/1404.0.5 Darwin/22.3.0";
    event("virtual_identity_headers",0);
    return result;
}

__attribute__((visibility("default"))) int rw_auth_lab_install(void) {
    static dispatch_once_t once;
    dispatch_once(&once,^{ @autoreleasepool {
        if (!configure()) return;
        dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
        dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
        Class controller = objc_getClass("AKAnisetteProvisioningController");
        Class session = objc_getClass("AKAppleIDSession");
        Method a = class_getInstanceMethod(controller,sel_registerName("fetchAnisetteDataAndProvisionIfNecessary:withCompletion:"));
        Method b = class_getInstanceMethod(controller,sel_registerName("fetchAnisetteDataAndProvisionIfNecessary:error:"));
        Method c = class_getInstanceMethod(session,sel_registerName("_genericAppleIDHeadersDictionaryForRequest:"));
        Method d = class_getInstanceMethod(session,sel_registerName("_generateAppleIDHeadersForRequest:error:"));
        Class native=objc_getClass("AKNativeAnisetteService");
        Class helper=objc_getClass("AKSRPContextHelper");
        Class cloud=objc_getClass("AALoginAccountRequest");
        Class signing=objc_getClass("AAURLSessionContext");
        Method signingMethod=signing?class_getInstanceMethod(signing,sel_registerName("_additionalAbsintheHeadersForData:completion:")):NULL;
        Method cloudMethod=cloud?class_getInstanceMethod(cloud,sel_registerName("urlRequest")):NULL;
        Method p=helper?class_getInstanceMethod(helper,sel_registerName("authenticationParametersWithCompletion:")):NULL;
        Method n=native?class_getInstanceMethod(native,sel_registerName("_unsafe_anisetteDataWithRoutingInfoForDSID:withError:")):NULL;
        Class dataClass=objc_getClass("AKAnisetteData");
        objc_property_t mid=class_getProperty(dataClass,"machineID"), otp=class_getProperty(dataClass,"oneTimePassword");
        if (!a || !b || !c || !d || !mid || !otp ||
            !strstr(property_getAttributes(mid),"T@\"NSString\"") || !strstr(property_getAttributes(otp),"T@\"NSString\"") ||
            strcmp(method_getTypeEncoding(a),"v28@0:8B16@?20") || strcmp(method_getTypeEncoding(b),"@28@0:8B16^@20") ||
            strcmp(method_getTypeEncoding(c),"@24@0:8@16") || strcmp(method_getTypeEncoding(d),"@32@0:8@16^@24")) {
            event("unsupported_authkit_abi",0); return;
        }
        if (native && (!n || strcmp(method_getTypeEncoding(n),"@32@0:8Q16^@24"))) {
            event("unsupported_native_provider_abi",0); return;
        }
        if (helper) {
            NSString *__unsafe_unretained *cpd=(NSString *__unsafe_unretained *)dlsym(RTLD_DEFAULT,"kAppleIDAuthSupportClientProvidedData");
            NSString *__unsafe_unretained *extra=(NSString *__unsafe_unretained *)dlsym(RTLD_DEFAULT,"kAppleIDAuthSupportAdditionalHeaders");
            NSString *__unsafe_unretained *info=(NSString *__unsafe_unretained *)dlsym(RTLD_DEFAULT,"kAppleIDAuthSupportClientInfo");
            if (!p || strcmp(method_getTypeEncoding(p),"v24@0:8@?16") || !cpd || !extra || !info ||
                ![*cpd isKindOfClass:NSString.class] || ![*extra isKindOfClass:NSString.class] || ![*info isKindOfClass:NSString.class]) {
                event("unsupported_payload_abi",0); return;
            }
            cpdParameterKey=*cpd; extraParameterKey=*extra; infoParameterKey=*info;
        }
        if(cloud && (!cloudMethod || strcmp(method_getTypeEncoding(cloudMethod),"@16@0:8"))) {
            event("unsupported_icloud_request_abi",0);return;
        }
        if(signing && (!signingMethod || strcmp(method_getTypeEncoding(signingMethod),"v32@0:8@16@?24"))) {
            event("unsupported_native_signing_abi",0);return;
        }
        originalAsync = (void *)method_getImplementation(a);
        originalGeneric = (void *)method_getImplementation(c);
        originalFormatter = (void *)method_getImplementation(d);
        if(p) originalParameters=(void *)method_getImplementation(p);
        if(cloudMethod) originalCloudRequest=(void *)method_getImplementation(cloudMethod);
        if(signingMethod)originalSigningHeaders=(void *)method_getImplementation(signingMethod);
        method_setImplementation(a,(IMP)fetchAsync);
        method_setImplementation(b,(IMP)fetchSync);
        method_setImplementation(c,(IMP)genericHeaders);
        method_setImplementation(d,(IMP)formatHeaders);
        if(n){method_setImplementation(n,(IMP)nativeData);event("native_provider_installed",0);}
        if(p){method_setImplementation(p,(IMP)parametersWithIdentity);event("payload_adapter_installed",0);}
        if(cloudMethod){method_setImplementation(cloudMethod,(IMP)cloudRequest);event("icloud_request_adapter_installed",0);}
        if(signingMethod){method_setImplementation(signingMethod,(IMP)nativeSigningHeaders);event("native_signing_adapter_installed",0);}
        installed = YES; event("bridge_installed",0);
    }});
    return installed;
}
__attribute__((constructor)) static void start(void) {
#if RW_ACCOUNT_LAYER_BUNDLE
    if(!rw_shared_account_process())return;
#endif
    dispatch_async(dispatch_get_main_queue(),^{rw_auth_lab_install();});
}

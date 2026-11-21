/* Native machine metadata helper. No account-store, keychain or credential API.
 * --check prints availability only. --stdio is exclusively an SSH/broker RPC:
 * replies contain short-lived machine proofs and must never be printed/logged.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include "icloud-signing-metadata.h"

@interface NSObject (NativeMachineProvider)
+ (id)retrieveOTPHeadersForDSID:(id)dsid;
+ (id)currentDevice;
+ (id)machineUDID;
+ (id)sharedSigner;
+ (id)clientInfoHeader;
+ (id)userAgentHeader;
+ (id)appleIDClientIdentifier;
+ (id)udid;
- (id)localUserUUID;
- (id)uniqueDeviceIdentifier;
- (id)serialNumber;
- (id)serverFriendlyDescription;
- (void)signatureForData:(NSData *)data completion:(void (^)(id,NSError *))completion;
- (id)initWithAccount:(id)account delegates:(id)delegates;
- (NSURLRequest *)urlRequest;
- (id)_genericAppleIDHeadersDictionaryForRequest:(NSURLRequest *)request;
@end

static BOOL validString(id value) {
    return [value isKindOfClass:NSString.class] && [value length] && [value length]<=16384 &&
        [value rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location==NSNotFound;
}
/* Extract actual AppleAccount request metadata from an UNSENT nil-account
 * prototype. AADeviceInfo's nominal User-Agent is not AARequest's final value. */
static NSDictionary *nativeCloudMetadata(void) {
    Class cls=objc_getClass("AALoginAccountRequest");
    Method method=class_getInstanceMethod(cls,@selector(initWithAccount:delegates:));
    if(!method || strcmp(method_getTypeEncoding(method),"@32@0:8@16@24"))return nil;
    NSURLRequest *request=[[[cls alloc] initWithAccount:nil delegates:@{}] urlRequest];
    if(![request isKindOfClass:NSURLRequest.class] || [request valueForHTTPHeaderField:@"Authorization"].length)return nil;
    id info=[request valueForHTTPHeaderField:@"X-MMe-Client-Info"],agent=[request valueForHTTPHeaderField:@"User-Agent"];
    if(!validString(info))return nil;
    NSMutableDictionary *metadata=[@{@"X-MMe-Client-Info":info} mutableCopy];
    if(validString(agent))metadata[@"User-Agent"]=agent;
    return metadata;
}
static NSDictionary *snapshot(NSData *signingData,BOOL cloudMetadata) {
    id otp=[objc_getClass("AOSUtilities") retrieveOTPHeadersForDSID:@"-2"];
    if(![otp isKindOfClass:NSDictionary.class])return nil;
    id device=[objc_getClass("AKDevice") currentDevice];
    // Match the AOSKit OTP source with its machine-derived local-user identity,
    // as AltServer's native AOSKit path does. Do not mix AuthKit's user UUID.
    id deviceID=[objc_getClass("AOSUtilities") machineUDID];
    if(![deviceID isKindOfClass:NSString.class] || ![deviceID isEqual:[device uniqueDeviceIdentifier]])return nil;
    id user=[[deviceID dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0];
    id serial=[device serialNumber], info=[device serverFriendlyDescription];
    id nativeSession=[objc_getClass("AKAppleIDSession") new];
    id nativeHeaders=[nativeSession _genericAppleIDHeadersDictionaryForRequest:
        [NSURLRequest requestWithURL:[NSURL URLWithString:@"https://gsa.apple.com/grandslam/GsService2"]]];
    if([nativeHeaders isKindOfClass:NSDictionary.class] && validString(nativeHeaders[@"X-MMe-Client-Info"]))
        info=nativeHeaders[@"X-MMe-Client-Info"];
    id userAgent=[nativeHeaders isKindOfClass:NSDictionary.class]?nativeHeaders[@"User-Agent"]:nil;
    if(!validString(userAgent)) {
        NSString *version=[NSBundle bundleWithIdentifier:@"com.apple.CFNetwork"].infoDictionary[@"CFBundleVersion"];
        struct utsname system; if(!validString(version) || uname(&system))return nil;
        userAgent=[NSString stringWithFormat:@"akd/1.0 CFNetwork/%@ Darwin/%s",version,system.release];
    }
    if(signingData || cloudMetadata) {
        Class cloudDevice=objc_getClass("AADeviceInfo");
        info=[cloudDevice clientInfoHeader];userAgent=[cloudDevice userAgentHeader];
        if(!validString(info) || !validString(userAgent))return nil;
    }
    id mid=otp[@"X-Apple-MD-M"], proof=otp[@"X-Apple-MD"];
    for(id value in @[deviceID?:NSNull.null,user?:NSNull.null,serial?:NSNull.null,
        info?:NSNull.null,mid?:NSNull.null,proof?:NSNull.null])if(!validString(value))return nil;
    dispatch_semaphore_t done=dispatch_semaphore_create(0);
    __block NSData *validation=nil;
    [[objc_getClass("AAAbsintheSigner") sharedSigner] signatureForData:signingData?:NSData.data completion:^(id signature,NSError *error){
        if(!error && [signature isKindOfClass:NSData.class])validation=signature;
        dispatch_semaphore_signal(done);
    }];
    BOOL completed=NO;NSDate *end=[NSDate dateWithTimeIntervalSinceNow:20];
    while(end.timeIntervalSinceNow>0) {
        if(!dispatch_semaphore_wait(done,DISPATCH_TIME_NOW)){completed=YES;break;}
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    }
    if(!completed || !validation.length)return nil;
    NSString *encoded=[validation base64EncodedStringWithOptions:0];
    if(!validString(encoded))return nil;
    NSISO8601DateFormatter *date=[NSISO8601DateFormatter new];
    return @{@"X-Apple-I-MD":proof,@"X-Apple-I-MD-M":mid,
        // AltServer's documented native-Mac routing hint; not a retrieved value.
        @"X-Apple-I-MD-RINFO":@"84215040",@"X-Apple-I-MD-LU":user,
        @"X-Apple-I-Client-Time":[date stringFromDate:NSDate.date],
        @"X-Mme-Device-Id":deviceID,@"X-MMe-Client-Info":info,@"X-Apple-I-SRL-NO":serial,
        @"X-Mme-Nas-Qualify":encoded,@"User-Agent":userAgent};
}
static void emit(NSDictionary *value) {
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
    if(bytes){fwrite(bytes.bytes,1,bytes.length,stdout);fputc('\n',stdout);fflush(stdout);}
}
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2 || (strcmp(argv[1],"--check") && strcmp(argv[1],"--stdio")))return 2;
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AOSKit.framework/AOSKit",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
    Class utilities=objc_getClass("AOSUtilities"), device=objc_getClass("AKDevice"), signer=objc_getClass("AAAbsintheSigner");
    if(![utilities respondsToSelector:@selector(retrieveOTPHeadersForDSID:)] ||
       ![device respondsToSelector:@selector(currentDevice)] || ![signer respondsToSelector:@selector(sharedSigner)])return 2;
    Method method=class_getInstanceMethod(signer,@selector(signatureForData:completion:));
    if(!method || strcmp(method_getTypeEncoding(method),"v32@0:8@16@?24"))return 2;
    if(!strcmp(argv[1],"--check")) {
        NSDictionary *headers=snapshot(nil,NO);
        NSDictionary *actualCloud=nativeCloudMetadata();
        Class aa=objc_getClass("AADeviceInfo");
        id info=[aa clientInfoHeader],agent=[aa userAgentHeader],client=[aa appleIDClientIdentifier];
        emit(@{@"local_user_identity_source":@"AOSKit-machineUDID-base64",
            @"local_user_identity_matches_device_id_derivation":@([headers[@"X-Apple-I-MD-LU"] isEqual:[[headers[@"X-Mme-Device-Id"] dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0]]),
            @"metadata_available":@(headers!=nil),@"validation_available":@(headers[@"X-Mme-Nas-Qualify"]!=nil),
            @"actual_icloud_request_metadata_available":@(actualCloud!=nil),
            @"actual_icloud_explicit_user_agent_present":@(actualCloud[@"User-Agent"]!=nil),
            @"actual_icloud_user_agent_matches_nominal":@([actualCloud[@"User-Agent"] isEqual:agent]),
            @"icloud_client_info_available":@(validString(info)),@"icloud_user_agent_available":@(validString(agent)),
            @"icloud_client_info_matches_authkit":@([info isEqual:headers[@"X-MMe-Client-Info"]]),
            @"icloud_user_agent_matches_authkit":@([agent isEqual:headers[@"User-Agent"]]),
            @"icloud_client_id_available":@(validString(client)),
            @"icloud_client_id_matches_device_id":@([client isEqual:headers[@"X-Mme-Device-Id"]]),
            @"routing_source":@"AltServer-native-Mac-hint",@"account_login_attempted":@NO});
        return headers?0:1;
    }
    char line[24001];
    while(fgets(line,sizeof(line),stdin)) { @autoreleasepool {
        size_t size=strlen(line);
        if(size>24000 || !strchr(line,'\n')) {
            if(!strchr(line,'\n')){int c;while((c=getchar())!=EOF && c!='\n'){} }
            emit(@{@"error":@"invalid_request"});continue;
        }
        NSData *input=[NSData dataWithBytes:line length:size];
        id request=[NSJSONSerialization JSONObjectWithData:input options:0 error:NULL];
        NSSet *keys=[request isKindOfClass:NSDictionary.class]?[NSSet setWithArray:[request allKeys]]:nil;
        BOOL simple=[keys isEqual:[NSSet setWithArray:@[@"version",@"nonce"]]];
        BOOL scoped=[keys isEqual:[NSSet setWithArray:@[@"version",@"nonce",@"icloud_body"]]];
        BOOL service=[keys isEqual:[NSSet setWithArray:@[@"version",@"nonce",@"service"]]] && [request[@"service"] isEqual:@"icloud"];
        if(![request isKindOfClass:NSDictionary.class] || (!simple && !scoped && !service) ||
           ![request[@"version"] isEqual:@1] || ![request[@"nonce"] isKindOfClass:NSString.class]) {
            emit(@{@"error":@"invalid_request"});continue;
        }
        NSString *nonce=request[@"nonce"];
        NSCharacterSet *allowed=[NSCharacterSet characterSetWithCharactersInString:@"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"];
        if(nonce.length<16 || nonce.length>64 || [nonce rangeOfCharacterFromSet:allowed.invertedSet].location!=NSNotFound) {
            emit(@{@"error":@"invalid_request"});continue;
        }
        NSData *signingData=nil;
        if(scoped) {
            id encoded=request[@"icloud_body"];
            if([encoded isKindOfClass:NSString.class] && [encoded length]<=22000)
                signingData=[[NSData alloc] initWithBase64EncodedString:encoded options:0];
            if(!rw_auth_lab_valid_signing_metadata(signingData)) {
                emit(@{@"error":@"invalid_signing_metadata"});continue;
            }
        }
        NSDictionary *headers=snapshot(signingData,service);
        if(!headers){emit(@{@"error":@"native_provider_unavailable"});continue;}
        NSTimeInterval now=NSDate.date.timeIntervalSince1970;
        NSMutableDictionary *reply=[@{@"version":@1,@"nonce":nonce,@"issued_at":@(now),@"expires_at":@(now+25),@"headers":headers} mutableCopy];
        if(signingData)reply[@"signed_metadata_sha256"]=rw_lab_metadata_digest(signingData);
        emit(reply);
    }}
    return 0;
}}

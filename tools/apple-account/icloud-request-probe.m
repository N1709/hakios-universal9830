/* Build an unsent native request with NO account; inspect machine metadata only. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include "icloud-signing-metadata.h"
@interface NSObject (ICloudRequestProbe)
- (id)initWithAccount:(id)account delegates:(id)delegates;
- (NSURLRequest *)urlRequest;
- (id)fetchAnisetteDataAndProvisionIfNecessary:(BOOL)flag error:(NSError **)error;
- (void)_additionalAbsintheHeadersForData:(NSData *)data completion:(void (^)(NSDictionary *,NSError *))reply;
- (id)initRequiringSigning:(BOOL)flag;
- (void)_additionalHeadersForRequest:(NSURLRequest *)request completion:(void (^)(NSDictionary *,NSError *))reply;
@end
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2)return 2;
    dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
    void *bridge=dlopen(argv[1],RTLD_NOW);
    int (*install)(void)=bridge?(void *)dlsym(bridge,"rw_auth_lab_install"):NULL;
    NSDictionary *(*compare)(NSDictionary *)=bridge?(void *)dlsym(bridge,"rw_auth_lab_compare_identity"):NULL;
    if(!install || !compare || !install())return 1;
    NSError *error=nil;
    id data=[[objc_getClass("AKAnisetteProvisioningController") new]
        fetchAnisetteDataAndProvisionIfNecessary:NO error:&error];
    if(!data || error){puts("Provider unavailable");return 1;}
    Class cls=objc_getClass("AALoginAccountRequest");
    id native=[[cls alloc] initWithAccount:nil delegates:@{@"com.apple.mobileme":@{},@"com.apple.gamecenter":@{},@"com.apple.private.ids":@{@"protocol-version":@"4"}}];
    NSURLRequest *request=[native urlRequest];
    if(![request isKindOfClass:NSURLRequest.class])return 1;
    NSMutableDictionary *metadata=[NSMutableDictionary dictionary];
    for(NSString *field in @[@"X-Mme-Device-Id",@"X-Apple-I-MD-LU",@"X-Apple-I-MD-M",@"X-Apple-I-MD-RINFO",@"X-Apple-I-SRL-NO",@"X-MMe-Client-Info"])
        if([request valueForHTTPHeaderField:field])metadata[field]=[request valueForHTTPHeaderField:field];
    NSMutableDictionary *report=[@{@"headers":compare(metadata),@"account_supplied":@NO,
        @"request_sent":@NO,@"header_count":@(metadata.count)} mutableCopy];
    report[@"fixture_path"]=request.URL.path?:@"";
    report[@"fixture_explicit_user_agent_present"]=@([request valueForHTTPHeaderField:@"User-Agent"].length>0);
    report[@"legacy_icloud_proof_present"]=@([request valueForHTTPHeaderField:@"X-Apple-MD"].length>0);
    report[@"legacy_icloud_machine_present"]=@([request valueForHTTPHeaderField:@"X-Apple-MD-M"].length>0);
    report[@"legacy_icloud_proof_matches_authkit_alias"]=@([[request valueForHTTPHeaderField:@"X-Apple-MD"] isEqual:[request valueForHTTPHeaderField:@"X-Apple-I-MD"]]);
    report[@"legacy_icloud_machine_matches_authkit_alias"]=@([[request valueForHTTPHeaderField:@"X-Apple-MD-M"] isEqual:[request valueForHTTPHeaderField:@"X-Apple-I-MD-M"]]);
    report[@"fixture_signing_metadata_allowed"]=@(rw_auth_lab_valid_signing_metadata(request.HTTPBody));
    Class contextClass=objc_getClass("AAURLSessionContext");
    dispatch_semaphore_t done=dispatch_semaphore_create(0);
    __block NSDictionary *signingHeaders;__block NSError *signingError;
    [[[contextClass alloc] initRequiringSigning:YES] _additionalHeadersForRequest:request completion:^(NSDictionary *value,NSError *error){
        signingHeaders=value;signingError=error;dispatch_semaphore_signal(done);
    }];
    BOOL completed=NO;NSDate *end=[NSDate dateWithTimeIntervalSinceNow:12];
    while(end.timeIntervalSinceNow>0) {
        if(!dispatch_semaphore_wait(done,DISPATCH_TIME_NOW)){completed=YES;break;}
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    }
    report[@"native_signing_completed"]=@(completed);
    if(completed) {
        report[@"native_signing_header_present"]=@([signingHeaders[@"X-MMe-Nas-Qualify"] isKindOfClass:NSString.class] && [signingHeaders[@"X-MMe-Nas-Qualify"] length]>0);
        report[@"native_signing_error_header_present"]=@(signingHeaders[@"X-MMe-Nas-E"]!=nil);
        report[@"native_signing_error_present"]=@(signingError!=nil);
    }
    report[@"fixture_validation_header_present"]=@([request valueForHTTPHeaderField:@"X-Mme-Nas-Qualify"].length>0);
    BOOL (*separate)(void)=(void *)dlsym(bridge,"rw_auth_lab_separate_service_metadata");
    if(separate)report[@"service_specific_metadata_separated"]=@(separate());

    if(argc==2 && getenv("RW_LAB_LOOPBACK_DELIVERY")) {
        /* Account-free fixture only: no login request, credentials or account. */
        NSMutableURLRequest *local=request.mutableCopy;
        local.URL=[NSURL URLWithString:@"http://127.0.0.1:49743/fixture"];
        [local setValue:nil forHTTPHeaderField:@"Authorization"];
        NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
        __block BOOL delivered=NO;__block NSInteger localStatus=0;
        [[session dataTaskWithRequest:local completionHandler:^(NSData *bytes,NSURLResponse *response,NSError *error){
            (void)bytes;localStatus=[response isKindOfClass:NSHTTPURLResponse.class]?[(NSHTTPURLResponse *)response statusCode]:0;
            delivered=!error;
        }] resume];
        NSDate *limit=[NSDate dateWithTimeIntervalSinceNow:5];
        while(!delivered && !localStatus && limit.timeIntervalSinceNow>0)
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
        report[@"loopback_fixture_delivered"]=@(delivered && localStatus==204);
        [session invalidateAndCancel];
    }
    report[@"device_udid_matches_device_header"]=@([[request valueForHTTPHeaderField:@"Device-UDID"]
        isEqual:[request valueForHTTPHeaderField:@"X-Mme-Device-Id"]]);
    // Only this deliberately account-free fixture is decoded; never a live body.
    id body=request.HTTPBody?[NSPropertyListSerialization propertyListWithData:request.HTTPBody options:0 format:NULL error:NULL]:nil;
    if([body isKindOfClass:NSDictionary.class]) {
        report[@"fixture_top_level_keys"]=[[body allKeys] sortedArrayUsingSelector:@selector(compare:)];
        if([body[@"userInfo"] isKindOfClass:NSDictionary.class]) {
            report[@"fixture_user_info_keys"]=[[body[@"userInfo"] allKeys] sortedArrayUsingSelector:@selector(compare:)];
            id client=body[@"userInfo"][@"client-id"];
            if([client isKindOfClass:NSString.class]) {
                NSCharacterSet *hex=[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF"];
                BOOL isHex=[client length] && [client rangeOfCharacterFromSet:hex.invertedSet].location==NSNotFound;
                NSString *normalized=[[client stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString];
                report[@"fixture_client_id_shape"]=@{@"length":@([client length]),
                    @"uuid":@([[NSUUID alloc] initWithUUIDString:client]!=nil),@"hex":@(isHex),
                    @"all_zero":@(normalized.length && [normalized stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"0"]].length==0)};
            }
        }
        NSMutableArray *keys=[NSMutableArray array];
        for(NSString *key in @[@"deviceInfo",@"client-id",@"device-id",@"delegates",@"apple-id",@"password"])
            if(body[key]) [keys addObject:key];
        report[@"fixture_known_fields"]=keys;
    }
    NSData *output=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
    if(output)fwrite(output.bytes,1,output.length,stdout);
    return output?0:1;
}}

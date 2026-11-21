/* One native, unsaved Mac login comparison using an owner-authorized phone PET.
 * Input is private stdin only. Output is statuses/booleans; never payload/token.
 * --build-test sends nothing; --loopback-test sends only an unauthenticated fixture.
 * Native request construction and direct native body signer, public NSURLSession. */
#import <Foundation/Foundation.h>
#import <Accounts/Accounts.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "icloud-signing-metadata.h"
#include "icloud-response-summary.h"
@interface ACAccountCredential (PrivatePassword)
+ (id)credentialWithPassword:(NSString *)password;
@end
@interface NSObject (NativePETComparison)
- (id)initWithAccount:(id)account delegates:(id)delegates;
- (NSURLRequest *)urlRequest;
+ (id)sharedSigner;
+ (id)retrieveOTPHeadersForDSID:(id)dsid;
+ (id)machineUDID;
+ (id)currentDevice;
- (id)serialNumber;
- (void)signatureForData:(NSData *)data completion:(void (^)(id,NSError *))completion;
- (void)_aa_setPersonID:(id)dsid;
- (void)_aa_setAltDSID:(id)dsid;
- (id)httpResponse;
- (NSError *)error;
@end
static void emit(NSDictionary *value) {
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
    if(bytes){fwrite(bytes.bytes,1,bytes.length,stdout);putchar('\n');fflush(stdout);}
}
static BOOL bounded(id value,NSUInteger max) {
    return [value isKindOfClass:NSString.class] && [value length]>0 && [value length]<=max &&
        [value rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location==NSNotFound;
}
/* A public NSURLSession does not auto-fetch private AuthKit metadata. Supply
 * the same AOSKit-sourced identity as the issuer helper, from one OTP snapshot. */
static NSDictionary *aosMetadata(void) {
    id utility=objc_getClass("AOSUtilities"),device=[objc_getClass("AKDevice") currentDevice];
    id otp=[utility retrieveOTPHeadersForDSID:@"-2"],identifier=[utility machineUDID],serial=[device serialNumber];
    if(![otp isKindOfClass:NSDictionary.class] || !bounded(identifier,256) || !bounded(serial,128) ||
       !bounded(otp[@"X-Apple-MD"],16384) || !bounded(otp[@"X-Apple-MD-M"],16384))return nil;
    NSISO8601DateFormatter *date=[NSISO8601DateFormatter new];
    return @{@"X-Apple-I-MD":otp[@"X-Apple-MD"],@"X-Apple-I-MD-M":otp[@"X-Apple-MD-M"],
        @"X-Apple-MD":otp[@"X-Apple-MD"],@"X-Apple-MD-M":otp[@"X-Apple-MD-M"],
        @"X-Apple-I-MD-LU":[[identifier dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0],
        @"X-Apple-I-MD-RINFO":@"84215040",@"X-Mme-Device-Id":identifier,@"Device-UDID":identifier,
        @"X-Apple-I-SRL-NO":serial,@"X-Apple-I-Client-Time":[date stringFromDate:NSDate.date]};
}
int main(int argc,const char **argv) { @autoreleasepool {
    BOOL snapshotTest=argc==2 && !strcmp(argv[1],"--snapshot-test");
    BOOL loopback=argc==2 && !strcmp(argv[1],"--loopback-test");
    BOOL signingTest=argc==2 && !strcmp(argv[1],"--sign-test");
    BOOL fixture=signingTest || loopback || (argc==2 && !strcmp(argv[1],"--build-test"));
    if(!fixture && !snapshotTest && (argc!=2 || strcmp(argv[1],"--compare-once")))return 2;
    dlopen("/System/Library/PrivateFrameworks/IntlPreferences.framework/IntlPreferences",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AOSKit.framework/AOSKit",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
    NSDictionary *input=nil;
    if(fixture)input=@{@"username":@"fixture@example.invalid",@"pet":@"synthetic-not-a-real-token",@"dsid":@"123",@"alt_dsid":@"synthetic-alt",@"version":@1,@"nonce":@"synthetic-fixture-nonce"};
    else {
        char line[32769];if(!fgets(line,sizeof(line),stdin) || !strchr(line,'\n'))return 3;
        NSData *bytes=[NSData dataWithBytes:line length:strlen(line)];
        input=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL];memset(line,0,sizeof(line));
        if(![input isKindOfClass:NSDictionary.class] || ![input[@"version"] isEqual:@1] || (input.count!=7 && input.count!=8 && input.count!=9) ||
           ![input[@"issued_at"] isKindOfClass:NSNumber.class] || fabs([input[@"issued_at"] doubleValue]-NSDate.date.timeIntervalSince1970)>60)return 3;
    }
    if(!bounded(input[@"username"],320) || ![input[@"username"] containsString:@"@"] || !bounded(input[@"pet"],16384) ||
       !bounded(input[@"dsid"],20) || !bounded(input[@"alt_dsid"],256) || !bounded(input[@"nonce"],64))return 3;
    if([input[@"dsid"] rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location!=NSNotFound)return 3;
    ACAccountStore *store=[ACAccountStore new];
    ACAccountType *type=[store accountTypeWithAccountTypeIdentifier:@"com.apple.account.AppleAccount"];
    ACAccount *account=[[ACAccount alloc] initWithAccountType:type];
    if(!account || ![account respondsToSelector:@selector(_aa_setPersonID:)] || ![account respondsToSelector:@selector(_aa_setAltDSID:)])return 4;
    account.username=input[@"username"];
    account.credential=[ACAccountCredential credentialWithPassword:input[@"pet"]];
    [account _aa_setPersonID:input[@"dsid"]];[account _aa_setAltDSID:input[@"alt_dsid"]];
    Class cls=objc_getClass("AALoginAccountRequest");
    id request=[[cls alloc] initWithAccount:account delegates:@{@"com.apple.mobileme":@{},@"com.apple.gamecenter":@{},@"com.apple.private.ids":@{@"protocol-version":@"4"}}];
    NSDictionary *machine=aosMetadata();
    if(fixture && !loopback && !signingTest) {
        NSURLRequest *native=[request urlRequest];
        emit(@{@"machine_metadata_available":@(machine!=nil),@"request_constructed":@(native!=nil),@"authorization_present":@([native valueForHTTPHeaderField:@"Authorization"].length>0),@"request_sent":@NO,@"account_saved":@NO});
        return native?0:1;
    }
    NSMutableURLRequest *outgoing=[[request urlRequest] mutableCopy];if(!outgoing)return 5;
    if(input[@"icloud_body"]) {
        if(!bounded(input[@"icloud_body"],22000))return 6;
        NSData *body=[[NSData alloc] initWithBase64EncodedString:input[@"icloud_body"] options:0];
        if(!rw_auth_lab_valid_signing_metadata(body))return 6;
        outgoing.HTTPBody=body;
    }
    NSDictionary *snapshot=input[@"machine_headers"];
    if(snapshot) {
        NSSet *fields=[NSSet setWithArray:@[@"X-Apple-I-MD",@"X-Apple-I-MD-M",@"X-Apple-I-MD-RINFO",@"X-Apple-I-MD-LU",@"X-Apple-I-Client-Time",@"X-Mme-Device-Id",@"X-MMe-Client-Info",@"X-Apple-I-SRL-NO",@"X-Mme-Nas-Qualify",@"User-Agent"]];
        if(![snapshot isKindOfClass:NSDictionary.class] || ![[NSSet setWithArray:snapshot.allKeys] isEqual:fields] || !input[@"icloud_body"])return 6;
        for(id key in snapshot)if(!bounded(snapshot[key],16384))return 6;
        NSMutableDictionary *selected=snapshot.mutableCopy;
        // The phone preserves native UA rather than forcing the nominal helper UA.
        [selected removeObjectForKey:@"User-Agent"];
        selected[@"X-Apple-MD"]=snapshot[@"X-Apple-I-MD"];
        selected[@"X-Apple-MD-M"]=snapshot[@"X-Apple-I-MD-M"];
        selected[@"Device-UDID"]=snapshot[@"X-Mme-Device-Id"];
        machine=selected;
    }
    if(!machine){emit(@{@"version":@1,@"nonce":input[@"nonce"],@"error":@"machine_metadata_unavailable",@"account_saved":@NO});return 1;}
    for(NSString *key in machine)[outgoing setValue:machine[key] forHTTPHeaderField:key];
    if(loopback) {
        // Fresh request: allHTTPHeaderFields assignment can MERGE native fields.
        NSData *body=outgoing.HTTPBody;
        NSString *content=[outgoing valueForHTTPHeaderField:@"Content-Type"];
        outgoing=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:@"http://127.0.0.1:49761/fixture"]];
        outgoing.HTTPMethod=@"POST";outgoing.HTTPBody=body;
        if(content)[outgoing setValue:content forHTTPHeaderField:@"Content-Type"];
    } else if(!snapshot) {
        __block NSData *signature=nil;__block BOOL signedData=NO;
        [[objc_getClass("AAAbsintheSigner") sharedSigner] signatureForData:outgoing.HTTPBody completion:^(id value,NSError *error){
            if(!error && [value isKindOfClass:NSData.class])signature=value;signedData=YES;
        }];
        NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:20];
        while(!signedData && deadline.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
        if(!signature.length){emit(@{@"version":@1,@"nonce":input[@"nonce"],@"error":@"native_signing_unavailable",@"account_saved":@NO});return 1;}
        [outgoing setValue:[signature base64EncodedStringWithOptions:0] forHTTPHeaderField:@"X-MMe-Nas-Qualify"];
        if(signingTest){emit(@{@"signature_present":@YES,@"request_sent":@NO,@"account_saved":@NO});return 0;}
    }
    if(snapshotTest){emit(@{@"snapshot_present":@(snapshot!=nil),@"signature_preserved":@([[outgoing valueForHTTPHeaderField:@"X-MMe-Nas-Qualify"] isEqual:snapshot[@"X-Mme-Nas-Qualify"]]),@"request_sent":@NO,@"account_saved":@NO});return snapshot?0:1;}
    outgoing.timeoutInterval=30;
    __block NSDictionary *result=nil;__block BOOL completed=NO;
    NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    [[session dataTaskWithRequest:outgoing completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
        // Public NSURLSession callback ABI; returned data/tokens are discarded.
        NSDictionary *summary=rw_icloud_response_summary(data);
        NSInteger status=[response isKindOfClass:NSHTTPURLResponse.class]?[(NSHTTPURLResponse *)response statusCode]:0;
        NSMutableArray *codes=[NSMutableArray array];
        if(error)[codes addObject:@{@"domain":[error.domain isEqual:@"NSURLErrorDomain"]?error.domain:@"other",@"code":@(error.code)}];
        result=@{@"version":@1,@"nonce":input[@"nonce"],@"http_status":@(status),@"errors":codes,@"response_present":@(response!=nil),@"account_saved":@NO,@"application_summary":summary};completed=YES;
    }] resume];
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:40];
    while(!completed && end.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    [session invalidateAndCancel];
    if(!completed){emit(@{@"version":@1,@"nonce":input[@"nonce"],@"error":@"timeout",@"account_saved":@NO});return 1;}
    emit(result);return 0;
}}

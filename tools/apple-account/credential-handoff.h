/* Ephemeral keyed comparisons only. Never persist credentials or digest values. */
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonHMAC.h>
#include <stdlib.h>
@interface RWCredentialHandoff : NSObject
- (void)captureUsername:(id)username password:(id)password dsid:(id)dsid;
- (void)captureAltDSID:(id)identifier;
- (NSDictionary *)compareAltDSID:(id)identifier;
- (NSDictionary *)compareAuthorization:(NSString *)header;
@end
@implementation RWCredentialHandoff {
    NSLock *_lock;
    NSMutableData *_key;
    NSData *_usernameDigest,*_dsidDigest,*_altDSIDDigest;
    NSDate *_expires;
}
- (instancetype)init { if((self=[super init]))_lock=[NSLock new];return self; }
- (NSData *)digest:(NSString *)text {
    if(![text isKindOfClass:NSString.class] || !text.length || text.length>65536)return nil;
    NSData *bytes=[text dataUsingEncoding:NSUTF8StringEncoding];
    unsigned char value[CC_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256,_key.bytes,_key.length,bytes.bytes,bytes.length,value);
    return [NSData dataWithBytes:value length:sizeof(value)];
}
- (NSData *)credentialDigest:(id)identity password:(NSString *)password {
    if([identity isKindOfClass:NSNumber.class])identity=[identity stringValue];
    if(![identity isKindOfClass:NSString.class] || ![identity length] || [identity length]>4096)return nil;
    NSString *pair=[identity stringByAppendingFormat:@":%@",password];
    NSString *header=[@"Basic " stringByAppendingString:[[pair dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0]];
    return [self digest:header];
}
- (void)captureUsername:(id)username password:(id)password dsid:(id)dsid {
    [_lock lock];
    if(_key.length)memset(_key.mutableBytes,0,_key.length);
    _key=[NSMutableData dataWithLength:32];arc4random_buf(_key.mutableBytes,_key.length);
    _usernameDigest=nil;_dsidDigest=nil;_altDSIDDigest=nil;_expires=nil;
    if([password isKindOfClass:NSString.class] && [password length] && [password length]<=16384) {
        _usernameDigest=[self credentialDigest:username password:password];
        _dsidDigest=[self credentialDigest:dsid password:password];
        _expires=[NSDate dateWithTimeIntervalSinceNow:120];
    }
    [_lock unlock];
}
- (void)captureAltDSID:(id)identifier {
    [_lock lock];
    _altDSIDDigest=_expires.timeIntervalSinceNow>0 && [identifier isKindOfClass:NSString.class] && [identifier length]<=4096 ? [self digest:[@"altDSID:" stringByAppendingString:identifier]]:nil;
    [_lock unlock];
}
- (NSDictionary *)compareAltDSID:(id)identifier {
    [_lock lock];
    BOOL fresh=_expires.timeIntervalSinceNow>0 && _altDSIDDigest!=nil;
    BOOL present=[identifier isKindOfClass:NSString.class] && [identifier length]>0;
    NSData *candidate=fresh && present && [identifier length]<=4096?[self digest:[@"altDSID:" stringByAppendingString:identifier]]:nil;
    NSDictionary *result=@{@"reference_available":@(fresh),@"header_present":@(present),@"matches_authkit":@(candidate && [candidate isEqual:_altDSIDDigest])};
    [_lock unlock];return result;
}
- (NSDictionary *)compareAuthorization:(NSString *)header {
    [_lock lock];
    BOOL fresh=_expires.timeIntervalSinceNow>0;
    if(!fresh) {
        if(_key.length)memset(_key.mutableBytes,0,_key.length);
        _key=nil;_usernameDigest=nil;_dsidDigest=nil;_altDSIDDigest=nil;_expires=nil;
    }
    NSData *actual=fresh?[self digest:header]:nil;
    NSDictionary *result=@{@"fresh_authkit_reference_available":@(fresh && (_usernameDigest || _dsidDigest)),
        @"authorization_present":@([header isKindOfClass:NSString.class] && header.length>0),
        @"authorization_matches_authkit_username_credential":@(actual && _usernameDigest && [actual isEqual:_usernameDigest]),
        @"authorization_matches_authkit_dsid_credential":@(actual && _dsidDigest && [actual isEqual:_dsidDigest])};
    [_lock unlock];return result;
}
@end

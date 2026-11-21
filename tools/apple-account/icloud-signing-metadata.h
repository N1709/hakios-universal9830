/* Narrow non-secret signing schema. Never forward account/credential payloads. */
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
static inline NSString *rw_lab_metadata_digest(NSData *bytes) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.bytes,(CC_LONG)bytes.length,digest);
    NSMutableString *result=[NSMutableString stringWithCapacity:64];
    for(unsigned i=0;i<sizeof(digest);i++)[result appendFormat:@"%02x",digest[i]];
    return result.copy;
}
static BOOL rw_lab_metadata_string(id value,NSString *pattern,NSUInteger limit) {
    if(![value isKindOfClass:NSString.class] || ![value length] || [value length]>limit)return NO;
    NSRegularExpression *expression=[NSRegularExpression regularExpressionWithPattern:pattern options:0 error:NULL];
    return [expression numberOfMatchesInString:value options:0 range:NSMakeRange(0,[value length])]==1;
}
static BOOL rw_auth_lab_valid_signing_metadata(NSData *bytes) {
    if(![bytes isKindOfClass:NSData.class] || !bytes.length || bytes.length>16384)return NO;
    id root=[NSPropertyListSerialization propertyListWithData:bytes options:0 format:NULL error:NULL];
    if(![root isKindOfClass:NSDictionary.class] || ![[NSSet setWithArray:[root allKeys]]
        isEqual:[NSSet setWithArray:@[@"protocolVersion",@"userInfo",@"delegates"]]] ||
        ![root[@"protocolVersion"] isEqual:@"1.0"])return NO;
    id user=root[@"userInfo"],delegates=root[@"delegates"];
    if(![user isKindOfClass:NSDictionary.class] || ![[NSSet setWithArray:[user allKeys]]
        isEqual:[NSSet setWithArray:@[@"client-id",@"language",@"timezone"]]])return NO;
    id identifier=user[@"client-id"];
    if(![identifier isKindOfClass:NSString.class] || [identifier length]!=36 ||
        ![[NSUUID alloc] initWithUUIDString:identifier] ||
        !rw_lab_metadata_string(user[@"language"],@"\\A[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8}){0,3}\\z",35) ||
        !rw_lab_metadata_string(user[@"timezone"],@"\\A[A-Za-z0-9_+./:-]+\\z",64))return NO;
    if(![delegates isKindOfClass:NSDictionary.class] || [delegates count]>3)return NO;
    for(id key in delegates) {
        id value=delegates[key];
        if(![value isKindOfClass:NSDictionary.class])return NO;
        if([key isEqual:@"com.apple.mobileme"] || [key isEqual:@"com.apple.gamecenter"]) { if([value count])return NO; }
        else if([key isEqual:@"com.apple.private.ids"]) {
            if([value count]!=1 || ![value[@"protocol-version"] isKindOfClass:NSString.class] ||
                ![@[@"1",@"2",@"3",@"4"] containsObject:value[@"protocol-version"]])return NO;
        } else return NO;
    }
    return YES;
}

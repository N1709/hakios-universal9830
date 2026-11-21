/* Synthetic schema/privacy tests. No accounts or network. */
#import <Foundation/Foundation.h>
#include "icloud-signing-metadata.h"
static NSData *encode(id value) {
    return [NSPropertyListSerialization dataWithPropertyList:value format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
}
int main(void) { @autoreleasepool {
    NSDictionary *user=@{@"client-id":@"123E4567-E89B-42D3-A456-426614174000",@"language":@"en-US",@"timezone":@"Asia/Kolkata"};
    NSDictionary *good=@{@"protocolVersion":@"1.0",@"userInfo":user,
        @"delegates":@{@"com.apple.mobileme":@{},@"com.apple.gamecenter":@{},@"com.apple.private.ids":@{@"protocol-version":@"4"}}};
    NSCAssert(rw_auth_lab_valid_signing_metadata(encode(good)),@"allow bounded device/service metadata");
    NSMutableDictionary *bad=good.mutableCopy;bad[@"password"]=@"synthetic-blocked";
    NSCAssert(!rw_auth_lab_valid_signing_metadata(encode(bad)),@"reject credential field");
    bad=good.mutableCopy;bad[@"delegates"]=@{@"com.apple.mobileme":@{@"token":@"synthetic-blocked"}};
    NSCAssert(!rw_auth_lab_valid_signing_metadata(encode(bad)),@"reject nested token field");
    bad=good.mutableCopy;NSMutableDictionary *badUser=user.mutableCopy;badUser[@"client-id"]=@"synthetic-account@example.invalid";bad[@"userInfo"]=badUser;
    NSCAssert(!rw_auth_lab_valid_signing_metadata(encode(bad)),@"reject account identity in metadata field");
    bad=good.mutableCopy;bad[@"delegates"]=@{@"com.apple.gamecenter":@{@"token":@"synthetic-blocked"}};
    NSCAssert(!rw_auth_lab_valid_signing_metadata(encode(bad)),@"reject Game Center credential payloads");
    bad=good.mutableCopy;bad[@"delegates"]=@{@"com.apple.unknown":@{}};
    NSCAssert(!rw_auth_lab_valid_signing_metadata(encode(bad)),@"reject unknown services");
    NSCAssert(!rw_auth_lab_valid_signing_metadata([@"arbitrary signing input" dataUsingEncoding:NSUTF8StringEncoding]),@"reject non-plist input");
    NSCAssert(!rw_auth_lab_valid_signing_metadata([NSMutableData dataWithLength:16385]),@"reject oversized input");
    puts("PASS: only bounded device/service metadata accepted; credential, token, identity and arbitrary payloads refused");
    return 0;
}}

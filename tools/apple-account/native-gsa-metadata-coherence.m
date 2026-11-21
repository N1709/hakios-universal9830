/* Compare native GSA metadata sources. Availability/equality only; no accounts. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface NSObject (GSAMetadataCoherence)
+ (id)currentDevice;
+ (id)machineUDID;
- (id)localUserUUID;
- (id)uniqueDeviceIdentifier;
- (id)serialNumber;
- (id)MLBSerialNumber;
- (id)ROMAddress;
- (id)_genericAppleIDHeadersDictionaryForRequest:(NSURLRequest *)request;
@end
static BOOL available(id value){return [value isKindOfClass:NSString.class] && [value length]>0;}
static NSString *kind(id value){return [value isKindOfClass:NSString.class]?@"string":[value isKindOfClass:NSData.class]?@"data":value?@"other":@"absent";}
int main(void) { @autoreleasepool {
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AOSKit.framework/AOSKit",RTLD_NOW);
    id device=[objc_getClass("AKDevice") currentDevice];
    id session=[objc_getClass("AKAppleIDSession") new];
    id headers=[session _genericAppleIDHeadersDictionaryForRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"https://gsa.apple.com/grandslam/GsService2"]]];
    if(![headers isKindOfClass:NSDictionary.class])return 1;
    NSMutableDictionary *out=[@{@"account_login_attempted":@NO,@"native_board_id_kind":kind([device MLBSerialNumber]),@"native_rom_kind":kind([device ROMAddress])} mutableCopy];
    id aosDevice=[objc_getClass("AOSUtilities") machineUDID];
    NSString *aosLocal=[aosDevice isKindOfClass:NSString.class]?[[aosDevice dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0]:nil;
    out[@"aos_device_id_matches_authkit"]=@([aosDevice isEqual:[device uniqueDeviceIdentifier]]);
    out[@"authkit_local_user_matches_aos_derived_local_user"]=@(aosLocal && [[device localUserUUID] isEqual:aosLocal]);
    out[@"authkit_local_user_length"]=@([[device localUserUUID] length]);
    out[@"aos_derived_local_user_length"]=@(aosLocal.length);
    NSDictionary *sources=@{@"X-Mme-Device-Id":[device uniqueDeviceIdentifier]?:NSNull.null,@"X-Apple-I-MD-LU":[device localUserUUID]?:NSNull.null,@"X-Apple-I-SRL-NO":[device serialNumber]?:NSNull.null,@"X-Apple-I-MLB":[device MLBSerialNumber]?:NSNull.null,@"X-Apple-I-ROM":[device ROMAddress]?:NSNull.null};
    for(NSString *name in sources)out[name]=@{@"native_header_present":@(available(headers[name])),@"device_value_present":@(available(sources[name])),@"header_matches_device_value":@(available(headers[name]) && [headers[name] isEqual:sources[name]])};
    NSData *json=[NSJSONSerialization dataWithJSONObject:out options:0 error:NULL];fwrite(json.bytes,1,json.length,stdout);putchar('\n');return 0;
}}

/* Compare native generic/account-scoped machine proofs; boolean output only.
 * Reads only PersonID from the already signed-in Mac's account preferences.
 * No account-token, password or keychain APIs. Never performs a login. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface NSObject (DSIDScope)
+ (id)retrieveOTPHeadersForDSID:(id)dsid;
- (void)aa_addDeviceProvisioningInfoHeadersWithDSID:(id)dsid;
@end
int main(void) { @autoreleasepool {
    dlopen("/System/Library/PrivateFrameworks/AOSKit.framework/AOSKit",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
    id domain=[NSUserDefaults.standardUserDefaults persistentDomainForName:@"MobileMeAccounts"];
    id accounts=[domain isKindOfClass:NSDictionary.class]?domain[@"Accounts"]:nil;
    NSString *dsid=nil;
    if([accounts isKindOfClass:NSArray.class])for(id account in accounts) {
        if(![account isKindOfClass:NSDictionary.class])continue;
        id value=nil;
        for(NSString *key in @[@"PersonID",@"AccountDSID",@"DSID",@"dsid"])if(account[key]){value=account[key];break;}
        if([value isKindOfClass:NSNumber.class])value=[value stringValue];
        if(![value isKindOfClass:NSString.class] || ![value length] || [value length]>20)continue;
        if([value rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location!=NSNotFound)continue;
        dsid=value;break;
    }
    NSMutableDictionary *report=[@{@"account_dsid_available":@(dsid!=nil),@"account_login_attempted":@NO} mutableCopy];
    if(dsid) {
        Class utility=objc_getClass("AOSUtilities");
        NSDictionary *generic=[utility retrieveOTPHeadersForDSID:@"-2"];
        NSDictionary *scoped=[utility retrieveOTPHeadersForDSID:dsid];
        BOOL a=[generic isKindOfClass:NSDictionary.class],b=[scoped isKindOfClass:NSDictionary.class];
        report[@"generic_proof_available"]=@(a && [generic[@"X-Apple-MD"] isKindOfClass:NSString.class] && [generic[@"X-Apple-MD"] length]>0);
        report[@"scoped_proof_available"]=@(b && [scoped[@"X-Apple-MD"] isKindOfClass:NSString.class] && [scoped[@"X-Apple-MD"] length]>0);
        NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:@"https://example.invalid/unsent"]];
        SEL selector=@selector(aa_addDeviceProvisioningInfoHeadersWithDSID:);
        if([request respondsToSelector:selector]) {
            [request aa_addDeviceProvisioningInfoHeadersWithDSID:dsid];
            report[@"native_scoped_proof_available"]=@([request valueForHTTPHeaderField:@"X-Apple-I-MD"].length>0);
            report[@"native_scoped_machine_identity_available"]=@([request valueForHTTPHeaderField:@"X-Apple-I-MD-M"].length>0);
            report[@"native_scoped_routing_available"]=@([request valueForHTTPHeaderField:@"X-Apple-I-MD-RINFO"].length>0);
            report[@"native_scoped_identity_matches_generic"]=@(generic[@"X-Apple-MD-M"] && [[request valueForHTTPHeaderField:@"X-Apple-I-MD-M"] isEqual:generic[@"X-Apple-MD-M"]]);
        }
        report[@"machine_provisioning_identity_matches"]=@(a && b && generic[@"X-Apple-MD-M"] && [generic[@"X-Apple-MD-M"] isEqual:scoped[@"X-Apple-MD-M"]]);
    }
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:0 error:NULL];
    fwrite(json.bytes,1,json.length,stdout);putchar('\n');return 0;
}}

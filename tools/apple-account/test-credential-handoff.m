#import <Foundation/Foundation.h>
#include "credential-handoff.h"
static NSString *basic(NSString *pair) {return [@"Basic " stringByAppendingString:[[pair dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0]];}
int main(void) { @autoreleasepool {
    RWCredentialHandoff *probe=[RWCredentialHandoff new];
    if([[probe compareAuthorization:basic(@"fixture:password")][@"fresh_authkit_reference_available"] boolValue])return 1;
    [probe captureUsername:@"fixture" password:@"synthetic-only" dsid:@123];
    [probe captureAltDSID:@"synthetic-alt-id"];
    if(![[probe compareAltDSID:@"synthetic-alt-id"][@"matches_authkit"] boolValue] || [[probe compareAltDSID:@"wrong"][@"matches_authkit"] boolValue])return 6;
    NSDictionary *u=[probe compareAuthorization:basic(@"fixture:synthetic-only")];
    NSDictionary *d=[probe compareAuthorization:basic(@"123:synthetic-only")];
    NSDictionary *bad=[probe compareAuthorization:basic(@"fixture:wrong")];
    if(![u[@"authorization_matches_authkit_username_credential"] boolValue] || ![d[@"authorization_matches_authkit_dsid_credential"] boolValue] || [bad[@"authorization_matches_authkit_username_credential"] boolValue])return 2;
    [probe setValue:[NSDate distantPast] forKey:@"expires"];
    if([[probe compareAuthorization:basic(@"fixture:synthetic-only")][@"fresh_authkit_reference_available"] boolValue])return 4;
    [probe captureUsername:@"fixture" password:@"different" dsid:@123];
    if([[probe compareAuthorization:basic(@"fixture:synthetic-only")][@"authorization_matches_authkit_username_credential"] boolValue])return 5;
    [probe captureUsername:nil password:nil dsid:nil];
    if([[probe compareAuthorization:basic(@"fixture:synthetic-only")][@"fresh_authkit_reference_available"] boolValue])return 3;
    puts("PASS: keyed handoff comparison distinguishes username/DSID, rejects mismatch, clears stale reference; boolean output only");return 0;
}}

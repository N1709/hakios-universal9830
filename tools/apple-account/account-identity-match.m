/* User-requested identity comparison only. Expected email arrives over stdin;
 * output is counts/booleans. Never access credentials, tokens or keychains. */
#import <Foundation/Foundation.h>
#import <Accounts/Accounts.h>
#include <TargetConditionals.h>
#include <stdio.h>
static BOOL matches(id value,NSString *expected) {
    return [value isKindOfClass:NSString.class] &&
        [[value stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
            caseInsensitiveCompare:expected]==NSOrderedSame;
}
int main(void) { @autoreleasepool {
    char line[512];if(!fgets(line,sizeof(line),stdin))return 2;
    NSString *expected=[[NSString stringWithUTF8String:line]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if(!expected.length || ![expected containsString:@"@"])return 2;
    ACAccountStore *store=[ACAccountStore new];
    NSMutableDictionary *report=[NSMutableDictionary dictionary];
    for(NSString *identifier in @[@"com.apple.account.AppleAccount",@"com.apple.account.idms"]) {
        ACAccountType *type=[store accountTypeWithAccountTypeIdentifier:identifier];
        NSArray *accounts=type?[store accountsWithAccountType:type]:nil;
        NSUInteger count=0;for(ACAccount *account in accounts)if(matches(account.username,expected))count++;
        report[identifier]=@{@"visible_count":@(accounts.count),@"matching_count":@(count)};
    }
#if TARGET_OS_OSX
    // macOS may deny account-store access to a plain diagnostic executable.
    // Read only the identity field from user preferences as an independent check.
    id domain=[NSUserDefaults.standardUserDefaults persistentDomainForName:@"MobileMeAccounts"];
    id accounts=[domain isKindOfClass:NSDictionary.class]?domain[@"Accounts"]:nil;
    NSUInteger count=0,seen=0;
    if([accounts isKindOfClass:NSArray.class])for(id entry in accounts) {
        if(![entry isKindOfClass:NSDictionary.class])continue;
        id identifier=entry[@"AccountID"];
        if([identifier isKindOfClass:NSString.class])seen++;
        if(matches(identifier,expected))count++;
    }
    report[@"macos_account_identity_preferences"]=@{@"visible_count":@(seen),@"matching_count":@(count)};
#endif
    NSData *output=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
    if(output)fwrite(output.bytes,1,output.length,stdout);
    return output?0:1;
}}

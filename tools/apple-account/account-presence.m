/* Account-type counts only: never access names, IDs, credentials or tokens. */
#import <Foundation/Foundation.h>
#import <Accounts/Accounts.h>
int main(void) { @autoreleasepool {
    ACAccountStore *store=[ACAccountStore new];
    NSMutableDictionary *result=[NSMutableDictionary dictionary];
    for (NSString *identifier in @[@"com.apple.account.AppleAccount", @"com.apple.account.idms"]) {
        ACAccountType *type=[store accountTypeWithAccountTypeIdentifier:identifier];
        NSArray *accounts=type?[store accountsWithAccountType:type]:nil;
        result[identifier]=@{@"type_available":@(type!=nil), @"count":@(accounts.count)};
    }
    NSData *data=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:NULL];
    if(data) fwrite(data.bytes,1,data.length,stdout);
    return data?0:1;
}}

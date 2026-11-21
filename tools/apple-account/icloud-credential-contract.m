/* Synthetic in-memory account only. Never save, authenticate, or send requests. */
#import <Foundation/Foundation.h>
#import <Accounts/Accounts.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface ACAccountCredential (SyntheticPassword)
- (id)initWithPassword:(NSString *)password;
@end
@interface NSObject (CredentialContract)
- (id)initWithAccount:(id)account delegates:(id)delegates;
- (NSURLRequest *)urlRequest;
@end
int main(int argc,const char **argv) { @autoreleasepool {
    dlopen("/System/Library/PrivateFrameworks/IntlPreferences.framework/IntlPreferences",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
    if(argc==2) {
        void *library=dlopen(argv[1],RTLD_NOW);
        int (*install)(void)=library?(void *)dlsym(library,"rw_auth_lab_install"):NULL;
        if(!install || !install())return 2;
    } else if(argc!=1)return 2;
    ACAccountStore *store=[ACAccountStore new];
    ACAccountType *type=[store accountTypeWithAccountTypeIdentifier:@"com.apple.account.AppleAccount"];
    if(!type)return 3;
    ACAccount *account=[[ACAccount alloc] initWithAccountType:type];
    NSString *username=@"fixture@example.invalid",*password=@"synthetic-fixture-only";
    account.username=username;
    if(![ACAccountCredential instancesRespondToSelector:@selector(initWithPassword:)])return 5;
    account.credential=[[ACAccountCredential alloc] initWithPassword:password];
    id object=[[objc_getClass("AALoginAccountRequest") alloc] initWithAccount:account delegates:@{@"com.apple.mobileme":@{}}];
    NSURLRequest *request=[object urlRequest];
    if(!request)return 4;
    NSString *expected=[@"Basic " stringByAppendingString:[[[username stringByAppendingFormat:@":%@",password] dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0]];
    NSString *actual=[request valueForHTTPHeaderField:@"Authorization"];
    NSDictionary *report=@{@"synthetic_account_only":@YES,@"request_sent":@NO,@"account_saved":@NO,
        @"authorization_present":@(actual.length>0),@"authorization_matches_synthetic_credential":@([actual isEqual:expected]),
        @"adapter_loaded":@(argc==2)};
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:0 error:NULL];
    fwrite(json.bytes,1,json.length,stdout);putchar('\n');
    return [actual isEqual:expected]?0:1;
}}

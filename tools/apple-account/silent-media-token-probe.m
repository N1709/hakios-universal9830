/* One owner-authorized native service-token check. No UI or raw token output.
 * Uses existing account through native AuthKit; never reads stored credentials. */
#import <Foundation/Foundation.h>
#import <Accounts/Accounts.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface NSObject (RWSilentTokens)
- (id)aa_altDSID;
- (void)setAltDSID:(id)value;
- (void)setServiceIdentifiers:(id)value;
- (void)setShouldPreventInteractiveAuth:(BOOL)value;
- (void)setShouldUpdatePersistentServiceTokens:(BOOL)value;
- (BOOL)isContextEligibleForSilentAuth;
- (void)authenticateWithContext:(id)context completion:(void (^)(id,NSError *))reply;
@end
int main(void){@autoreleasepool{
 dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
 dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
 ACAccountStore *store=[ACAccountStore new];
 id type=[store accountTypeWithAccountTypeIdentifier:@"com.apple.account.AppleAccount"];
 NSArray *accounts=type?[store accountsWithAccountType:type]:nil;
 id account=accounts.count==1?accounts.firstObject:nil;
 id identifier=[account respondsToSelector:@selector(aa_altDSID)]?[account aa_altDSID]:nil;
 if(![identifier isKindOfClass:NSString.class] || ![identifier length]){puts("{\"started\":false,\"reason\":\"account_identifier_unavailable\"}");return 1;}
 id context=[NSClassFromString(@"AKAppleIDAuthenticationContext") new];
 Method prevent=class_getInstanceMethod([context class],@selector(setShouldPreventInteractiveAuth:));
 Method persist=class_getInstanceMethod([context class],@selector(setShouldUpdatePersistentServiceTokens:));
 if(!prevent || !persist || strcmp(method_getTypeEncoding(prevent),"v20@0:8B16") || strcmp(method_getTypeEncoding(persist),"v20@0:8B16"))return 2;
 [context setAltDSID:identifier];[context setServiceIdentifiers:@[@"com.apple.gs.itunes.auth"]];
 [context setShouldPreventInteractiveAuth:YES];[context setShouldUpdatePersistentServiceTokens:NO];
 if(![context isContextEligibleForSilentAuth]){puts("{\"started\":false,\"reason\":\"native_context_not_silent_eligible\"}");return 1;}
 __block BOOL done=NO;__block NSDictionary *summary=nil;
 [[NSClassFromString(@"AKAppleIDAuthenticationController") new] authenticateWithContext:context completion:^(id result,NSError *error){
  NSMutableArray *errors=[NSMutableArray array];
  for(unsigned i=0;i<4 && [error isKindOfClass:NSError.class];i++){
   NSString *domain=error.domain.length<=128?error.domain:@"other";
   [errors addObject:@{@"domain":domain,@"code":@(error.code)}];error=error.userInfo[NSUnderlyingErrorKey];
  }
  summary=@{@"started":@YES,@"interactive_allowed":@NO,@"persistent_updates_requested":@NO,@"result_present":@(result!=nil),@"errors":errors};done=YES;
 }];
 NSDate *end=[NSDate dateWithTimeIntervalSinceNow:20];
 while(!done && end.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
 if(!done)summary=@{@"started":@YES,@"interactive_allowed":@NO,@"timed_out":@YES};
 NSData *out=[NSJSONSerialization dataWithJSONObject:summary options:0 error:NULL];if(out)fwrite(out.bytes,1,out.length,stdout);return done?0:1;
}}

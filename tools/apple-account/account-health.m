/* Read-only account counts and native boolean state. No identities/credentials. */
#import <Foundation/Foundation.h>
#import <Accounts/Accounts.h>
#import <objc/runtime.h>
int main(void){@autoreleasepool{
 ACAccountStore *store=[ACAccountStore new];
 ACAccountType *type=[store accountTypeWithAccountTypeIdentifier:@"com.apple.account.AppleAccount"];
 NSArray *accounts=type?[store accountsWithAccountType:type]:nil;
 NSMutableArray *states=[NSMutableArray array];
 for(id account in accounts){
  NSMutableDictionary *state=[NSMutableDictionary dictionary];
  for(NSString *name in @[@"isAuthenticated",@"isWarmingUp",@"isActive",@"isEnabled"]){
   SEL selector=sel_registerName(name.UTF8String);Method method=class_getInstanceMethod([account class],selector);
   const char *encoding=method?method_getTypeEncoding(method):NULL;
   if(encoding && (!strcmp(encoding,"B16@0:8") || !strcmp(encoding,"c16@0:8"))){
    BOOL value=((BOOL(*)(id,SEL))method_getImplementation(method))(account,selector);state[name]=@(value);
   }
  }[states addObject:state];
 }
 NSDictionary *report=@{@"account_count":@(accounts.count),@"states":states,@"credentials_accessed":@NO};
 NSData *json=[NSJSONSerialization dataWithJSONObject:report options:0 error:NULL];
 if(json)fwrite(json.bytes,1,json.length,stdout);return json?0:1;
}}

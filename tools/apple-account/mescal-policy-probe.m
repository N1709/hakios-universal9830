/* Summarize public bag signing policy only. No account or request-body access. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface NSObject (RWBagProbe)
+ (id)createBagForSubProfile;
- (id)dictionaryForKey:(id)key;
- (id)valuePromise;
- (BOOL)isFinished;
- (id)resultWithError:(NSError **)error;
@end
static void summarize(id value,NSMutableDictionary *counts,unsigned depth){
 if(depth>10)return;
 if([value isKindOfClass:NSDictionary.class]){
  for(NSString *key in @[@"fields",@"headers"]){
   id list=value[key];if(![list isKindOfClass:NSArray.class])continue;
   counts[@"policy_lists"]=@([counts[@"policy_lists"] integerValue]+1);
   for(id name in list)if([name isKindOfClass:NSString.class]){
    NSString *lower=[name lowercaseString];
    NSString *category=[lower containsString:@"password"]?@"password_fields":([lower containsString:@"token"]?@"token_fields":@"other_fields");
    counts[category]=@([counts[category] integerValue]+1);
   }
  }
  for(id child in [value allValues])summarize(child,counts,depth+1);
 }else if([value isKindOfClass:NSArray.class])for(id child in value)summarize(child,counts,depth+1);
}
int main(void){@autoreleasepool{
 dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 Class cls=NSClassFromString(@"AMSMescal");
 if(![cls respondsToSelector:@selector(createBagForSubProfile)])return 3;
 id bag=[cls createBagForSubProfile];id value=[bag dictionaryForKey:@"signed-actions"];id promise=[value valuePromise];
 if(![promise respondsToSelector:@selector(isFinished)] || ![promise respondsToSelector:@selector(resultWithError:)])return 4;
 NSDate *end=[NSDate dateWithTimeIntervalSinceNow:15];
 while(![promise isFinished] && end.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
 NSMutableDictionary *report=[@{@"completed":@([promise isFinished]),@"account_login_attempted":@NO} mutableCopy];
 if([promise isFinished]){
  NSError *error=nil;id policy=[promise resultWithError:&error];
  report[@"dictionary_available"]=@([policy isKindOfClass:NSDictionary.class]);
  if(error)report[@"error_code"]=@(error.code);
  summarize(policy,report,0);
 }
 NSData *out=[NSJSONSerialization dataWithJSONObject:report options:0 error:NULL];
 if(out)fwrite(out.bytes,1,out.length,stdout);return out?0:1;
}}

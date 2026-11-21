/* Owner's native Mac signing availability. Synthetic literal input only.
 * No account login, account-store/keychain APIs, token fetch or signature output.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>
@interface NSObject (RWNativeMescalProbe)
+ (id)defaultSession;
- (id)signData:(NSData *)data bag:(id)bag;
- (BOOL)isFinished;
- (id)resultWithError:(NSError **)error;
@end
static BOOL methodOK(Class cls,SEL sel,const char *type){
 Method m=class_getInstanceMethod(cls,sel);return m && !strcmp(method_getTypeEncoding(m),type);
}
int main(int argc,const char **argv){@autoreleasepool{
 if(argc>2 || (argc==2 && strcmp(argv[1],"--synthetic-sign")))return 2;
 dlopen("/System/Library/PrivateFrameworks/PodcastsFoundation.framework/PodcastsFoundation",RTLD_NOW);
 dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 Class sessionClass=NSClassFromString(@"AMSMescalSession"),bagClass=NSClassFromString(@"IMURLBag");
 Method factory=class_getClassMethod(sessionClass,@selector(defaultSession));
 BOOL available=factory && !strcmp(method_getTypeEncoding(factory),"@16@0:8") &&
   methodOK(sessionClass,@selector(signData:bag:),"@32@0:8@16@24") && bagClass;
 NSMutableDictionary *report=[@{@"native_session_available":@(available),@"synthetic_sign_requested":@(argc==2),@"account_login_attempted":@NO} mutableCopy];
 if(available && argc==2){
  id session=[sessionClass defaultSession];id bag=[bagClass new];
  NSData *data=[@"redwood-account-lab/synthetic-signing-check/v1" dataUsingEncoding:NSUTF8StringEncoding];
  id promise=[session signData:data bag:bag];Class cls=[promise class];
  BOOL compatible=methodOK(cls,@selector(resultWithError:),"@24@0:8^@16") &&
   (methodOK(cls,@selector(isFinished),"B16@0:8") || methodOK(cls,@selector(isFinished),"c16@0:8"));
  report[@"promise_interface_matches"]=@(compatible);
  if(compatible){
   NSDate *end=[NSDate dateWithTimeIntervalSinceNow:15];
   while(![promise isFinished] && end.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
   BOOL finished=[promise isFinished];report[@"completed"]=@(finished);
   if(finished){
    NSError *error=nil;id result=[promise resultWithError:&error];
    report[@"signature_present"]=@([result isKindOfClass:NSData.class] && [result length]>0);
    NSMutableArray *errors=[NSMutableArray array];
    for(unsigned i=0;i<4 && [error isKindOfClass:NSError.class];i++){
     NSString *domain=[@[@"AMSErrorDomain",@"NSURLErrorDomain",@"NSCocoaErrorDomain"] containsObject:error.domain]?error.domain:@"other";
     [errors addObject:@{@"domain":domain,@"code":@(error.code)}];error=error.userInfo[NSUnderlyingErrorKey];
    }report[@"errors"]=errors;
   }
  }
 }
 NSData *out=[NSJSONSerialization dataWithJSONObject:report options:0 error:NULL];
 if(out)fwrite(out.bytes,1,out.length,stdout);return out?0:1;
}}

/* Local native AARequest/parser ABI test. No account, credentials or Apple request. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface NSObject (RWNativeRequest)
- (id)initWithURLString:(NSString *)url;
- (void)performRequestWithSession:(id)session withHandler:(void (^)(id,id,NSError *))completion;
- (NSError *)error;
- (NSHTTPURLResponse *)httpResponse;
@end
int main(int argc,const char **argv){@autoreleasepool{
 if(argc<2 || argc>3)return 2;
 dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
 void *lib=dlopen(argv[1],RTLD_NOW);id (*factory)(void)=lib?(void *)dlsym(lib,argc==3?argv[2]:"rw_public_session_fixture_proxy"):NULL;
 if(!factory)return 3;
 for(NSNumber *code in @[@200,@401]) {
  id request=[[objc_getClass("AARequest") alloc] initWithURLString:[NSString stringWithFormat:@"http://127.0.0.1:49763/%@",code]];
  __block BOOL completed=NO,identity=NO;__block NSInteger actual=0;
  [request performRequestWithSession:factory() withHandler:^(id native,id response,NSError *error){
   identity=native==request && response!=nil && error==nil;
   actual=[[response httpResponse] statusCode];completed=YES;
  }];
  NSDate *limit=[NSDate dateWithTimeIntervalSinceNow:8];
  while(!completed && limit.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
  if(!completed || !identity || actual!=code.integerValue){printf("FAIL native callback completed=%d identity=%d actual=%ld expected=%ld\n",completed,identity,(long)actual,(long)code.integerValue);return 1;}
 }
 puts("PASS: native AARequest callback request/response/error ABI preserved for local HTTP200 and401; no account or Apple request");return 0;
}}

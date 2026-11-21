#import <Foundation/Foundation.h>
#include <dlfcn.h>
typedef struct __AppleIDAuthSupportTokenData *RWToken;
static id expectedURL,expectedServices,expectedParams,expectedResult;static NSError *expectedError;static int calls;
@interface RWTokenNative:NSObject
- (id)appleIDAuthSupportCopyAppTokensWithMasterToken:(RWToken)t authURL:(id)u serviceIds:(id)s authParams:(id)p error:(NSError **)e;
@end
@implementation RWTokenNative
- (id)appleIDAuthSupportCopyAppTokensWithMasterToken:(RWToken)t authURL:(id)u serviceIds:(id)s authParams:(id)p error:(NSError **)e{
 NSCAssert(t==(RWToken)0x1234 && u==expectedURL && s==expectedServices && p==expectedParams,@"native args changed");calls++;if(e)*e=expectedError;return expectedResult;
}
@end
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;
 expectedURL=[NSURL URLWithString:@"https://example.invalid/unsent"];expectedServices=@[@"synthetic"];expectedParams=@{@"private":@"synthetic-no-account"};
 void *lib=dlopen(argv[1],RTLD_NOW);BOOL(*install)(void)=lib?dlsym(lib,"rw_app_token_probe_install"):NULL;if(!install || !install())return 3;
 RWTokenNative *n=[RWTokenNative new];
 for(unsigned i=0;i<2;i++){
  expectedResult=i?nil:@{@"synthetic":@YES};expectedError=i?[NSError errorWithDomain:@"com.apple.Synthetic" code:2 userInfo:nil]:nil;
  NSError *error=nil;id result=[n appleIDAuthSupportCopyAppTokensWithMasterToken:(RWToken)0x1234 authURL:expectedURL serviceIds:expectedServices authParams:expectedParams error:&error];
  NSCAssert(result==expectedResult && error==expectedError,@"native result/error changed");
 }
 NSCAssert(calls==2,@"wrong call count");puts("PASS: token-service arguments, success result and failure NSError identity unchanged; no authentication");return 0;
}}

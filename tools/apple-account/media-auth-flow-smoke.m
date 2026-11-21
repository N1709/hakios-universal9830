/* Synthetic method ABI/identity tests; no Apple framework or account operations. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
static id expected,controller,context,update,account;static BOOL decision;static int calls;
@interface AMSAuthKitUpdateTask:NSObject
- (BOOL)authenticationController:(id)c shouldContinueWithAuthenticationResults:(id)r error:(id)e forContext:(id)x;
@end
@implementation AMSAuthKitUpdateTask
- (BOOL)authenticationController:(id)c shouldContinueWithAuthenticationResults:(id)r error:(id)e forContext:(id)x{
 NSCAssert(c==controller && r==expected && e==nil && x==context,@"delegate args changed");calls++;return decision;
}
@end
@interface AMSAuthenticateTask:NSObject
- (void)_processAuthKitUpdateResult:(id)r;
- (void)_finishWithVerifiedAccount:(id)a andAuthKitUpdateResult:(id)r;
@end
@implementation AMSAuthenticateTask
- (void)_processAuthKitUpdateResult:(id)r{NSCAssert(r==update,@"update identity changed");calls++;}
- (void)_finishWithVerifiedAccount:(id)a andAuthKitUpdateResult:(id)r{NSCAssert(a==account && r==update,@"finish args changed");calls++;}
@end
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;
 expected=@{@"synthetic":@YES};controller=[NSObject new];context=[NSObject new];update=[NSObject new];account=[NSObject new];
 void *lib=dlopen(argv[1],RTLD_NOW);BOOL(*install)(void)=lib?dlsym(lib,"rw_media_auth_flow_install"):NULL;if(!install || !install())return 3;
 AMSAuthKitUpdateTask *task=[AMSAuthKitUpdateTask new];
 decision=YES;NSCAssert([task authenticationController:controller shouldContinueWithAuthenticationResults:expected error:nil forContext:context],@"true decision changed");
 decision=NO;NSCAssert(![task authenticationController:controller shouldContinueWithAuthenticationResults:expected error:nil forContext:context],@"false decision changed");
 AMSAuthenticateTask *auth=[AMSAuthenticateTask new];[auth _processAuthKitUpdateResult:update];[auth _finishWithVerifiedAccount:account andAuthKitUpdateResult:update];
 NSCAssert(calls==4,@"wrong original call count");puts("PASS: native YES/NO decisions, callback arguments and account/update object identities preserved; synthetic classes only");return 0;
}}

/* Unsent synthetic requests through real private AMD provider; no account login. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
static id expectedProperties;static NSError *nativeError;static NSUInteger calls;
@interface AMSURLSession:NSObject
- (id)_prepareRequest:(id)request properties:(id)properties error:(NSError **)error;
@end
@implementation AMSURLSession
- (id)_prepareRequest:(id)request properties:(id)properties error:(NSError **)error{
 NSCAssert(properties==expectedProperties,@"native properties changed");calls++;if(error)*error=nativeError;return nativeError?nil:request;
}
@end
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;expectedProperties=[NSObject new];
 void *lib=dlopen(argv[1],RTLD_NOW);BOOL(*install)(void)=lib?dlsym(lib,"rw_store_amd_install"):NULL;if(!install || !install())return 3;
 AMSURLSession *session=[AMSURLSession new];
 NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:@"https://auth.itunes.apple.com/auth/v1/native?guid=synthetic"]];
 request.HTTPMethod=@"POST";request.HTTPBody=[@"synthetic-private-body-never-forwarded" dataUsingEncoding:NSUTF8StringEncoding];
 [request setValue:@"synthetic-sap" forHTTPHeaderField:@"X-Apple-ActionSignature"];
 [request setValue:@"synthetic-i-md" forHTTPHeaderField:@"X-Apple-I-MD"];
 NSError *error=nil;NSURLRequest *result=[session _prepareRequest:request properties:expectedProperties error:&error];
 NSCAssert(result && !error && [result.URL isEqual:request.URL] && [result.HTTPMethod isEqual:request.HTTPMethod] && [result.HTTPBody isEqual:request.HTTPBody],@"native request altered");
 for(NSString *field in @[@"X-Apple-ActionSignature",@"X-Apple-I-MD"])NSCAssert([[result valueForHTTPHeaderField:field] isEqual:[request valueForHTTPHeaderField:field]],@"native proof overwritten");
 for(NSString *field in @[@"X-Apple-AMD",@"X-Apple-AMD-M"])NSCAssert([result valueForHTTPHeaderField:field].length>0,@"missing Store AMD");
 NSCAssert([request valueForHTTPHeaderField:@"X-Apple-AMD"]==nil,@"original request mutated");
 for(NSString *url in @[@"https://auth.itunes.apple.com.evil.invalid/auth/v1/native",@"http://auth.itunes.apple.com/auth/v1/native",@"https://auth.itunes.apple.com/auth/v1/native/unlisted",@"https://setup.icloud.com/setup/signin/v2/login"]){
  NSURLRequest *outside=[NSURLRequest requestWithURL:[NSURL URLWithString:url]];
  NSCAssert([session _prepareRequest:outside properties:expectedProperties error:&error]==outside,@"out-of-scope request changed");
 }
 nativeError=[NSError errorWithDomain:@"com.redwood.Synthetic" code:42 userInfo:nil];
 NSCAssert([session _prepareRequest:request properties:expectedProperties error:&error]==nil && error==nativeError,@"native failure changed");
 NSCAssert(calls==6,@"wrong native call count");
 puts("PASS: genuine Store AMD pair added; body/URL/query/SAP/I-MD/native error preserved; hostile endpoints refused; NO login request sent");return 0;
}}

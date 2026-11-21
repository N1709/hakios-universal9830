/* Local-only HTTP test; no account/Apple connection or credentials. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdlib.h>
@interface RWDelegate:NSObject<NSURLSessionDelegate>
@end
@implementation RWDelegate
@end
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;
 setenv("RW_MEDIA_SESSION_SELFTEST","1",1);
 RWDelegate *delegate=[RWDelegate new];NSOperationQueue *queue=[NSOperationQueue new];
 NSURLSessionConfiguration *config=NSURLSessionConfiguration.ephemeralSessionConfiguration;
 config.HTTPAdditionalHeaders=@{@"X-Synthetic-Fixture":@"preserved"};
 NSURLSession *session=[NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:queue];
 void *library=dlopen(argv[1],RTLD_NOW);BOOL(*install)(void)=library?dlsym(library,"rw_media_session_isolation_install"):NULL;if(!install || !install())return 3;
 for(NSNumber *status in @[@200,@401]){
  NSURL *url=[NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:49763/%@",status]];
  dispatch_semaphore_t done=dispatch_semaphore_create(0);__block BOOL passed=NO;
  [[session dataTaskWithRequest:[NSURLRequest requestWithURL:url] completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
   passed=!error && [response isKindOfClass:NSHTTPURLResponse.class] && [(NSHTTPURLResponse *)response statusCode]==status.integerValue && [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] isEqual:@"fixture-preserved"];
   dispatch_semaphore_signal(done);
  }] resume];
  if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC)) || !passed){puts("FAIL: native HTTP completion changed");return 1;}
 }
 [session invalidateAndCancel];puts("PASS: isolated native session preserves request/header delivery and real HTTP200/401 completion; no account or Apple request");return 0;
}}

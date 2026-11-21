/* Real localhost traffic only; verifies capture without altering native bytes. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdlib.h>
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;setenv("RW_TRANSACTION_SELFTEST","1",1);
 NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
 void *lib=dlopen(argv[1],RTLD_NOW);BOOL(*install)(void)=lib?dlsym(lib,"rw_transaction_recorder_install"):NULL;if(!install || !install())return 3;
 NSMutableData *expected=[NSMutableData dataWithLength:160000];memset(expected.mutableBytes,'R',expected.length);[expected appendData:[@":private-fixture" dataUsingEncoding:NSUTF8StringEncoding]];
 for(NSNumber *status in @[@200,@401]){
  NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:49763/%@",status]]];
  request.HTTPMethod=@"POST";request.HTTPBody=[@"SYNTHETIC_REQUEST_SECRET" dataUsingEncoding:NSUTF8StringEncoding];
  dispatch_semaphore_t done=dispatch_semaphore_create(0);__block BOOL passed=NO;
  [[session dataTaskWithRequest:request completionHandler:^(NSData *bytes,NSURLResponse *response,NSError *error){
   passed=!error && [response isKindOfClass:NSHTTPURLResponse.class] && [(NSHTTPURLResponse *)response statusCode]==status.integerValue && [bytes isEqual:expected];dispatch_semaphore_signal(done);
  }] resume];
  if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,8*NSEC_PER_SEC)) || !passed){puts("FAIL: native response bytes/status changed");return 1;}
 }
 [session invalidateAndCancel];puts("PASS: native response/status unchanged for160KB HTTP200 and401; no account/Apple request");return 0;
}}

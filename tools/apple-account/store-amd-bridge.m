/* Attach genuine owner-Mac Store AMD type2 proofs at native request preparation.
 * No endpoint rewriting, request-body forwarding, SAP replacement or success stub. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include "shared-layer-scope.h"
static id(*nativePrepare)(id,SEL,id,id,NSError **);
static int output=-1;
static BOOL allowed(NSURL *url){
 return [url.scheme.lowercaseString isEqual:@"https"] && [url.host.lowercaseString isEqual:@"auth.itunes.apple.com"] && !url.user.length && !url.password.length && (!url.port || url.port.integerValue==443) &&
 [@[@"/auth/v1/native",@"/auth/v1/native/",@"/auth/v1/native/fast",@"/auth/v1/native/fast/"] containsObject:url.path];
}
static NSDictionary *proofs(NSURL *url,NSError **error){
 if(error)*error=nil;
 NSData *bytes=[NSData dataWithContentsOfFile:@"/var/mobile/Library/RedwoodAppleAccountLab/connection.json"];
 id config=bytes?[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL]:nil;
 if(![config isKindOfClass:NSDictionary.class] || ![config[@"enabled"] boolValue] || ![config[@"port"] isKindOfClass:NSNumber.class] || ![config[@"token"] isKindOfClass:NSString.class])return nil;
 NSInteger port=[config[@"port"] integerValue];if(port<1024 || port>65535)return nil;
 NSString *nonce=[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""];
 // Endpoint only; query, account, password and body stay on the phone.
 NSString *endpoint=[@"https://auth.itunes.apple.com" stringByAppendingString:url.path];
 NSDictionary *value=@{@"version":@1,@"nonce":nonce,@"operation":@"store-proof",@"endpoint":endpoint};
 NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:%ld/v1/media",(long)port]]];
 request.HTTPMethod=@"POST";request.HTTPBody=[NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];request.timeoutInterval=8;
 [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
 [request setValue:[@"Bearer " stringByAppendingString:config[@"token"]] forHTTPHeaderField:@"Authorization"];
 NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
 dispatch_semaphore_t done=dispatch_semaphore_create(0);__block NSData *data=nil;__block NSInteger status=0;
 [[session dataTaskWithRequest:request completionHandler:^(NSData *b,NSURLResponse *r,NSError *e){
  if(!e && [r isKindOfClass:NSHTTPURLResponse.class] && b.length<=65536){status=[(NSHTTPURLResponse *)r statusCode];data=b;}dispatch_semaphore_signal(done);
 }] resume];
 BOOL timeout=dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,9*NSEC_PER_SEC))!=0;[session invalidateAndCancel];
 id reply=!timeout && status==200 && data?[NSJSONSerialization JSONObjectWithData:data options:0 error:NULL]:nil;
 if(![reply isKindOfClass:NSDictionary.class] || ![reply[@"nonce"] isEqual:nonce] || ![reply[@"version"] isEqual:@1] || reply[@"error"])return nil;
 id headers=reply[@"headers"];
 if(![headers isKindOfClass:NSDictionary.class] || ![[NSSet setWithArray:[headers allKeys]] isEqual:[NSSet setWithArray:@[@"X-Apple-AMD",@"X-Apple-AMD-M"]]])return nil;
 for(id field in headers){id v=headers[field];if(![v isKindOfClass:NSString.class] || ![v length] || [v length]>16384 || [v rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location!=NSNotFound)return nil;}
 return headers;
}
static id prepare(id self,SEL cmd,id request,id properties,NSError **error){
 id result=nativePrepare(self,cmd,request,properties,error);
 if(![result isKindOfClass:NSURLRequest.class] || !allowed([result URL]) || (error && *error))return result;
 NSDictionary *headers=proofs([result URL],NULL);
 if(!headers){
  if(error)*error=[NSError errorWithDomain:@"AMSErrorDomain" code:12 userInfo:@{NSUnderlyingErrorKey:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCannotConnectToHost userInfo:nil]}];
  if(output>=0)dprintf(output,"{\"event\":\"store_amd_provider_unavailable\",\"pid\":%d}\n",getpid());return nil;
 }
 NSMutableURLRequest *copy=[result mutableCopy];
 for(NSString *field in headers)[copy setValue:headers[field] forHTTPHeaderField:field];
 if(output>=0)dprintf(output,"{\"event\":\"store_amd_headers_attached\",\"pid\":%d,\"body_preserved\":%s,\"sap_signature_preserved\":%s}\n",getpid(),[[copy HTTPBody] isEqual:[result HTTPBody]]?"true":"false",[[copy valueForHTTPHeaderField:@"X-Apple-ActionSignature"] isEqual:[result valueForHTTPHeaderField:@"X-Apple-ActionSignature"]]?"true":"false");
 return copy;
}
__attribute__((visibility("default"))) BOOL rw_store_amd_install(void){
 static BOOL installed=NO;if(installed)return YES;
 Method m=class_getInstanceMethod(NSClassFromString(@"AMSURLSession"),sel_registerName("_prepareRequest:properties:error:"));
 if(!m || strcmp(method_getTypeEncoding(m),"@40@0:8@16@24^@32"))return NO;
 nativePrepare=(void *)method_getImplementation(m);method_setImplementation(m,(IMP)prepare);installed=YES;return YES;
}
__attribute__((constructor)) static void start(void){
 if(!rw_shared_media_process())return;
 dispatch_async(dispatch_get_main_queue(),^{
  dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
  output=open("/var/mobile/Library/RedwoodAppleAccountLab/media-error-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  BOOL installed=rw_store_amd_install();if(output>=0)dprintf(output,"{\"event\":\"store_amd_bridge_installed\",\"installed\":%s,\"pid\":%d}\n",installed?"true":"false",getpid());
 });
}

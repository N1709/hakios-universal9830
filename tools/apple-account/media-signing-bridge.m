/* Owner-native Mac Mescal signing AND verification over private SSH broker.
 * No credential/proof logging or fabricated validation success. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include "shared-layer-scope.h"
static int output=-1;
static void note(const char *operation,BOOL success){
 if(output>=0)dprintf(output,"{\"event\":\"media_native_helper_result\",\"operation\":\"%s\",\"success\":%s,\"pid\":%d}\n",operation,success?"true":"false",getpid());
}
static NSError *failure(void){return [NSError errorWithDomain:@"AMSErrorDomain" code:12 userInfo:@{NSUnderlyingErrorKey:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCannotConnectToHost userInfo:nil]}];}
static NSDictionary *rpc(NSString *operation,NSData *data,NSData *signature,NSError **error){
 if(error)*error=nil;
 if(![data isKindOfClass:NSData.class] || !data.length || data.length>131072 || (signature && (![signature isKindOfClass:NSData.class] || !signature.length || signature.length>131072))){if(error)*error=failure();return nil;}
 NSData *configBytes=[NSData dataWithContentsOfFile:@"/var/mobile/Library/RedwoodAppleAccountLab/connection.json"];
 id config=configBytes?[NSJSONSerialization JSONObjectWithData:configBytes options:0 error:NULL]:nil;
 if(![config isKindOfClass:NSDictionary.class] || ![config[@"enabled"] boolValue] || ![config[@"token"] isKindOfClass:NSString.class] || ![config[@"port"] isKindOfClass:NSNumber.class]){if(error)*error=failure();return nil;}
 NSInteger port=[config[@"port"] integerValue];if(port<1024 || port>65535){if(error)*error=failure();return nil;}
 NSString *nonce=[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""];
 NSMutableDictionary *body=[@{@"version":@1,@"nonce":nonce,@"operation":operation,@"data":[data base64EncodedStringWithOptions:0]} mutableCopy];
 if(signature)body[@"signature"]=[signature base64EncodedStringWithOptions:0];
 NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:%ld/v1/media",(long)port]]];
 request.HTTPMethod=@"POST";request.timeoutInterval=8;request.HTTPBody=[NSJSONSerialization dataWithJSONObject:body options:0 error:NULL];
 [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
 [request setValue:[@"Bearer " stringByAppendingString:config[@"token"]] forHTTPHeaderField:@"Authorization"];
 NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
 dispatch_semaphore_t done=dispatch_semaphore_create(0);__block NSData *bytes=nil;__block NSInteger status=0;
 [[session dataTaskWithRequest:request completionHandler:^(NSData *value,NSURLResponse *response,NSError *transportError){
  if(!transportError && [response isKindOfClass:NSHTTPURLResponse.class]){status=[(NSHTTPURLResponse *)response statusCode];if(value.length<=200000)bytes=value;}
  dispatch_semaphore_signal(done);
 }] resume];
 BOOL timeout=dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,9*NSEC_PER_SEC))!=0;[session invalidateAndCancel];
 id result=!timeout && status==200 && bytes?[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL]:nil;
 if(![result isKindOfClass:NSDictionary.class] || ![result[@"version"] isEqual:@1] || ![result[@"nonce"] isEqual:nonce] || result[@"error"]){if(error)*error=failure();return nil;}
 return result;
}
static id signOperation(NSString *operation,NSData *data,NSError **error){
 NSDictionary *result=rpc(operation,data,nil,error);id encoded=result[@"signature"];
 NSData *signature=[encoded isKindOfClass:NSString.class] && [encoded length]<=180000?[[NSData alloc] initWithBase64EncodedString:encoded options:0]:nil;
 if(!signature.length || signature.length>131072){if(error)*error=failure();signature=nil;}
 note(operation.UTF8String,signature!=nil);return signature;
}
static id signData(__unused id self,__unused SEL cmd,NSData *data,__unused id bag,NSError **error){return signOperation(@"sign",data,error);}
static id primeData(__unused id self,__unused SEL cmd,NSData *data,__unused id bag,NSError **error){return signOperation(@"prime",data,error);}
static BOOL verifyOperation(NSString *operation,NSData *data,NSData *signature,NSError **error){
 NSDictionary *reply=rpc(operation,data,signature,error);
 BOOL valid=[reply[@"verified"] isKindOfClass:NSNumber.class] && [reply[@"verified"] boolValue];
 if(!valid && error)*error=failure();note(operation.UTF8String,valid);return valid;
}
static BOOL verifyData(__unused id self,__unused SEL cmd,NSData *data,NSData *signature,__unused id bag,NSError **error){return verifyOperation(@"verify",data,signature,error);}
static BOOL verifyPrime(__unused id self,__unused SEL cmd,NSData *signature,NSError **error){return verifyOperation(@"verify-prime",signature,nil,error);}
__attribute__((visibility("default"))) BOOL rw_media_signing_install(void){
 static BOOL installed=NO;if(installed)return YES;
 Class cls=NSClassFromString(@"AMSMescalSession");
 SEL selectors[]={@selector(signData:bag:error:),@selector(primeSignatureForData:bag:error:),@selector(verifyData:withSignature:bag:error:),@selector(verifyPrimeSignature:error:)};
 const char *types[]={"@40@0:8@16@24^@32","@40@0:8@16@24^@32","B48@0:8@16@24@32^@40","B32@0:8@16^@24"};
 IMP implementations[]={(IMP)signData,(IMP)primeData,(IMP)verifyData,(IMP)verifyPrime};
 for(unsigned i=0;i<4;i++){Method method=class_getInstanceMethod(cls,selectors[i]);if(!method || strcmp(method_getTypeEncoding(method),types[i]))return NO;}
 for(unsigned i=0;i<4;i++)method_setImplementation(class_getInstanceMethod(cls,selectors[i]),implementations[i]);
 installed=YES;return YES;
}
__attribute__((constructor)) static void start(void){
 if(!rw_shared_media_process())return;
 dispatch_async(dispatch_get_main_queue(),^{
  dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
  output=open("/var/mobile/Library/RedwoodAppleAccountLab/media-error-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  BOOL installed=rw_media_signing_install();if(output>=0)dprintf(output,"{\"event\":\"media_signing_bridge_installed\",\"installed\":%s,\"pid\":%d}\n",installed?"true":"false",getpid());
 });
}

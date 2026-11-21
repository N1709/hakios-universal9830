/* Private stdio RPC to owner's native Mescal implementation. No raw logging.
 * Inputs/proofs may be sensitive: stdin/stdout only through authenticated broker.
 * Uses Apple's real signing and verification; no fabricated success results. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
@interface NSObject (RWMediaProvider)
+ (id)_headersForRequest:(NSURLRequest *)request account:(id)account type:(NSInteger)type;
+ (id)defaultSession;
+ (id)primeSession;
- (id)signData:(NSData *)data bag:(id)bag;
- (id)primeSignatureForData:(NSData *)data bag:(id)bag;
- (id)verifyData:(NSData *)data withSignature:(NSData *)signature bag:(id)bag;
- (id)verifyPrimeSignature:(NSData *)signature;
- (BOOL)isFinished;
- (id)resultWithError:(NSError **)error;
+ (id)deviceGUID;
+ (id)createBagForSubProfile;
- (id)initWithBag:(id)bag;
- (id)initWithBundleIdentifier:(id)bundle;
- (void)setClientInfo:(id)info;
- (id)requestWithMethod:(NSInteger)method URL:(id)url parameters:(id)parameters error:(NSError **)error;
@end
static void emit(NSDictionary *value){
 NSData *bytes=[NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
 if(bytes){fwrite(bytes.bytes,1,bytes.length,stdout);fputc('\n',stdout);fflush(stdout);}
}
static NSData *decode(id value){
 if(![value isKindOfClass:NSString.class] || [value length]>180000)return nil;
 NSData *data=[[NSData alloc] initWithBase64EncodedString:value options:0];
 return data.length && data.length<=131072?data:nil;
}
static NSDictionary *perform(id request){
 NSString *nonce=[request isKindOfClass:NSDictionary.class]?request[@"nonce"]:nil;
 NSCharacterSet *allowed=[NSCharacterSet characterSetWithCharactersInString:@"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"];
 if(![nonce isKindOfClass:NSString.class] || nonce.length<16 || nonce.length>64 || [nonce rangeOfCharacterFromSet:allowed.invertedSet].location!=NSNotFound)return @{@"error":@"invalid_request"};
 NSMutableDictionary *reply=[@{@"version":@1,@"nonce":nonce} mutableCopy];
 NSString *op=request[@"operation"];
 if([op isEqual:@"store-device"]){
  if(![[NSSet setWithArray:[request allKeys]] isEqual:[NSSet setWithArray:@[@"version",@"nonce",@"operation"]]] || ![request[@"version"] isEqual:@1]){reply[@"error"]=@"invalid_request";return reply;}
  Class cls=NSClassFromString(@"AMSDevice");Method method=class_getClassMethod(cls,@selector(deviceGUID));
  if(!method || strcmp(method_getTypeEncoding(method),"@16@0:8")){reply[@"error"]=@"unsupported_interface";return reply;}
  id guid=[cls deviceGUID];
  if(![guid isKindOfClass:NSString.class] || [guid length]!=12 || [guid rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF"].invertedSet].location!=NSNotFound){reply[@"error"]=@"invalid_native_result";return reply;}
  reply[@"device_guid"]=guid;return reply;
 }
 if([op isEqual:@"store-proof"]){
  NSSet *keys=[NSSet setWithArray:[request allKeys]];
  NSString *endpoint=request[@"endpoint"];
  NSArray *allowed=@[@"https://auth.itunes.apple.com/auth/v1/native",@"https://auth.itunes.apple.com/auth/v1/native/",@"https://auth.itunes.apple.com/auth/v1/native/fast",@"https://auth.itunes.apple.com/auth/v1/native/fast/"];
  if(![keys isEqual:[NSSet setWithArray:@[@"version",@"nonce",@"operation",@"endpoint"]]] || ![request[@"version"] isEqual:@1] || ![allowed containsObject:endpoint]){reply[@"error"]=@"invalid_request";return reply;}
  Class cls=NSClassFromString(@"AMSAnisette");Method method=class_getClassMethod(cls,@selector(_headersForRequest:account:type:));
  if(!method || strcmp(method_getTypeEncoding(method),"@40@0:8@16@24q32")){reply[@"error"]=@"unsupported_interface";return reply;}
  id headers=[cls _headersForRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:endpoint]] account:nil type:2];
  NSMutableDictionary *selected=[NSMutableDictionary dictionary];
  for(NSString *field in @[@"X-Apple-AMD",@"X-Apple-AMD-M"]){
   id value=[headers isKindOfClass:NSDictionary.class]?headers[field]:nil;
   if(![value isKindOfClass:NSString.class] || ![value length] || [value length]>16384 || [value rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location!=NSNotFound){reply[@"error"]=@"native_failure";return reply;}
   selected[field]=value;
  }
  reply[@"headers"]=selected;return reply;
 }
 if([op isEqual:@"build-store-request"]){
  NSSet *keys=[NSSet setWithArray:[request allKeys]];
  NSString *endpoint=request[@"endpoint"];
  NSArray *allowed=@[@"https://auth.itunes.apple.com/auth/v1/native",@"https://auth.itunes.apple.com/auth/v1/native/",@"https://auth.itunes.apple.com/auth/v1/native/fast",@"https://auth.itunes.apple.com/auth/v1/native/fast/"];
  if(![keys isEqual:[NSSet setWithArray:@[@"version",@"nonce",@"operation",@"endpoint",@"parameters"]]] || ![request[@"version"] isEqual:@1] || ![allowed containsObject:endpoint]){reply[@"error"]=@"invalid_request";return reply;}
  id pb64=request[@"parameters"];
  if(![pb64 isKindOfClass:NSString.class] || [pb64 length]>200000){reply[@"error"]=@"invalid_request";return reply;}
  NSData *pdata=[[NSData alloc] initWithBase64EncodedString:pb64 options:0];
  if(!pdata.length || pdata.length>131072){reply[@"error"]=@"invalid_request";return reply;}
  id parameters=[NSPropertyListSerialization propertyListWithData:pdata options:0 format:NULL error:NULL];
  if(![parameters isKindOfClass:NSDictionary.class]){reply[@"error"]=@"invalid_request";return reply;}
  Class ecls=NSClassFromString(@"AMSURLRequestEncoder");
  if(!ecls){reply[@"error"]=@"unsupported_interface";return reply;}
  id bag=[ecls respondsToSelector:@selector(createBagForSubProfile)]?[ecls createBagForSubProfile]:nil;
  id encoder=[[ecls alloc] initWithBag:bag];
  Class pcls=NSClassFromString(@"AMSProcessInfo");
  id process=pcls?[[pcls alloc] initWithBundleIdentifier:@"com.apple.AppStore"]:nil;
  if(process && [encoder respondsToSelector:@selector(setClientInfo:)])[encoder setClientInfo:process];
  void(*setN)(id,SEL,NSInteger)=(void*)objc_msgSend;
  if([encoder respondsToSelector:sel_registerName("setMescalType:")])setN(encoder,sel_registerName("setMescalType:"),1);
  if([encoder respondsToSelector:sel_registerName("setRequestEncoding:")])setN(encoder,sel_registerName("setRequestEncoding:"),2);
  if([encoder respondsToSelector:sel_registerName("setShouldSetCookiesFromResponse:")])setN(encoder,sel_registerName("setShouldSetCookiesFromResponse:"),0);
  if([encoder respondsToSelector:sel_registerName("setShouldSetStorefrontFromResponse:")])setN(encoder,sel_registerName("setShouldSetStorefrontFromResponse:"),0);
  if([encoder respondsToSelector:sel_registerName("setAlwaysIncludeAuthKitHeaders:")])setN(encoder,sel_registerName("setAlwaysIncludeAuthKitHeaders:"),1);
  NSError *error=nil;
  id built=[encoder requestWithMethod:4 URL:[NSURL URLWithString:endpoint] parameters:parameters error:&error];
  if(![built isKindOfClass:NSURLRequest.class]){reply[@"error"]=@"native_failure";if(error)reply[@"native_code"]=@(error.code);return reply;}
  NSDictionary *built_headers=[built allHTTPHeaderFields]?:@{};
  // Require the self-consistent proof set the gate needs.
  if(!built_headers[@"X-Apple-AMD"] || !built_headers[@"X-Apple-ActionSignature"]){reply[@"error"]=@"native_failure";return reply;}
  reply[@"url"]=[[built URL] absoluteString];
  reply[@"request_headers"]=built_headers;
  reply[@"body"]=[[built HTTPBody] base64EncodedStringWithOptions:0]?:@"";
  return reply;
 }

 NSSet *keys=[NSSet setWithArray:[request allKeys]];
 NSSet *expected=[NSSet setWithArray:[op isEqual:@"verify"]?@[@"version",@"nonce",@"operation",@"data",@"signature"]:@[@"version",@"nonce",@"operation",@"data"]];
 NSData *data=decode(request[@"data"]),*signature=[op isEqual:@"verify"]?decode(request[@"signature"]):nil;
 if(![keys isEqual:expected] || ![request[@"version"] isEqual:@1] || !data || ([op isEqual:@"verify"] && !signature) || ![@[@"sign",@"prime",@"verify",@"verify-prime"] containsObject:op]){reply[@"error"]=@"invalid_request";return reply;}
 id session=([op isEqual:@"prime"] || [op isEqual:@"verify-prime"])?[NSClassFromString(@"AMSMescalSession") primeSession]:[NSClassFromString(@"AMSMescalSession") defaultSession];
 id bag=[NSClassFromString(@"IMURLBag") new],promise=nil;
 if([op isEqual:@"sign"])promise=[session signData:data bag:bag];
 else if([op isEqual:@"prime"])promise=[session primeSignatureForData:data bag:bag];
 else if([op isEqual:@"verify"])promise=[session verifyData:data withSignature:signature bag:bag];
 else promise=[session verifyPrimeSignature:data];
 if(![promise respondsToSelector:@selector(isFinished)] || ![promise respondsToSelector:@selector(resultWithError:)]){reply[@"error"]=@"unsupported_interface";return reply;}
 NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:25];
 while(![promise isFinished] && deadline.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
 if(![promise isFinished]){reply[@"error"]=@"timeout";return reply;}
 NSError *error=nil;id result=[promise resultWithError:&error];
 if(error){reply[@"error"]=@"native_failure";reply[@"native_code"]=@(error.code);reply[@"native_domain"]=[error.domain isEqual:@"AMSErrorDomain"]?@"AMSErrorDomain":@"other";return reply;}
 if([op hasPrefix:@"verify"]){
  if(![result isKindOfClass:NSNumber.class]){reply[@"error"]=@"invalid_native_result";return reply;}
  reply[@"verified"]=@([result boolValue]);
 }else{
  if(![result isKindOfClass:NSData.class] || ![result length] || [result length]>131072){reply[@"error"]=@"invalid_native_result";return reply;}
  reply[@"signature"]=[result base64EncodedStringWithOptions:0];
 }
 return reply;
}
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2 || strcmp(argv[1],"--stdio"))return 2;
 dlopen("/System/Library/PrivateFrameworks/PodcastsFoundation.framework/PodcastsFoundation",RTLD_NOW);
 dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 char *line=NULL;size_t capacity=0;ssize_t length;
 while((length=getline(&line,&capacity,stdin))>0){@autoreleasepool{
  if(length>370000){emit(@{@"error":@"invalid_request"});memset(line,0,capacity);continue;}
  NSData *bytes=[NSData dataWithBytes:line length:(NSUInteger)length];
  id request=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL];memset(line,0,capacity);
  @try{emit(perform(request));}@catch(NSException *exception){(void)exception;emit(@{@"error":@"native_exception"});}
 }}
 if(line){memset(line,0,capacity);free(line);}return 0;
}}

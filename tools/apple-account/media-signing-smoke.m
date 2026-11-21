/* Synthetic bytes only. Exercise real helper signing and rejection, no login. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
@interface NSObject (RWMediaSmoke)
+ (id)defaultSession;
- (id)signData:(NSData *)data bag:(id)bag error:(NSError **)error;
- (id)primeSignatureForData:(NSData *)data bag:(id)bag error:(NSError **)error;
- (BOOL)verifyPrimeSignature:(NSData *)signature error:(NSError **)error;
- (BOOL)verifyData:(NSData *)data withSignature:(NSData *)signature bag:(id)bag error:(NSError **)error;
@end
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=2)return 2;
 dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 void *library=dlopen(argv[1],RTLD_NOW);
 BOOL (*install)(void)=library?dlsym(library,"rw_media_signing_install"):NULL;
 if(!install || !install()){puts("FAIL: ABI/install");return 1;}
 id session=[NSClassFromString(@"AMSMescalSession") defaultSession];
 NSError *error=nil;NSData *fixture=[@"redwood/media-native-adapter/synthetic/v1" dataUsingEncoding:NSUTF8StringEncoding];
 id signature=[session signData:fixture bag:nil error:&error];
 if(error || ![signature isKindOfClass:NSData.class] || ![signature length]){puts("FAIL: synthetic signing");return 1;}
 NSData *invalid=[@"invalid-server-signature" dataUsingEncoding:NSUTF8StringEncoding];
 BOOL verified=[session verifyData:fixture withSignature:invalid bag:nil error:&error];
 if(verified || !error){puts("FAIL: invalid server signature accepted");return 1;}
 error=nil;
 id prime=[session primeSignatureForData:fixture bag:nil error:&error];
 if(error || ![prime isKindOfClass:NSData.class] || ![prime length]){puts("FAIL: synthetic prime signing");return 1;}
 BOOL primeVerified=[session verifyPrimeSignature:invalid error:&error];
 if(primeVerified || !error){puts("FAIL: invalid prime signature accepted");return 1;}
 error=nil;
 id empty=[session signData:NSData.data bag:nil error:&error];
 if(empty || !error){puts("FAIL: invalid input accepted");return 1;}
 puts("PASS: real private native signature, invalid server signature rejected, invalid input fails closed; no account login");return 0;
}}

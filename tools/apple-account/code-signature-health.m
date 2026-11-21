/* Read-only csops status/identity/one entitlement. Never emits entitlement blob. */
#import <Foundation/Foundation.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
extern int csops(pid_t,unsigned int,void *,size_t);
int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2)return 2;pid_t pid=(pid_t)strtol(argv[1],NULL,10);if(pid<=0)return 2;
    unsigned flags=0;int result=csops(pid,0,&flags,sizeof(flags));int statusError=result?errno:0;
    unsigned char identity[1024]={0};result=csops(pid,11,identity,sizeof(identity));int identityError=result?errno:0;
    BOOL matches=!result && !strcmp((const char *)identity+8,"com.apple.Preferences");
    NSString *signingID=!result?[[NSString alloc] initWithBytes:identity+8 length:strnlen((const char *)identity+8,128) encoding:NSUTF8StringEncoding]:nil;
    NSCharacterSet *alphabet=[NSCharacterSet characterSetWithCharactersInString:@"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-"];
    if(!signingID.length || [signingID rangeOfCharacterFromSet:alphabet.invertedSet].location!=NSNotFound)signingID=@"unparsed";
    unsigned char entitlements[65536]={0};result=csops(pid,7,entitlements,sizeof(entitlements));int entitlementError=result?errno:0;
    id value=nil;
    if(!result) {
        uint32_t size=0;memcpy(&size,entitlements+4,4);size=__builtin_bswap32(size);
        if(size>8 && size<=sizeof(entitlements))value=[NSPropertyListSerialization propertyListWithData:[NSData dataWithBytes:entitlements+8 length:size-8] options:0 format:NULL error:NULL];
    }
    NSDictionary *out=@{@"pid":@(pid),@"status_errno":@(statusError),@"flags":@(flags),@"native_apple_binary_bits_match":@((flags&0x0c000001)==0x04000001),@"identity_errno":@(identityError),@"signing_identity":signingID,@"identity_matches_preferences":@(matches),@"entitlement_errno":@(entitlementError),@"internal_authkit_entitlement_present":@([value isKindOfClass:NSDictionary.class] && [value[@"com.apple.authkit.client.internal"] boolValue])};
    NSData *json=[NSJSONSerialization dataWithJSONObject:out options:0 error:NULL];fwrite(json.bytes,1,json.length,stdout);putchar('\n');return 0;
}}

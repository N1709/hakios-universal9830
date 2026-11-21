/* Mac-side helper: given {endpoint, parameters{...}}, run the native AMSURLRequestEncoder
 * to build + sign a complete Store request (self-consistent AMD + ActionSignature + body)
 * from those exact parameters, and emit it as a request_dump for verbatim replay.
 * No account is read; the caller supplies all parameters. No success stub. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>

static id get(id o,const char *n){SEL s=sel_registerName(n);return [o respondsToSelector:s]?((id(*)(id,SEL))objc_msgSend)(o,s):nil;}
static void set(id o,const char *n,NSInteger v){SEL s=sel_registerName(n);if([o respondsToSelector:s])((void(*)(id,SEL,NSInteger))objc_msgSend)(o,s,v);}

int main(void){@autoreleasepool{
    dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
    NSData *in=[[NSFileHandle fileHandleWithStandardInput] readDataToEndOfFile];
    id req=[NSJSONSerialization JSONObjectWithData:in options:0 error:NULL];
    if(![req isKindOfClass:NSDictionary.class]){fprintf(stderr,"bad input\n");return 2;}
    NSString *endpoint=req[@"endpoint"]?:@"https://auth.itunes.apple.com/auth/v1/native";
    id parameters=req[@"parameters"];
    if(![parameters isKindOfClass:NSDictionary.class]){fprintf(stderr,"no params\n");return 2;}

    Class cls=NSClassFromString(@"AMSURLRequestEncoder");
    id bag=get(cls,"createBagForSubProfile");
    id encoder=((id(*)(id,SEL,id))objc_msgSend)([cls alloc],sel_registerName("initWithBag:"),bag);
    Class processClass=NSClassFromString(@"AMSProcessInfo");
    id process=((id(*)(id,SEL,id))objc_msgSend)([processClass alloc],sel_registerName("initWithBundleIdentifier:"),@"com.apple.AppStore");
    if([encoder respondsToSelector:sel_registerName("setClientInfo:")])
        ((void(*)(id,SEL,id))objc_msgSend)(encoder,sel_registerName("setClientInfo:"),process);
    set(encoder,"setMescalType:",1);set(encoder,"setRequestEncoding:",2);
    set(encoder,"setShouldSetCookiesFromResponse:",0);set(encoder,"setShouldSetStorefrontFromResponse:",0);
    set(encoder,"setAlwaysIncludeAuthKitHeaders:",1);
    NSError *error=nil;
    SEL selector=sel_registerName("requestWithMethod:URL:parameters:error:");
    id request=((id(*)(id,SEL,NSInteger,id,id,NSError **))objc_msgSend)(encoder,selector,4,[NSURL URLWithString:endpoint],parameters,&error);
    if(![request isKindOfClass:NSURLRequest.class]){fprintf(stderr,"build failed %ld\n",(long)error.code);return 4;}

    NSMutableDictionary *out=[NSMutableDictionary dictionary];
    out[@"url"]=[[request URL] absoluteString];
    out[@"method"]=[request HTTPMethod]?:@"POST";
    out[@"headers"]=[request allHTTPHeaderFields]?:@{};
    out[@"body_b64"]=[[request HTTPBody] base64EncodedStringWithOptions:0]?:@"";
    NSData *j=[NSJSONSerialization dataWithJSONObject:@{@"request_dump":out} options:NSJSONWritingPrettyPrinted error:NULL];
    fwrite(j.bytes,1,j.length,stdout);putchar('\n');
    return 0;
}}

/* Mac-side helper: given a fully-built Store request (url, headers, body), run the
 * native AMSURLSession._prepareRequest: so Apple's own AMS attaches a self-consistent
 * proof set (X-Apple-AMD / X-Apple-AMD-M / X-Apple-ActionSignature) bound to THAT exact
 * body, and emit the prepared request. No account read, no body rebuild, no success stub.
 * stdin: JSON {url, method, headers{}, body_b64}  ->  stdout: same shape, proofs added. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>

int main(void){@autoreleasepool{
    dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
    NSData *in=[[NSFileHandle fileHandleWithStandardInput] readDataToEndOfFile];
    id req=[NSJSONSerialization JSONObjectWithData:in options:0 error:NULL];
    if(![req isKindOfClass:NSDictionary.class]){fprintf(stderr,"bad input\n");return 2;}
    NSMutableURLRequest *r=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:req[@"url"]]];
    r.HTTPMethod=req[@"method"]?:@"POST";
    /* Set the incoming headers, but never let caller-supplied proof headers survive -
     * the native prepare must compute them fresh over the body. */
    NSSet *proof=[NSSet setWithArray:@[@"x-apple-amd",@"x-apple-amd-m",@"x-apple-actionsignature"]];
    id hdrs=req[@"headers"];
    if([hdrs isKindOfClass:NSDictionary.class])
        for(NSString *f in hdrs)
            if(![proof containsObject:[f lowercaseString]])
                [r setValue:hdrs[f] forHTTPHeaderField:f];
    NSString *b64=req[@"body_b64"];
    if([b64 isKindOfClass:NSString.class]&&b64.length)r.HTTPBody=[[NSData alloc] initWithBase64EncodedString:b64 options:0];

    Class S=NSClassFromString(@"AMSURLSession");
    /* macOS AMS: -[AMSURLSession _prepareRequest:logUUID:] (id, request, logUUID). */
    SEL sel=sel_registerName("_prepareRequest:logUUID:");
    Method m=class_getInstanceMethod(S,sel);
    if(!m){fprintf(stderr,"iface\n");return 3;}
    id session=((id(*)(id,SEL,id))objc_msgSend)([S alloc],sel_registerName("initWithConfiguration:"),[NSURLSessionConfiguration ephemeralSessionConfiguration]);
    id prepared=((id(*)(id,SEL,id,id))objc_msgSend)(session,sel,r,[NSUUID UUID]);
    if(![prepared isKindOfClass:NSURLRequest.class]){fprintf(stderr,"prepare failed\n");return 4;}

    NSMutableDictionary *out=[NSMutableDictionary dictionary];
    out[@"url"]=[[prepared URL] absoluteString];
    out[@"method"]=[prepared HTTPMethod]?:@"POST";
    out[@"headers"]=[prepared allHTTPHeaderFields]?:@{};
    out[@"body_b64"]=[[prepared HTTPBody] base64EncodedStringWithOptions:0]?:@"";
    /* Wrap as a request_dump so the phone replay path consumes it directly. */
    NSData *j=[NSJSONSerialization dataWithJSONObject:@{@"request_dump":out} options:NSJSONWritingPrettyPrinted error:NULL];
    fwrite(j.bytes,1,j.length,stdout);putchar('\n');
    return 0;
}}

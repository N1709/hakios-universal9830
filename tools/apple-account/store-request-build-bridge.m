/* Build the Store /auth/v1/native request on the owner's Mac so the Mescal proof set
 * (X-Apple-AMD / X-Apple-AMD-M / X-Apple-ActionSignature) is self-consistent with the body.
 *
 * Hooks -[AMSURLRequestEncoder requestWithMethod:URL:parameters:error:]. For the native
 * Store auth endpoint it lets the native encoder build the phone's request (to obtain the
 * exact request parameters as the encoded body), forwards those parameters to the broker's
 * build-store-request op, and returns a request made from the Mac-built url/headers/body.
 * Any failure falls back to the native request unchanged. All other requests are untouched.
 * No credential is logged; the body crosses to the owner Mac only to be signed, as today. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include "shared-layer-scope.h"

static id(*nativePrepare)(id,SEL,id,id,NSError **);
static int output=-1;

static BOOL store_endpoint(NSURL *url){
    return [url.scheme.lowercaseString isEqual:@"https"] && [url.host.lowercaseString isEqual:@"auth.itunes.apple.com"] &&
    [@[@"/auth/v1/native",@"/auth/v1/native/",@"/auth/v1/native/fast",@"/auth/v1/native/fast/"] containsObject:url.path];
}

/* POST build-store-request to the local broker; return {url,request_headers,body} or nil. */
static NSDictionary *broker_build(NSString *endpoint,NSDictionary *parameters){
    NSData *bytes=[NSData dataWithContentsOfFile:@"/var/mobile/Library/RedwoodAppleAccountLab/connection.json"];
    id config=bytes?[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL]:nil;
    if(![config isKindOfClass:NSDictionary.class] || ![config[@"enabled"] boolValue] || ![config[@"port"] isKindOfClass:NSNumber.class] || ![config[@"token"] isKindOfClass:NSString.class])return nil;
    NSInteger port=[config[@"port"] integerValue];if(port<1024||port>65535)return nil;
    NSData *pdata=[NSPropertyListSerialization dataWithPropertyList:parameters format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    if(!pdata.length || pdata.length>131072)return nil;
    NSString *nonce=[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""];
    NSDictionary *value=@{@"version":@1,@"nonce":nonce,@"operation":@"build-store-request",
                          @"endpoint":[@"https://auth.itunes.apple.com" stringByAppendingString:[[NSURL URLWithString:endpoint] path]],
                          @"parameters":[pdata base64EncodedStringWithOptions:0]};
    NSMutableURLRequest *req=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:%ld/v1/media",(long)port]]];
    req.HTTPMethod=@"POST";req.HTTPBody=[NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];req.timeoutInterval=8;
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    [req setValue:[@"Bearer " stringByAppendingString:config[@"token"]] forHTTPHeaderField:@"Authorization"];
    NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    dispatch_semaphore_t done=dispatch_semaphore_create(0);__block NSData *data=nil;__block NSInteger status=0;
    [[session dataTaskWithRequest:req completionHandler:^(NSData *b,NSURLResponse *r,NSError *e){
        if(!e && [r isKindOfClass:NSHTTPURLResponse.class] && b.length<=300000){status=[(NSHTTPURLResponse *)r statusCode];data=b;}dispatch_semaphore_signal(done);
    }] resume];
    BOOL timeout=dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,9*NSEC_PER_SEC))!=0;[session invalidateAndCancel];
    id reply=!timeout && status==200 && data?[NSJSONSerialization JSONObjectWithData:data options:0 error:NULL]:nil;
    if(![reply isKindOfClass:NSDictionary.class] || reply[@"error"] || ![reply[@"nonce"] isEqual:nonce] || ![reply[@"version"] isEqual:@1])return nil;
    NSString *url=reply[@"url"];NSDictionary *headers=reply[@"request_headers"];NSString *body=reply[@"body"];
    if(![url isKindOfClass:NSString.class] || !store_endpoint([NSURL URLWithString:url]) || ![headers isKindOfClass:NSDictionary.class] || !headers[@"X-Apple-AMD"] || !headers[@"X-Apple-ActionSignature"] || ![body isKindOfClass:NSString.class])return nil;
    for(id f in headers){id v=headers[f];if(![f isKindOfClass:NSString.class] || ![v isKindOfClass:NSString.class] || ![v length] || [v length]>16384 || [v rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location!=NSNotFound)return nil;}
    return reply;
}

static id prepare(id self,SEL cmd,id request,id properties,NSError **error){
    id native=nativePrepare(self,cmd,request,properties,error);
    @try{
        if(![native isKindOfClass:NSURLRequest.class] || !store_endpoint([native URL]) || (error && *error))return native;
        /* Use the native encoder's exact serialized body as the parameters to rebuild on the Mac. */
        id params=[NSPropertyListSerialization propertyListWithData:[native HTTPBody] options:0 format:NULL error:NULL];
        if(![params isKindOfClass:NSDictionary.class])return native;
        NSDictionary *built=broker_build([[native URL] absoluteString],params);
        if(!built){
            if(output>=0)dprintf(output,"{\"event\":\"store_build_unavailable\",\"pid\":%d}\n",getpid());
            return native;
        }
        NSMutableURLRequest *out=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:built[@"url"]]];
        out.HTTPMethod=[native HTTPMethod]?:@"POST";
        for(NSString *f in built[@"request_headers"])[out setValue:built[@"request_headers"][f] forHTTPHeaderField:f];
        NSString *b64=built[@"body"];
        if([b64 isKindOfClass:NSString.class] && b64.length)out.HTTPBody=[[NSData alloc] initWithBase64EncodedString:b64 options:0];
        if(error)*error=nil;
        if(output>=0)dprintf(output,"{\"event\":\"store_request_built_on_mac\",\"pid\":%d,\"amd\":true,\"sig\":true}\n",getpid());
        return out;
    }@catch(NSException *ex){(void)ex;return native;}
}

__attribute__((visibility("default"))) BOOL rw_store_build_install(void){
    static BOOL installed=NO;if(installed)return YES;
    Method m=class_getInstanceMethod(NSClassFromString(@"AMSURLSession"),sel_registerName("_prepareRequest:properties:error:"));
    if(!m || strcmp(method_getTypeEncoding(m),"@40@0:8@16@24^@32"))return NO;
    nativePrepare=(void *)method_getImplementation(m);
    method_setImplementation(m,(IMP)prepare);
    installed=YES;return YES;
}

__attribute__((constructor)) static void start(void){
    if(!rw_shared_media_process())return;
    dispatch_async(dispatch_get_main_queue(),^{
        dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
        output=open("/var/mobile/Library/RedwoodAppleAccountLab/media-error-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
        BOOL installed=rw_store_build_install();
        if(output>=0)dprintf(output,"{\"event\":\"store_build_bridge_installed\",\"installed\":%s,\"pid\":%d}\n",installed?"true":"false",getpid());
    });
}

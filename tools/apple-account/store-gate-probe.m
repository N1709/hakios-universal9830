/* Store pre-auth gate probe for the S20 port.
 *
 * Builds the phone's native, signed /auth/v1/native request using a NON-EXISTENT
 * fixture account (fixture@example.invalid) -- never the owner's Apple ID -- then
 * sends it and several copies with device-identity headers removed. Apple's Store
 * returns an empty 200 when its pre-account gate rejects the request, and a small
 * plist (e.g. failureType/customerMessage) once the gate is passed. Comparing the
 * variants locates which header the gate objects to. No real credential is used,
 * no account state is read or changed, no response bytes are written to disk.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>

@interface RWStoreFixture:NSObject
@property NSUInteger credentialSource;
@end
@implementation RWStoreFixture
- (id)options{return self;}
- (id)credential{return self;}
- (id)password{return @"SYNTHETIC-NOT-A-PASSWORD";}
- (id)username{return @"fixture@example.invalid";}
- (NSInteger)ams_freePasswordPromptSetting{return 0;}
- (NSInteger)ams_paidPasswordPromptSetting{return 0;}
- (void)setDeviceOffersCheckEncodingForRequestParameters:(id)value{(void)value;}
@end

static id get(id object,const char *name){SEL s=sel_registerName(name);return [object respondsToSelector:s]?((id(*)(id,SEL))objc_msgSend)(object,s):nil;}
static void set(id object,const char *name,NSInteger value){SEL s=sel_registerName(name);if([object respondsToSelector:s])((void(*)(id,SEL,NSInteger))objc_msgSend)(object,s,value);}

/* Fetch the borrowed X-Apple-AMD/AMD-M from the broker, exactly as store-amd-bridge does. */
static NSDictionary *rw_amd(NSString *path){
    NSData *bytes=[NSData dataWithContentsOfFile:@"/var/mobile/Library/RedwoodAppleAccountLab/connection.json"];
    id config=bytes?[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL]:nil;
    if(![config isKindOfClass:NSDictionary.class]||![config[@"port"] isKindOfClass:NSNumber.class]||![config[@"token"] isKindOfClass:NSString.class])return nil;
    NSString *nonce=[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""];
    NSString *endpoint=[@"https://auth.itunes.apple.com" stringByAppendingString:path];
    NSDictionary *value=@{@"version":@1,@"nonce":nonce,@"operation":@"store-proof",@"endpoint":endpoint};
    NSMutableURLRequest *req=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:%ld/v1/media",(long)[config[@"port"] integerValue]]]];
    req.HTTPMethod=@"POST";req.HTTPBody=[NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];req.timeoutInterval=35;
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    [req setValue:[@"Bearer " stringByAppendingString:config[@"token"]] forHTTPHeaderField:@"Authorization"];
    NSURLSession *s=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    dispatch_semaphore_t done=dispatch_semaphore_create(0);__block NSData *data=nil;
    [[s dataTaskWithRequest:req completionHandler:^(NSData *b,NSURLResponse *r,NSError *e){if(!e&&b.length<=65536)data=b;dispatch_semaphore_signal(done);}] resume];
    dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,36*NSEC_PER_SEC));
    id reply=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:NULL]:nil;
    if(![reply isKindOfClass:NSDictionary.class]||reply[@"error"])return nil;
    id headers=reply[@"headers"];
    return [headers isKindOfClass:NSDictionary.class]?headers:nil;
}

/* Call the broker's build-store-request op with the given parameters plist; return the
 * Mac-built request {url, request_headers, body} or nil. Mirrors the production hook. */
static NSDictionary *rw_broker_build(NSString *endpoint,NSDictionary *parameters){
    NSData *bytes=[NSData dataWithContentsOfFile:@"/var/mobile/Library/RedwoodAppleAccountLab/connection.json"];
    id config=bytes?[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL]:nil;
    if(![config isKindOfClass:NSDictionary.class]||![config[@"port"] isKindOfClass:NSNumber.class]||![config[@"token"] isKindOfClass:NSString.class])return nil;
    NSData *pdata=[NSPropertyListSerialization dataWithPropertyList:parameters format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    if(!pdata.length)return nil;
    NSString *nonce=[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""];
    NSDictionary *value=@{@"version":@1,@"nonce":nonce,@"operation":@"build-store-request",@"endpoint":endpoint,@"parameters":[pdata base64EncodedStringWithOptions:0]};
    NSMutableURLRequest *req=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithFormat:@"http://127.0.0.1:%ld/v1/media",(long)[config[@"port"] integerValue]]]];
    req.HTTPMethod=@"POST";req.HTTPBody=[NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];req.timeoutInterval=35;
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    [req setValue:[@"Bearer " stringByAppendingString:config[@"token"]] forHTTPHeaderField:@"Authorization"];
    NSURLSession *s=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    dispatch_semaphore_t done=dispatch_semaphore_create(0);__block NSData *data=nil;
    [[s dataTaskWithRequest:req completionHandler:^(NSData *b,NSURLResponse *r,NSError *e){if(!e&&b.length<=300000)data=b;dispatch_semaphore_signal(done);}] resume];
    dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,36*NSEC_PER_SEC));
    id reply=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:NULL]:nil;
    if(![reply isKindOfClass:NSDictionary.class]||reply[@"error"]||![reply[@"url"] isKindOfClass:NSString.class])return nil;
    return reply;
}

static NSDictionary *rw_send(NSURLRequest *req,NSArray *drop){
    NSMutableURLRequest *m=[req mutableCopy];
    for(NSString *h in drop)[m setValue:nil forHTTPHeaderField:h];
    NSURLSession *s=[NSURLSession sessionWithConfiguration:[NSURLSessionConfiguration ephemeralSessionConfiguration]];
    __block NSMutableDictionary *r=[NSMutableDictionary dictionary];
    dispatch_semaphore_t sem=dispatch_semaphore_create(0);
    [[s dataTaskWithRequest:m completionHandler:^(NSData *d,NSURLResponse *resp,NSError *e){
        NSHTTPURLResponse *h=(NSHTTPURLResponse *)resp;
        r[@"status"]=@(h.statusCode);
        r[@"bytes"]=@(d.length);
        r[@"content_type"]=[h valueForHTTPHeaderField:@"Content-Type"]?:@"";
        if(e)r[@"error"]=[NSString stringWithFormat:@"%@ %ld",e.domain,(long)e.code];
        id pl=d.length?[NSPropertyListSerialization propertyListWithData:d options:0 format:NULL error:NULL]:nil;
        if([pl isKindOfClass:NSDictionary.class]){
            r[@"plist_keys"]=[[pl allKeys] sortedArrayUsingSelector:@selector(compare:)];
            for(NSString *k in @[@"failureType",@"customerMessage",@"m-allowed"]) if(pl[k]) r[k]=[pl[k] description];
            id dlg=pl[@"dialog"]; if([dlg isKindOfClass:NSDictionary.class]&&dlg[@"explanation"]) r[@"dialog"]=[dlg[@"explanation"] description];
        }
        dispatch_semaphore_signal(sem);
    }] resume];
    dispatch_semaphore_wait(sem,dispatch_time(DISPATCH_TIME_NOW,45*NSEC_PER_SEC));
    return r;
}

int main(int argc,const char **argv){@autoreleasepool{
    dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
    if(!dlopen("/System/Library/Accounts/Authentication/AMSAccountAuthenticationPlugin.bundle/AMSAccountAuthenticationPlugin",RTLD_NOW))
        dlopen("/System/Library/Accounts/Authentication/AMSAccountAuthenticationPlugin.bundle/Contents/MacOS/AMSAccountAuthenticationPlugin",RTLD_NOW);
    /* argv[1] = account-layer dylib (installs borrowed anisette + signing). */
    if(argc>=2){
        void *lib=dlopen(argv[1],RTLD_NOW);
        int(*install)(void)=lib?dlsym(lib,"rw_auth_lab_install"):NULL;
        BOOL(*signing)(void)=lib?dlsym(lib,"rw_media_signing_install"):NULL;
        if(!install||!install()||!signing||!signing()){fprintf(stderr,"layer install failed\n");return 2;}
    }
    /* argv[2..] = extra dylibs each exporting rw_store_amd_install / rw_store_guid_install. */
    for(int i=2;i<argc;i++){
        void *extra=dlopen(argv[i],RTLD_NOW);
        BOOL(*amd)(void)=extra?dlsym(extra,"rw_store_amd_install"):NULL;
        BOOL(*guid)(void)=extra?dlsym(extra,"rw_store_guid_install"):NULL;
        BOOL(*bld)(void)=extra?dlsym(extra,"rw_store_build_install"):NULL;
        if(amd&&!amd()){fprintf(stderr,"amd install failed\n");return 2;}
        if(guid&&!guid()){fprintf(stderr,"guid install failed\n");return 2;}
        if(bld&&!bld()){fprintf(stderr,"build install failed\n");return 2;}
    }

    NSMutableDictionary *report=[@{@"fixture_account":@"fixture@example.invalid",@"real_account_used":@NO} mutableCopy];
    /* RW_REPLAY handled first: send a request another host prepared, no local building. */
    const char *rp0=getenv("RW_REPLAY");
    if(rp0){
        NSData *rb=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:rp0]];
        id rd=rb?[NSJSONSerialization JSONObjectWithData:rb options:0 error:NULL]:nil;
        id dump=[rd isKindOfClass:NSDictionary.class]?rd[@"request_dump"]:nil;
        if([dump isKindOfClass:NSDictionary.class]){
            NSMutableURLRequest *rr=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:dump[@"url"]]];
            rr.HTTPMethod=dump[@"method"]?:@"POST";
            for(NSString *f in dump[@"headers"])[rr setValue:dump[@"headers"][f] forHTTPHeaderField:f];
            NSString *b64=dump[@"body_b64"];
            if([b64 isKindOfClass:NSString.class]&&b64.length)rr.HTTPBody=[[NSData alloc] initWithBase64EncodedString:b64 options:0];
            report[@"sent"]=@{@"replay":rw_send(rr,@[])};
        }else report[@"error"]=@"bad_replay";
        NSData *j0=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
        fwrite(j0.bytes,1,j0.length,stdout);putchar('\n');return 0;
    }
    @try{
        Method mm=class_getInstanceMethod(NSClassFromString(@"AMSVerifyCredentialsTask"),sel_registerName("_createAuthenticateRequestParametersForAccount:"));
        if(!mm||strcmp(method_getTypeEncoding(mm),"@24@0:8@16")){report[@"error"]=@"params_iface";goto done;}
        RWStoreFixture *fixture=[RWStoreFixture new];
        NSDictionary *parameters=((id(*)(id,SEL,id))method_getImplementation(mm))(fixture,method_getName(mm),fixture);
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
        NSMethodSignature *sig=[encoder methodSignatureForSelector:selector];
        if(!sig||sig.numberOfArguments!=6){report[@"error"]=@"encoder_iface";goto done;}
        id request=((id(*)(id,SEL,NSInteger,id,id,NSError **))objc_msgSend)(encoder,selector,4,[NSURL URLWithString:@"https://auth.itunes.apple.com/auth/v1/native"],parameters,&error);
        if(![request isKindOfClass:NSURLRequest.class]){report[@"error"]=@"no_request";if(error)report[@"error_code"]=@(error.code);goto done;}

        /* RW_PREPHOOK=1: run the fixture request through a real AMSURLSession _prepareRequest,
         * which fires the build-bridge hook (the production send-path interception), then send. */
        if(getenv("RW_PREPHOOK")){
            Class S=NSClassFromString(@"AMSURLSession");
            id sess=((id(*)(id,SEL,id))objc_msgSend)([S alloc],sel_registerName("initWithConfiguration:"),[NSURLSessionConfiguration ephemeralSessionConfiguration]);
            SEL ps=sel_registerName("_prepareRequest:properties:error:");
            NSError *pe=nil;
            id prepared=((id(*)(id,SEL,id,id,NSError **))objc_msgSend)(sess,ps,request,nil,&pe);
            report[@"prepared_is_request"]=@([prepared isKindOfClass:NSURLRequest.class]);
            if([prepared isKindOfClass:NSURLRequest.class]){
                report[@"prepared_headers"]=[[[prepared allHTTPHeaderFields] allKeys] sortedArrayUsingSelector:@selector(compare:)];
                report[@"prepared_has_amd"]=@([prepared valueForHTTPHeaderField:@"X-Apple-AMD"]!=nil);
                report[@"sent"]=@{@"prephook":rw_send(prepared,@[])};
            }else if(pe)report[@"prepare_error"]=@(pe.code);
            NSData *jp=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
            fwrite(jp.bytes,1,jp.length,stdout);putchar('\n');return 0;
        }

        /* RW_HOOKTEST=1: the build-bridge hook already replaced `request` with the Mac-built
         * one at encode time. Just send it, as accountsd would. */
        if(getenv("RW_HOOKTEST")){
            report[@"built_headers"]=[[[request allHTTPHeaderFields] allKeys] sortedArrayUsingSelector:@selector(compare:)];
            report[@"has_amd"]=@([request valueForHTTPHeaderField:@"X-Apple-AMD"]!=nil);
            report[@"sent"]=@{@"hook":rw_send(request,@[])};
            NSData *jh=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
            fwrite(jh.bytes,1,jh.length,stdout);putchar('\n');return 0;
        }

        /* RW_BROKER=1: production flow - send this request's parameters to the broker's
         * build-store-request op, then send the Mac-built request. */
        if(getenv("RW_BROKER")){
            id body=[NSPropertyListSerialization propertyListWithData:[request HTTPBody] options:0 format:NULL error:NULL];
            report[@"params_keys"]=[body isKindOfClass:NSDictionary.class]?[[body allKeys] sortedArrayUsingSelector:@selector(compare:)]:@[];
            NSDictionary *built=[body isKindOfClass:NSDictionary.class]?rw_broker_build(@"https://auth.itunes.apple.com/auth/v1/native",body):nil;
            report[@"broker_built"]=@(built!=nil);
            if(built){
                NSMutableURLRequest *mr=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:built[@"url"]]];
                mr.HTTPMethod=@"POST";
                for(NSString *f in built[@"request_headers"])[mr setValue:built[@"request_headers"][f] forHTTPHeaderField:f];
                NSString *bb=built[@"body"];
                if([bb isKindOfClass:NSString.class]&&bb.length)mr.HTTPBody=[[NSData alloc] initWithBase64EncodedString:bb options:0];
                report[@"built_headers"]=[[[mr allHTTPHeaderFields] allKeys] sortedArrayUsingSelector:@selector(compare:)];
                report[@"sent"]=@{@"broker_built":rw_send(mr,@[])};
            }
            NSData *jb=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
            fwrite(jb.bytes,1,jb.length,stdout);putchar('\n');return 0;
        }
        /* Attach the borrowed AMD proofs so the request matches the real send path. */
        NSDictionary *amd=rw_amd([[request URL] path]);
        report[@"amd_attached"]=@(amd!=nil);
        NSMutableURLRequest *req2=[request mutableCopy];
        for(NSString *f in amd)[req2 setValue:amd[f] forHTTPHeaderField:f];
        NSURLRequest *base=amd?[req2 copy]:request;
        report[@"request_headers"]=[[[base allHTTPHeaderFields] allKeys] sortedArrayUsingSelector:@selector(compare:)];

        NSArray *md=@[@"X-Apple-I-MD",@"X-Apple-I-MD-M",@"X-Apple-I-MD-RINFO",@"X-Apple-I-MD-LU"];
        NSArray *identity=@[@"X-Apple-I-SRL-NO",@"X-Mme-Device-Id",@"X-MMe-Client-Info"];
        NSMutableArray *both=[md mutableCopy];[both addObjectsFromArray:identity];
        /* RW_DUMP=1: emit the fully-prepared request (all headers + body) so another
         * host can replay it verbatim. Used to test Mac-prepared, phone-sent requests. */
        if(getenv("RW_DUMP")){
            NSMutableDictionary *dump=[NSMutableDictionary dictionary];
            dump[@"url"]=[[base URL] absoluteString];
            dump[@"method"]=[base HTTPMethod]?:@"POST";
            dump[@"headers"]=[base allHTTPHeaderFields]?:@{};
            dump[@"body_b64"]=[[base HTTPBody] base64EncodedStringWithOptions:0]?:@"";
            report[@"request_dump"]=dump;
            NSData *dj=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
            fwrite(dj.bytes,1,dj.length,stdout);putchar('\n');return 0;
        }
        /* RW_REPLAY=<path>: send a request dumped by another host, verbatim. */
        const char *rp=getenv("RW_REPLAY");
        if(rp){
            NSData *rb=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:rp]];
            id rd=rb?[NSJSONSerialization JSONObjectWithData:rb options:0 error:NULL]:nil;
            id dump=[rd isKindOfClass:NSDictionary.class]?rd[@"request_dump"]:nil;
            if(![dump isKindOfClass:NSDictionary.class]){report[@"error"]=@"bad_replay";goto done;}
            NSMutableURLRequest *rr=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:dump[@"url"]]];
            rr.HTTPMethod=dump[@"method"]?:@"POST";
            for(NSString *f in dump[@"headers"])[rr setValue:dump[@"headers"][f] forHTTPHeaderField:f];
            NSString *b64=dump[@"body_b64"];
            if([b64 isKindOfClass:NSString.class]&&b64.length)rr.HTTPBody=[[NSData alloc] initWithBase64EncodedString:b64 options:0];
            NSMutableDictionary *one=[NSMutableDictionary dictionary];
            one[@"replay"]=rw_send(rr,@[]);
            report[@"sent"]=one;
            goto done;
        }
        NSMutableDictionary *sent=[NSMutableDictionary dictionary];
        sent[@"full_with_amd"]=rw_send(base,@[]);
        sent[@"drop_i_md"]=rw_send(base,md);
        sent[@"drop_identity"]=rw_send(base,identity);
        sent[@"drop_i_md_and_identity"]=rw_send(base,both);
        sent[@"no_amd_full"]=rw_send(request,@[]);
        report[@"sent"]=sent;
    }@catch(NSException *ex){report[@"exception"]=ex.name?:@"?";}
done:;
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
    fwrite(json.bytes,1,json.length,stdout);putchar('\n');
    return 0;
}}

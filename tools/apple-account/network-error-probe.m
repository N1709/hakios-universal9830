/* Passive completion metadata and selected machine-proof presence booleans.
 * Never logs credential headers, request bodies or authentication result values.
 * Explicitly armed one-shot comparison forwards a phone PET privately in memory.
 * Failed response summaries expose only bounded numeric codes/fixed categories. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "credential-handoff.h"
#include "pet-comparison-trigger.h"
static RWCredentialHandoff *handoff;
static id authResultValue(NSDictionary *result,const char *symbol) {
    NSString *__unsafe_unretained *key=(NSString *__unsafe_unretained *)dlsym(RTLD_DEFAULT,symbol);
    return key && [*key isKindOfClass:NSString.class]?result[*key]:nil;
}

static int output=-1;
static void (*originalFinish)(id,SEL,NSError *);
static void (*originalAuthenticate)(id,SEL,id,id);
static void (*originalAccountCompletion)(id,SEL,id,id,id);
static id (*originalAccountError)(id,SEL);
@interface NSObject (AccountErrorProbe)
- (NSInteger)statusCode;
@end
static NSArray *errors(NSError *error) {
    NSMutableArray *chain=[NSMutableArray array];
    for(unsigned n=0;n<4 && [error isKindOfClass:NSError.class];n++) {
        NSString *domain=error.domain;
        [chain addObject:@{@"domain":domain.length<=128?domain:@"other",@"code":@(error.code)}];
        error=error.userInfo[NSUnderlyingErrorKey];
    }
    return chain;
}
static void record(NSDictionary *entry) {
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:entry options:0 error:NULL];
    if(output>=0 && bytes) @synchronized(NSProcessInfo.processInfo) {
        write(output,bytes.bytes,bytes.length);write(output,"\n",1);
    }
}
static void finished(NSURLSessionTask *task, SEL cmd, NSError *error) {
    NSString *host=task.currentRequest.URL.host.lowercaseString;
    BOOL selftest=getenv("RW_NETWORK_PROBE_SELFTEST")!=NULL;
    if([host hasSuffix:@".apple.com"] || [host hasSuffix:@".icloud.com"] ||
       (selftest && [host isEqual:@"127.0.0.1"])) {
        NSURLResponse *response=task.response;
        NSInteger status=[response isKindOfClass:NSHTTPURLResponse.class]?[(NSHTTPURLResponse *)response statusCode]:0;
        NSMutableDictionary *entry=[@{@"event":@"network_completion",@"host":host,@"status":@(status),
            @"errors":errors(error),@"pid":@(getpid()),
            @"validation_header_present":@([task.currentRequest valueForHTTPHeaderField:@"X-Mme-Nas-Qualify"].length>0)} mutableCopy];
        if([host isEqual:@"setup.icloud.com"]) {
            NSString *legacyProof=[task.currentRequest valueForHTTPHeaderField:@"X-Apple-MD"];
            NSString *legacyMachine=[task.currentRequest valueForHTTPHeaderField:@"X-Apple-MD-M"];
            entry[@"legacy_icloud_provisioning"]=@{
                @"proof_present":@(legacyProof.length>0),@"machine_present":@(legacyMachine.length>0),
                @"proof_matches_authkit_alias":@(legacyProof.length>0 && [legacyProof isEqual:[task.currentRequest valueForHTTPHeaderField:@"X-Apple-I-MD"]]),
                @"machine_matches_authkit_alias":@(legacyMachine.length>0 && [legacyMachine isEqual:[task.currentRequest valueForHTTPHeaderField:@"X-Apple-I-MD-M"]])};
        }
        entry[@"native_signing_error_header_present"]=@([task.currentRequest valueForHTTPHeaderField:@"X-MMe-Nas-E"]!=nil);
        NSDictionary *(*compare)(NSDictionary *)=(void *)dlsym(RTLD_DEFAULT,"rw_auth_lab_compare_identity");
        if(compare && [host isEqual:@"setup.icloud.com"]) {
            NSMutableDictionary *metadata=[NSMutableDictionary dictionary];
            for(NSString *field in @[@"X-Mme-Device-Id",@"X-Apple-I-MD-LU",@"X-Apple-I-MD-M",@"X-Apple-I-MD-RINFO",@"X-Apple-I-SRL-NO",@"X-MMe-Client-Info"])
                if([task.currentRequest valueForHTTPHeaderField:field])metadata[field]=[task.currentRequest valueForHTTPHeaderField:field];
            entry[@"identity_comparison"]=compare(metadata);
        }
        if([host isEqual:@"setup.icloud.com"] && [task.currentRequest.URL.path isEqual:@"/setup/signin/v2/login"])
        {
            entry[@"credential_handoff"]=[handoff compareAuthorization:[task.currentRequest valueForHTTPHeaderField:@"Authorization"]];
            entry[@"alternate_account_identifier"]=[handoff compareAltDSID:[task.currentRequest valueForHTTPHeaderField:@"X-Apple-ADSID"]];
            NSHTTPURLResponse *http=[response isKindOfClass:NSHTTPURLResponse.class]?(id)response:nil;
            id challenge=[http valueForHTTPHeaderField:@"WWW-Authenticate"];
            NSString *scheme=@"absent";
            if([challenge isKindOfClass:NSString.class] && [challenge length]<=4096) {
                NSString *lower=[challenge lowercaseString];
                scheme=[lower hasPrefix:@"basic"]?@"basic":[lower hasPrefix:@"bearer"]?@"bearer":@"other";
            }
            entry[@"server_authentication_challenge_scheme"]=scheme;
        }
        record(entry);
    }
    originalFinish(task,cmd,error);
}
static BOOL replyABI(id completion) {
    if(!completion)return NO;
    struct Block {void *isa;int flags,reserved;void *invoke;void *descriptor;};
    const struct Block *block=(__bridge const void *)completion;
    if(!(block->flags&(1<<30)))return NO;
    const uint8_t *descriptor=block->descriptor;descriptor+=2*sizeof(uintptr_t);
    if(block->flags&(1<<25))descriptor+=2*sizeof(void *);
    const char *signature=*(const char *const *)descriptor;
    NSMethodSignature *s=signature?[NSMethodSignature signatureWithObjCTypes:signature]:nil;
    return s.numberOfArguments==3 && !strcmp(s.methodReturnType,"v") &&
        [s getArgumentTypeAtIndex:1][0]=='@' && [s getArgumentTypeAtIndex:2][0]=='@';
}
static void authenticate(id self, SEL cmd, id context, id completion) {
    if(!replyABI(completion)){originalAuthenticate(self,cmd,context,completion);return;}
    void (^reply)(id,NSError *)=completion;
    originalAuthenticate(self,cmd,context,^(id result,NSError *error){
        NSMutableDictionary *entry=[@{@"event":@"authentication_completion",@"error_present":@(error!=nil),
            @"errors":errors(error),@"pid":@(getpid()),@"result_available":@(result!=nil)} mutableCopy];
        if([result isKindOfClass:NSDictionary.class]) {
            NSMutableDictionary *presence=[NSMutableDictionary dictionary];
            for(NSString *symbol in @[@"AKAuthenticationUsernameKey",@"AKAuthenticationPasswordKey",@"AKAuthenticationDSIDKey",@"AKAuthenticationAlternateDSIDKey"]) {
                NSString *__unsafe_unretained *key=(NSString *__unsafe_unretained *)dlsym(RTLD_DEFAULT,symbol.UTF8String);
                if(key && [*key isKindOfClass:NSString.class])
                    presence[symbol]=@(CFDictionaryContainsKey((__bridge CFDictionaryRef)result,(__bridge const void *)*key));
            }
            entry[@"known_result_key_presence"]=presence;
        }
        NSDictionary *values=!error && [result isKindOfClass:NSDictionary.class]?result:nil;
        [handoff captureUsername:authResultValue(values,"AKAuthenticationUsernameKey")
            password:authResultValue(values,"AKAuthenticationPasswordKey") dsid:authResultValue(values,"AKAuthenticationDSIDKey")];
        [handoff captureAltDSID:authResultValue(values,"AKAuthenticationAlternateDSIDKey")];
        rw_comparison_capture(values?@{@"username":authResultValue(values,"AKAuthenticationUsernameKey")?:NSNull.null,
            @"pet":authResultValue(values,"AKAuthenticationPasswordKey")?:NSNull.null,
            @"dsid":authResultValue(values,"AKAuthenticationDSIDKey")?:NSNull.null,
            @"alt_dsid":authResultValue(values,"AKAuthenticationAlternateDSIDKey")?:NSNull.null}:nil);
        record(entry);
        reply(result,error);
    });
}
/* Never return raw server text, arbitrary field names, or token values. */
__attribute__((visibility("default"))) NSDictionary *rw_network_probe_failed_response(NSData *data,NSInteger status) {
    if(status<400 || status>=600)return @{@"response_inspected":@NO};
    if(![data isKindOfClass:NSData.class] || !data.length)return @{@"response_inspected":@YES,@"format":@"empty"};
    if(data.length>65536)return @{@"response_inspected":@NO,@"format":@"oversized"};
    id root=[NSPropertyListSerialization propertyListWithData:data options:0 format:NULL error:NULL];
    NSString *format=@"plist";
    if(!root){root=[NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];format=@"json";}
    if(![root isKindOfClass:NSDictionary.class])return @{@"response_inspected":@YES,@"format":@"other"};
    NSMutableDictionary *codes=[NSMutableDictionary dictionary];
    NSString *category=@"unclassified";
    NSMutableArray *containers=[NSMutableArray arrayWithObject:root];
    for(NSString *key in @[@"Status",@"status",@"error"])
        if([root[key] isKindOfClass:NSDictionary.class])[containers addObject:root[key]];
    NSRegularExpression *numeric=[NSRegularExpression regularExpressionWithPattern:@"\\A-?[0-9]{1,12}\\z" options:0 error:NULL];
    for(NSUInteger index=0;index<containers.count;index++) {
        NSDictionary *container=containers[index];
        for(NSString *key in @[@"status",@"statusCode",@"status-code",@"errorCode",@"error-code",@"code",@"ec"] ) {
            id value=container[key];NSString *text=nil;
            if([value isKindOfClass:NSNumber.class])text=[value stringValue];
            else if([value isKindOfClass:NSString.class] && [value length]<=13)text=value;
            if(text && [numeric numberOfMatchesInString:text options:0 range:NSMakeRange(0,text.length)]==1)
                codes[[NSString stringWithFormat:@"%lu.%@",(unsigned long)index,key]]=@(text.longLongValue);
        }
        for(NSString *key in @[@"statusMessage",@"status-message",@"message",@"reason",@"em"]) {
            id value=container[key];if(![value isKindOfClass:NSString.class] || [value length]>2048)continue;
            NSString *lower=[value lowercaseString];
            if([lower containsString:@"mid"] && [lower containsString:@"invalid"])category=@"machine_identity_rejected";
            else if([lower containsString:@"password"] && ([lower containsString:@"incorrect"] || [lower containsString:@"invalid"]))category=@"credentials_rejected";
            else if([lower containsString:@"verification"] && [lower containsString:@"code"])category=@"verification_code_related";
            else if([lower containsString:@"locked"] || [lower containsString:@"disabled"])category=@"account_restricted";
        }
    }
    return @{@"response_inspected":@YES,@"format":format,@"numeric_codes":codes,@"message_category":category};
}
static void accountCompletion(id self, SEL cmd, id data, id response, id error) {
    NSInteger status=[response isKindOfClass:NSHTTPURLResponse.class]?[(NSHTTPURLResponse *)response statusCode]:0;
    record(@{@"event":@"account_request_completion",@"request_class":NSStringFromClass([self class]),
        @"status":@(status),@"errors":errors(error),@"pid":@(getpid())});
    if([NSStringFromClass([self class]) isEqual:@"AALoginAccountRequest"] && status>=400 && status<600)
        record(@{@"event":@"account_failed_response_summary",@"pid":@(getpid()),
            @"summary":rw_network_probe_failed_response(data,status)});
    BOOL compareNeeded=[NSStringFromClass([self class]) isEqual:@"AALoginAccountRequest"] && status==401;
    originalAccountCompletion(self,cmd,data,response,error);
    if(compareNeeded)rw_comparison_after_failure(record);
}
static id accountError(id self,SEL cmd) {
    id error=originalAccountError(self,cmd);
    if([error isKindOfClass:NSError.class])record(@{@"event":@"account_response_error",
        @"response_class":NSStringFromClass([self class]),@"errors":errors(error),@"pid":@(getpid())});
    return error;
}
__attribute__((visibility("default"))) int rw_network_probe_install(void) {
    static dispatch_once_t once;static BOOL installed;
    dispatch_once(&once,^{
        Class task=objc_getClass("__NSCFLocalSessionTask");
        Method finish=task?class_getInstanceMethod(task,sel_registerName("_onqueue_didFinishWithError:")):NULL;
        if(!finish || strcmp(method_getTypeEncoding(finish),"v24@0:8@16"))return;
        output=open("/var/mobile/Library/RedwoodAppleAccountLab/network-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
        if(output<0)return;
        handoff=[RWCredentialHandoff new];
        originalFinish=(void *)method_getImplementation(finish);
        method_setImplementation(finish,(IMP)finished);
        Class controller=objc_getClass("AKAppleIDAuthenticationController");
        Method method=controller?class_getInstanceMethod(controller,sel_registerName("authenticateWithContext:completion:")):NULL;
        if(method && !strcmp(method_getTypeEncoding(method),"v32@0:8@16@?24")) {
            originalAuthenticate=(void *)method_getImplementation(method);
            method_setImplementation(method,(IMP)authenticate);
        }
        Class requestClass=objc_getClass("AARequest");
        Method account=requestClass?class_getInstanceMethod(requestClass,sel_registerName("_handleDataTaskCompletionWithData:response:error:")):NULL;
        if(account && !strcmp(method_getTypeEncoding(account),"v40@0:8@16@24@32")) {
            originalAccountCompletion=(void *)method_getImplementation(account);
            method_setImplementation(account,(IMP)accountCompletion);
        }
        Class responseClass=objc_getClass("AAResponse");
        Method accountGetter=responseClass?class_getInstanceMethod(responseClass,sel_registerName("error")):NULL;
        if(accountGetter && !strcmp(method_getTypeEncoding(accountGetter),"@16@0:8")) {
            originalAccountError=(void *)method_getImplementation(accountGetter);
            method_setImplementation(accountGetter,(IMP)accountError);
        }
        installed=YES;
        record(@{@"event":@"network_probe_installed",@"pid":@(getpid()),@"auth_callback_hooked":@(originalAuthenticate!=NULL),
            @"account_completion_hooked":@(originalAccountCompletion!=NULL),@"account_error_hooked":@(originalAccountError!=NULL)});
    });
    return installed;
}
__attribute__((constructor)) static void start(void) {
    dispatch_async(dispatch_get_main_queue(),^{rw_network_probe_install();});
}

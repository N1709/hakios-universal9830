/* Native server error codes/flags only; no response, token or raw-text logging. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
static inline NSDictionary *rw_srp_server_summary(id response) {
    NSMutableDictionary *out=[@{@"response_available":@(response!=nil)} mutableCopy];
    SEL code=sel_registerName("errorCode");Method method=class_getInstanceMethod([response class],code);
    if(method && !strcmp(method_getTypeEncoding(method),"q16@0:8"))out[@"server_error_code"]=@(((NSInteger(*)(id,SEL))objc_msgSend)(response,code));
    for(NSString *name in @[@"isAnisetteResyncRequired",@"isAnisetteReprovisioningRequired",@"isURLSwitchingRequired",@"isRetryRequired",@"isSecondaryActionRequired",@"isServerUIRequired"]) {
        SEL selector=NSSelectorFromString(name);Method flag=class_getInstanceMethod([response class],selector);
        if(flag && !strcmp(method_getTypeEncoding(flag),"B16@0:8"))out[name]=@(((BOOL(*)(id,SEL))objc_msgSend)(response,selector));
    }
    return out;
}

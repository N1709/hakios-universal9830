/* Selected public service labels/flags only; unknown strings never leave process. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
static inline id rw_context_object(id context,NSString *name) {
    SEL selector=NSSelectorFromString(name);
    Method method=class_getInstanceMethod([context class],selector);
    if(!method || strcmp(method_getTypeEncoding(method),"@16@0:8"))return nil;
    return ((id(*)(id,SEL))objc_msgSend)(context,selector);
}
static inline NSDictionary *rw_context_summary(id context) {
    NSMutableDictionary *out=[NSMutableDictionary dictionary];
    for(NSString *name in @[@"authenticationType",@"serviceType",@"shouldRequestShortLivedToken",@"shouldUpdatePersistentServiceTokens",@"_shouldSendIdentityTokenForRemoteUI",@"_shouldSendGrandSlamTokensForRemoteUI"]) {
        SEL selector=NSSelectorFromString(name);Method method=class_getInstanceMethod([context class],selector);
        if(!method)continue;const char *type=method_getTypeEncoding(method);
        if(!strcmp(type,"Q16@0:8"))out[name]=@(((unsigned long long(*)(id,SEL))objc_msgSend)(context,selector));
        else if(!strcmp(type,"q16@0:8"))out[name]=@(((long long(*)(id,SEL))objc_msgSend)(context,selector));
        else if(!strcmp(type,"B16@0:8"))out[name]=@(((BOOL(*)(id,SEL))objc_msgSend)(context,selector));
        else if(!strcmp(type,"c16@0:8"))out[name]=@(((signed char(*)(id,SEL))objc_msgSend)(context,selector)!=0);
    }
    NSArray *allowed=@[@"com.apple.gs.idms.pet",@"com.apple.gs.icloud.auth",@"com.apple.mobileme",@"com.apple.accountsd",@"com.apple.Preferences",@"com.apple.account.AppleAccount"];
    for(NSString *name in @[@"serviceIdentifier",@"serviceIdentifiers",@"desiredInternalTokens"]) {
        id value=rw_context_object(context,name);NSMutableArray *items=[NSMutableArray array];
        if([value isKindOfClass:NSString.class])[items addObject:value];
        else if(([value isKindOfClass:NSArray.class] || [value isKindOfClass:NSSet.class]) && [value count]<=32)for(id item in value)[items addObject:item];
        NSMutableSet *known=[NSMutableSet set];NSUInteger unknown=0;
        for(id item in items)if([item isKindOfClass:NSString.class] && [allowed containsObject:item])[known addObject:item];else unknown++;
        out[name]=@{@"present":@(value!=nil),@"known_labels":[known.allObjects sortedArrayUsingSelector:@selector(compare:)],@"unclassified_count":@(unknown)};
    }
    out[@"app_provided_context_present"]=@(rw_context_object(context,@"appProvidedContext")!=nil);
    out[@"app_provided_data_present"]=@(rw_context_object(context,@"appProvidedData")!=nil);
    return out;
}

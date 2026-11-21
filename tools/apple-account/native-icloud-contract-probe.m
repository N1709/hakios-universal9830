/* Account-free native request contract comparison. Never sends a request. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface NSObject (ContractProbe)
- (id)initWithAccount:(id)account delegates:(id)delegates;
- (id)initWithAccount:(id)account;
- (NSURLRequest *)urlRequest;
+ (id)appleIDClientIdentifier;
+ (id)udid;
+ (id)clientInfoHeader;
+ (id)userAgentHeader;
@end
int main(void) { @autoreleasepool {
 dlopen("/System/Library/PrivateFrameworks/IntlPreferences.framework/IntlPreferences",RTLD_NOW);
 dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
 NSMutableDictionary *report=[@{@"account_supplied":@NO,@"request_sent":@NO} mutableCopy];
 Class device=objc_getClass("AADeviceInfo");
 for(NSString *name in @[@"AALoginAccountRequest",@"AALoginDelegatesRequest"]) {
  Class cls=objc_getClass(name.UTF8String);Method method=cls?class_getInstanceMethod(cls,@selector(initWithAccount:delegates:)):NULL;
  NSMutableDictionary *item=[@{@"class_present":@(cls!=Nil),@"initializer_present":@(method!=NULL)} mutableCopy];
  if(method && !strcmp(method_getTypeEncoding(method),"@32@0:8@16@24")) {
   id object=[[cls alloc] initWithAccount:nil delegates:@{@"com.apple.mobileme":@{}}];
   NSURLRequest *request=[object urlRequest];
   if([request isKindOfClass:NSURLRequest.class]) {
    item[@"path"]=request.URL.path?:@"";
    item[@"native_icloud_client_info_matches"]=@([[request valueForHTTPHeaderField:@"X-Mme-Client-Info"] isEqual:[device clientInfoHeader]]);
    item[@"native_icloud_user_agent_matches"]=@([[request valueForHTTPHeaderField:@"User-Agent"] isEqual:[device userAgentHeader]]);
    NSMutableDictionary *present=[NSMutableDictionary dictionary];
    for(NSString *key in @[@"Authorization",@"X-Apple-ADSID",@"X-Apple-I-MD",@"X-Apple-I-MD-M",@"X-Mme-Device-Id",@"X-MMe-Nas-Qualify"]) {
     BOOL found=NO;
     for(NSString *field in request.allHTTPHeaderFields)if([field caseInsensitiveCompare:key]==NSOrderedSame)found=YES;
     present[key]=@(found);
    }
    item[@"header_presence"]=present;
    id root=request.HTTPBody?[NSPropertyListSerialization propertyListWithData:request.HTTPBody options:0 format:NULL error:NULL]:nil;
    if([root isKindOfClass:NSDictionary.class]) {
     item[@"fixture_root_keys"]=[[root allKeys] sortedArrayUsingSelector:@selector(compare:)];
     id u=root[@"userInfo"];
     if([u isKindOfClass:NSDictionary.class])item[@"fixture_user_keys"]=[[u allKeys] sortedArrayUsingSelector:@selector(compare:)];
     id user=root[@"userInfo"];id client=[user isKindOfClass:NSDictionary.class]?user[@"client-id"]:root[@"client-id"];
     item[@"client_id_matches_native_appleid_id"]=@([client isEqual:[device appleIDClientIdentifier]]);
     item[@"client_id_matches_native_device_id"]=@([client isEqual:[device udid]]);
    }
   }
  }
  report[name]=item;
 }
 NSData *output=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
 if(output)fwrite(output.bytes,1,output.length,stdout);
 return output?0:1;
}}

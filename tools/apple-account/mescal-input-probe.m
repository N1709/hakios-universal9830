/* Inspect native signing-input transformation with synthetic markers only.
 * No requests sent; no account APIs, real credentials or signing calls.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface NSObject (RWInputProbe)
- (id)initWithRequest:(NSURLRequest *)request;
+ (id)_signedActionDataFromRequest:(id)request policy:(id)policy;
@end
int main(void){@autoreleasepool{
 dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 Class mescal=NSClassFromString(@"AMSMescal"),requestClass=NSClassFromString(@"AMSURLRequest");
 Method method=class_getClassMethod(mescal,@selector(_signedActionDataFromRequest:policy:));
 Method init=class_getInstanceMethod(requestClass,@selector(initWithRequest:));
 if(!method || strcmp(method_getTypeEncoding(method),"@32@0:8@16@24") || !init || strcmp(method_getTypeEncoding(init),"@24@0:8@16"))return 3;
 NSString *marker=@"SYNTHETIC-CREDENTIAL-MARKER-NO-ACCOUNT";
 NSString *url=[NSString stringWithFormat:@"https://example.invalid/unsent?clientId=synthetic-client&password=%@",marker];
 NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
 id nativeRequest=[[requestClass alloc] initWithRequest:request];
 NSMutableArray *rows=[NSMutableArray array];
 for(NSString *field in @[@"clientId",@"password"]){
  id value=[mescal _signedActionDataFromRequest:nativeRequest policy:@{@"fields":@[field],@"headers":@[]}];
  NSMutableDictionary *row=[@{@"policy_field":field,@"data_result":@([value isKindOfClass:NSData.class]),@"request_sent":@NO} mutableCopy];
  if([value isKindOfClass:NSData.class]){
   row[@"byte_count"]=@([value length]);
   row[@"contains_synthetic_credential_marker"]=@([value rangeOfData:[marker dataUsingEncoding:NSUTF8StringEncoding] options:0 range:NSMakeRange(0,[value length])].location!=NSNotFound);
  }[rows addObject:row];
 }
 NSData *output=[NSJSONSerialization dataWithJSONObject:rows options:0 error:NULL];
 if(output)fwrite(output.bytes,1,output.length,stdout);return output?0:1;
}}

/* Native app-token outcome observer. No tokens, credentials or parameters logged. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
typedef struct __AppleIDAuthSupportTokenData *RWToken;
static id(*nativeFetch)(id,SEL,RWToken,id,id,id,NSError **);
static int output=-1;
static NSString *symbolKey(const char *name){NSString *__unsafe_unretained *p=(NSString *__unsafe_unretained *)dlsym(RTLD_DEFAULT,name);return p && [*p isKindOfClass:NSString.class]?*p:nil;}
static id fetch(id self,SEL cmd,RWToken token,id url,id services,id parameters,NSError **outError){
 id result=nativeFetch(self,cmd,token,url,services,parameters,outError);
 NSMutableArray *errors=[NSMutableArray array];NSError *error=outError?*outError:nil;
 for(unsigned i=0;i<4 && [error isKindOfClass:NSError.class];i++){
  NSMutableDictionary *entry=[@{@"domain":[error.domain hasPrefix:@"com.apple."]?error.domain:@"other",@"code":@(error.code)} mutableCopy];
  NSString *key=symbolKey("kAppleIDAuthSupportStatus");id status=key?error.userInfo[key]:nil;
  if([status isKindOfClass:NSNumber.class])entry[@"server_status"]=status;
  [errors addObject:entry];error=error.userInfo[NSUnderlyingErrorKey];
 }
 NSMutableDictionary *entry=[@{@"event":@"app_token_service_result",@"pid":@(getpid()),@"result_present":@(result!=nil),@"errors":errors} mutableCopy];
 NSDictionary *(*compare)(NSDictionary *)=(void *)dlsym(RTLD_DEFAULT,"rw_auth_lab_compare_identity");
 if(compare && [parameters isKindOfClass:NSDictionary.class]){
  NSString *cpd=symbolKey("kAppleIDAuthSupportClientProvidedData"),*extra=symbolKey("kAppleIDAuthSupportAdditionalHeaders");
  for(NSString *key in @[@"cpd",@"extra"]){
   id source=[key isEqual:@"cpd"]?(cpd?parameters[cpd]:nil):(extra?parameters[extra]:nil);
   if(![source isKindOfClass:NSDictionary.class])continue;
   NSMutableDictionary *selected=[NSMutableDictionary dictionary];
   for(NSString *field in @[@"X-Mme-Device-Id",@"X-Apple-I-MD-LU",@"X-Apple-I-MD-M",@"X-Apple-I-MD-RINFO",@"X-Apple-I-SRL-NO",@"X-MMe-Client-Info"])if(source[field])selected[field]=source[field];
   entry[key]=compare(selected);
  }
 }
 if(output>=0){NSData *bytes=[NSJSONSerialization dataWithJSONObject:entry options:0 error:NULL];if(bytes){write(output,bytes.bytes,bytes.length);write(output,"\n",1);}}
 return result;
}
__attribute__((visibility("default"))) BOOL rw_app_token_probe_install(void){
 static BOOL installed=NO;if(installed)return YES;
 SEL target=sel_registerName("appleIDAuthSupportCopyAppTokensWithMasterToken:authURL:serviceIds:authParams:error:");
 unsigned n=0;Class *classes=objc_copyClassList(&n);Method found=NULL;unsigned matches=0;
 for(unsigned i=0;i<n;i++){
  unsigned count=0;Method *methods=class_copyMethodList(classes[i],&count);
  for(unsigned j=0;j<count;j++)if(method_getName(methods[j])==target){found=methods[j];matches++;}
  free(methods);
 }free(classes);
 if(matches!=1 || strcmp(method_getTypeEncoding(found),"@56@0:8^{__AppleIDAuthSupportTokenData=}16@24@32@40^@48"))return NO;
 nativeFetch=(void *)method_getImplementation(found);method_setImplementation(found,(IMP)fetch);installed=YES;return YES;
}
__attribute__((constructor)) static void start(void){
 if(![NSProcessInfo.processInfo.processName isEqual:@"akd"])return;
 dispatch_async(dispatch_get_main_queue(),^{
  output=open("/var/mobile/Library/RedwoodAppleAccountLab/request-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  BOOL installed=rw_app_token_probe_install();if(output>=0)dprintf(output,"{\"event\":\"app_token_probe_installed\",\"installed\":%s,\"pid\":%d}\n",installed?"true":"false",getpid());
 });
}

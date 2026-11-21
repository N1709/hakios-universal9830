/* Runtime method encodings only; no account operations. */
#import <Foundation/Foundation.h>
#import <Accounts/Accounts.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
int main(void){@autoreleasepool{
 dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
 dlopen("/System/Library/PrivateFrameworks/CoreCDP.framework/CoreCDP",RTLD_NOW);
 for(NSString *name in @[@"ACAccountStore",@"AASignInFlowController",@"AAiCloudLoginAccountRequester",@"CDPContext",@"CDPStateController",@"CDPStateMachine",@"CDPAccount"]){
  Class cls=objc_getClass(name.UTF8String);unsigned count=0;Method *list=class_copyMethodList(cls,&count);
  for(unsigned i=0;i<count;i++){const char *s=sel_getName(method_getName(list[i]));
   if((!strcmp(s,"_verifyLoginResponseForiCloudAccount:withSuccess:error:completion:") || !strcmp(s,"_validateAndEnrollCDPStateForAccount:withCDPContext:completion:"))) {
    IMP imp=method_getImplementation(list[i]);Dl_info info;
    if(dladdr((void *)imp,&info) && info.dli_fbase) {
     const struct mach_header_64 *header=info.dli_fbase;
     const struct load_command *lc=(const void *)(header+1);
     for(unsigned j=0;j<header->ncmds;j++){
      if(lc->cmd==LC_SEGMENT_64){const struct segment_command_64 *seg=(const void *)lc;
       if(!strcmp(seg->segname,"__TEXT"))printf("method=%s address=0x%llx\n",s,(unsigned long long)((uintptr_t)imp-(uintptr_t)header+seg->vmaddr));}
      lc=(const void *)((const char *)lc+lc->cmdsize);
     }
    }
   }
   if(strcasestr(s,"valid") || strcasestr(s,"verif") || strcasestr(s,"save") || strcasestr(s,"signIn") || strcasestr(s,"register"))
    printf("%s %s %s\n",name.UTF8String,s,method_getTypeEncoding(list[i]));
  }free(list);
 }
 return 0;
}}

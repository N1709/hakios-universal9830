/* Framework task method metadata only. No task creation/account authentication. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
int main(void){@autoreleasepool{
 dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 unsigned count=0;Class *classes=objc_copyClassList(&count);
 for(unsigned i=0;i<count;i++){
  const char *name=class_getName(classes[i]);
  if(strncmp(name,"AMS",3) || (!strstr(name,"Auth") && strcmp(name,"AMSTask") && strcmp(name,"AMSPromise") && strcmp(name,"AMSMutablePromise") && strcmp(name,"AMSURLSession") && strcmp(name,"AMSURLResult") && !strstr(name,"MediaAccount")))continue;
  unsigned n=0;Method *methods=class_copyMethodList(classes[i],&n);
  for(unsigned j=0;j<n;j++){
   const char *sel=sel_getName(method_getName(methods[j]));
   if(strcasestr(sel,"perform") || strcasestr(sel,"authenticat") || strcasestr(sel,"finish") || strcasestr(sel,"completion") || strcasestr(sel,"error") || strcasestr(sel,"response") || strcasestr(sel,"data") || strcasestr(sel,"result"))printf("%s %s %s\n",name,sel,method_getTypeEncoding(methods[j]));
  }free(methods);
 }free(classes);return 0;
}}

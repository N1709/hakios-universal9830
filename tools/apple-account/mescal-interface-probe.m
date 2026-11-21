/* Objective-C interface metadata only. No signing or account operations. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
static void list(Class cls,const char *name,char kind){
 unsigned n=0;Method *methods=class_copyMethodList(cls,&n);
 for(unsigned i=0;i<n;i++)printf("%c[%s %s] %s\n",kind,name,sel_getName(method_getName(methods[i])),method_getTypeEncoding(methods[i]));
 free(methods);
}
int main(void){@autoreleasepool{
 dlopen("/System/Library/PrivateFrameworks/PodcastsFoundation.framework/PodcastsFoundation",RTLD_NOW);
 dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 unsigned n=0;Class *classes=objc_copyClassList(&n);
 for(unsigned i=0;i<n;i++){
  const char *name=class_getName(classes[i]);
  BOOL bag=!strcmp(name,"AMSAnisette") || !strcmp(name,"AMSBagValue") || !strcmp(name,"AMSBag") || !strcmp(name,"IMURLBag");
  if(!bag && (strncmp(name,"AMS",3) || (!strstr(name,"Mescal") && !strstr(name,"SAP"))))continue;
  list(classes[i],name,'-');list(object_getClass(classes[i]),name,'+');
 }free(classes);return 0;
}}

/* Static method addresses only; no account/task creation or authentication. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
int main(void){ @autoreleasepool {
 dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 NSMutableArray *rows=[NSMutableArray array];
 for(NSString *name in @[@"AMSAuthenticateTask",@"AMSDelegateAuthenticateTask",@"AMSAuthKitUpdateTask",@"AMSAuthKitUpdateResult",@"AMSMescal"]){
  Class cls=NSClassFromString(name);
  if([name isEqual:@"AMSMescal"] || [name isEqual:@"AMSAuthKitUpdateResult"])cls=object_getClass(cls);
  unsigned count=0; Method *methods=class_copyMethodList(cls,&count);
  for(unsigned i=0;i<count;i++){
   IMP imp=method_getImplementation(methods[i]);Dl_info info;
   if(!dladdr((void *)imp,&info))continue;
   intptr_t slide=0;BOOL found=NO;
   for(uint32_t j=0;j<_dyld_image_count();j++)if((void *)_dyld_get_image_header(j)==info.dli_fbase){slide=_dyld_get_image_vmaddr_slide(j);found=YES;break;}
   if(found)[rows addObject:@{@"class":name,@"selector":NSStringFromSelector(method_getName(methods[i])),@"address":[NSString stringWithFormat:@"0x%llx",(unsigned long long)((uintptr_t)imp-slide)]}];
  }free(methods);
 }
 NSData *data=[NSJSONSerialization dataWithJSONObject:rows options:0 error:NULL];
 if(data)fwrite(data.bytes,1,data.length,stdout);
 return data?0:1;
}}

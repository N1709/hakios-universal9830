/* Read only two known framework constant strings, never live account data. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
int main(void){@autoreleasepool{
 dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices",RTLD_NOW);
 intptr_t slide=0;BOOL found=NO;
 for(uint32_t i=0;i<_dyld_image_count();i++){
  const char *name=_dyld_get_image_name(i);
  if(name && strstr(name,"/AppleMediaServices.framework/AppleMediaServices")){
   slide=_dyld_get_image_vmaddr_slide(i);
   if((uintptr_t)_dyld_get_image_header(i)-slide!=0x18fcc8000)return 2;
   found=YES;break;
  }
 }
 if(!found)return 3;
 uintptr_t addresses[]={0x1df7949e8,0x1df7a7ce8};
 NSMutableArray *strings=[NSMutableArray array];
 for(unsigned i=0;i<2;i++){
  id value=(__bridge id)(void *)(addresses[i]+slide);
  if(![value isKindOfClass:NSString.class] || ![NSStringFromClass([value class]) containsString:@"ConstantString"] || [value length]>256)return 4;
  [strings addObject:value];
 }
 NSData *data=[NSJSONSerialization dataWithJSONObject:strings options:0 error:NULL];if(data)fwrite(data.bytes,1,data.length,stdout);return 0;
}}

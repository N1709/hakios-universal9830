#import <Foundation/Foundation.h>
#import <objc/runtime.h>
int main(void){@autoreleasepool{
 NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];(void)session;
 for(NSString *name in @[@"NSURLSessionTask",@"__NSCFLocalSessionTask",@"__NSCFLocalDataTask",@"__NSCFURLSessionTask"]){
  Class cls=NSClassFromString(name);unsigned n=0;Method *methods=class_copyMethodList(cls,&n);
  for(unsigned i=0;i<n;i++){const char *selector=sel_getName(method_getName(methods[i]));if(strstr(selector,"didReceiveData") || strstr(selector,"didFinishWithError") || strstr(selector,"resume"))printf("%s %s %s\n",name.UTF8String,selector,method_getTypeEncoding(methods[i]));}free(methods);
 }return 0;
}}

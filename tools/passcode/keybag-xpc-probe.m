/* Diagnostic empty-to-empty native transaction only. No secret inputs. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dlfcn.h>
int main(void) { @autoreleasepool {
 void *h=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_NOW);
 int(*state)(CFDictionaryRef)=h?dlsym(h,"MKBGetDeviceLockState"):NULL;
 if(!state||state(NULL)!=3){fputs("Skipped: no-passcode state required\n",stderr);return 1;}
 Protocol *protocol=NSProtocolFromString(@"KBXPCProtocol");
 if(!protocol)return 1;
 NSXPCConnection *c=((id(*)(id,SEL,id,NSUInteger))objc_msgSend)([NSXPCConnection alloc],
  sel_registerName("initWithMachServiceName:options:"),@"com.apple.mobile.keybagd.xpc",0);
 c.remoteObjectInterface=[NSXPCInterface interfaceWithProtocol:protocol];
 [c resume];
 __block int result=1;
 id proxy=[c synchronousRemoteObjectProxyWithErrorHandler:^(NSError *e){
  printf("XPC transport: %s (%ld): %s\n",e.domain.UTF8String,(long)e.code,e.localizedDescription.UTF8String);
 }];
 void(^reply)(NSError *)=^(NSError *e){
  if(e)printf("keybagd reply: %s (%ld): %s\n",e.domain.UTF8String,(long)e.code,e.localizedDescription.UTF8String);
  else {puts("keybagd reply: success");result=0;}
 };
 ((void(*)(id,SEL,id,uint64_t,id,uint64_t,id,id))objc_msgSend)(proxy,
  sel_registerName("changeSystemSecretfromOldSecret:oldSize:toNewSecret:newSize:opaqueData:reply:"),
  nil,0,nil,0,nil,reply);
 [c invalidate];
 printf("native lock state afterward: %d\n",state(NULL));return result;
} }

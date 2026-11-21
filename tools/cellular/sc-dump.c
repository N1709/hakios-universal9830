/* MIT. Print SCDynamicStore keys matching a regex (default State:/Network/.*) -- a tiny scutil for the phone. */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
int main(int argc,char **argv)
{
    void *sc=dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration",RTLD_NOW);if (!sc) return 1;
    void *(*create)(CFAllocatorRef,CFStringRef,void *,void *)=dlsym(sc,"SCDynamicStoreCreate");
    CFArrayRef (*list)(void *,CFStringRef)=dlsym(sc,"SCDynamicStoreCopyKeyList");
    CFPropertyListRef (*get)(void *,CFStringRef)=dlsym(sc,"SCDynamicStoreCopyValue");
    void *st=create(NULL,CFSTR("sc-dump"),NULL,NULL);if (!st) return 1;
    CFStringRef pat=CFStringCreateWithCString(NULL,argc>1?argv[1]:"State:/Network/.*",kCFStringEncodingUTF8);
    CFArrayRef ks=list(st,pat);if (!ks) return 1;
    for (CFIndex i=0;i<CFArrayGetCount(ks);i++) {
        CFStringRef k=CFArrayGetValueAtIndex(ks,i);char b[256];CFStringGetCString(k,b,sizeof(b),kCFStringEncodingUTF8);
        printf("== %s\n",b);CFPropertyListRef v=get(st,k);if (v) {CFShow(v);CFRelease(v);}
    }
    return 0;
}

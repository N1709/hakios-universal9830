/* Read-only native MKB status, optionally test rejection of an invalid OLD
 * code on a device that has NO passcode. The requested new code is empty.
 * This program cannot set a nonempty device passcode. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static void trace(const char *message) {
 if(message && !strstr(message,"passcode=")) printf("MKB: %s\n",message);
}
static int raw_state(uint64_t *flags) {
 io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleKeyStore"));
 if(!s)return 0;
 io_connect_t c=0; kern_return_t r=IOServiceOpen(s,mach_task_self(),0,&c); IOObjectRelease(s);
 if(r)return 0;
 uint64_t input=0;uint32_t n=1;
 r=IOConnectCallScalarMethod(c,7,&input,1,flags,&n);IOServiceClose(c);return !r&&n==1;
}
int main(int argc,char **argv) {
 void *h=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_NOW);
 int (*state)(CFDictionaryRef)=h?dlsym(h,"MKBGetDeviceLockState"):NULL;
 int (*change)(CFDataRef,CFDataRef,CFDataRef)=h?dlsym(h,"MKBKeyBagChangeSystemSecret"):NULL;
 if(!state||!change)return 1;
 void (*set_log)(void (*)(const char *))=dlsym(h,"MKBSetLogFunction");
 if(set_log)set_log(trace);
 uint64_t before=0,after=0;
 if(!raw_state(&before))return 1;
 int native=state(NULL);printf("native MKB lock state: %d; raw AKS flags: %#llx\n",native,(unsigned long long)before);
 int reject=argc>1&&!strcmp(argv[1],"--reject-invalid-old");
 int noop=argc>1&&!strcmp(argv[1],"--empty-transaction");
 if(!reject&&!noop)return 0;
 if(native!=3||before!=6){fputs("Skipped: device is not in no-passcode state\n",stderr);return 1;}
 const UInt8 invalid[]="probe-invalid-old";
 CFDataRef old=CFDataCreate(NULL,invalid,reject?sizeof(invalid)-1:0), empty=CFDataCreate(NULL,NULL,0);
 int r=change(old,empty,NULL);
 CFRelease(old);CFRelease(empty);
 printf("native change result: %#x\n",(unsigned)r);
 if(!raw_state(&after)||after!=before){fputs("Unexpected state change; stop testing\n",stderr);return 1;}
 if(reject&&!r){fputs("Unexpected success; inspect native path before any passcode setup\n",stderr);return 1;}
 if(noop&&r){fputs("Native empty-to-empty transaction is not supported yet\n",stderr);return 1;}
 puts(reject?"PASS: invalid old code rejected through native service; system remains without passcode":
     "PASS: native empty-to-empty transaction; system remains without passcode");return 0;
}

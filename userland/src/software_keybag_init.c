/* Restore a software keybag BEFORE launchd/UI. No passcode input or KDF here.
 * Invalid/missing partial records fail closed. Active only with softpasscode=1.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define BAG_PATH "/private/var/keybags/systembag.kb"
static int enabled(void) {
 char args[1024]={0};size_t n=sizeof(args)-1;
 if(sysctlbyname("kern.bootargs",args,&n,NULL,0))return -1;
 char *save=NULL;
 for(char *t=strtok_r(args," ",&save);t;t=strtok_r(NULL," ",&save))
  if(!strcmp(t,"softpasscode=1"))return 1;
 return 0;
}
/* Return 0 absent, 1 parsed, -1 invalid. Never print serialized key material. */
static int record_data(CFDataRef *out) {
 /* Hold a private parent directory so a path replacement cannot redirect
  * the file lookup through a symlink between validation and open. */
 int dir=open("/private/var/keybags",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
 if(dir<0)return errno==ENOENT?0:-1;
 struct stat parent;
 if(fstat(dir,&parent)||parent.st_uid!=0||(parent.st_mode&0077)) {close(dir);return -1;}
 int fd=openat(dir,"systembag.kb",O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
 if(fd<0) {
  if(errno!=ENOENT){close(dir);return -1;}
  struct stat st;
  int partial=fstatat(dir,"systembag.kb.writing",&st,AT_SYMLINK_NOFOLLOW);
  int missing=partial<0&&errno==ENOENT;close(dir);
  if(!missing)return -1;
  return 0;
 }
 close(dir);
 struct stat st;
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_uid!=0||(st.st_mode&0077)||st.st_size<=0||st.st_size>65536) {
  close(fd);return -1;
 }
 size_t size=(size_t)st.st_size,got=0;uint8_t *buf=malloc(size);
 if(!buf){close(fd);return -1;}
 while(got<size) {ssize_t n=read(fd,buf+got,size-got);if(n<=0){free(buf);close(fd);return -1;}got+=(size_t)n;}
 close(fd);
 CFDataRef raw=CFDataCreate(NULL,buf,(CFIndex)size);
 volatile uint8_t *wipe=buf;for(size_t i=0;i<size;i++)wipe[i]=0;free(buf);
 if(!raw)return -1;
 CFPropertyListRef plist=CFPropertyListCreateWithData(NULL,raw,kCFPropertyListImmutable,NULL,NULL);
 CFRelease(raw);
 if(!plist||CFGetTypeID(plist)!=CFDictionaryGetTypeID()){if(plist)CFRelease(plist);return -1;}
 CFTypeRef keys=CFDictionaryGetValue((CFDictionaryRef)plist,CFSTR("KeyBagKeys"));
 if(!keys||CFGetTypeID(keys)!=CFDataGetTypeID()||CFDataGetLength((CFDataRef)keys)!=88||
    memcmp(CFDataGetBytePtr((CFDataRef)keys),"RW-SWKB1",8)) {CFRelease(plist);return -1;}
 *out=(CFDataRef)CFRetain(keys);CFRelease(plist);return 1;
}
static int restore(int inspect,int unprotected_test) {
 CFDataRef data=NULL;int found=record_data(&data);
 if(found<0){fputs("softkeybag-init: invalid or incomplete software keybag; UI startup refused\n",stderr);return 1;}
 if(!found){puts("softkeybag-init: no saved software keybag");return 0;}
 if(inspect){CFRelease(data);puts("softkeybag-init: private software keybag parsed");return 0;}
 io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleKeyStore"));
 if(!s){CFRelease(data);return 1;}
 CFTypeRef support=IORegistryEntryCreateCFProperty(s,CFSTR("RedwoodSoftwarePasscode"),NULL,0);
 int supported=support&&CFGetTypeID(support)==CFBooleanGetTypeID()&&CFBooleanGetValue((CFBooleanRef)support);
 if(support)CFRelease(support);
 io_connect_t c=0;kern_return_t r=supported?IOServiceOpen(s,mach_task_self(),0,&c):kIOReturnUnsupported;
 IOObjectRelease(s);if(r){CFRelease(data);return 1;}
 uint64_t bag=0,flags=0,system=0;uint32_t n=1;
 r=IOConnectCallMethod(c,6,NULL,0,CFDataGetBytePtr(data),(size_t)CFDataGetLength(data),&bag,&n,NULL,NULL);
 CFRelease(data);
 if(r||!bag||bag>15){IOServiceClose(c);return 1;}
 n=1;r=IOConnectCallScalarMethod(c,7,&bag,1,&flags,&n);
 if(!r&&unprotected_test) {
  uint64_t current=0;n=1;
  r=IOConnectCallScalarMethod(c,7,&system,1,&current,&n);
  if(!r&&(flags!=6||current!=6))r=kIOReturnNotPermitted;
 }
 if(!r) {
  uint64_t args[]={bag,0,0,0};
  r=IOConnectCallScalarMethod(c,5,args,4,NULL,NULL);
 }
 if(!r) {
  uint64_t current=0;n=1;
  r=IOConnectCallScalarMethod(c,7,&system,1,&current,&n);
  if(!r&&(current!=flags||(current!=1&&current!=6)))r=kIOReturnError;
 }
 IOConnectCallScalarMethod(c,4,&bag,1,NULL,NULL);IOServiceClose(c);
 if(r){fprintf(stderr,"softkeybag-init: restore failed (%#x); UI startup refused\n",r);return 1;}
 puts(flags==1?"softkeybag-init: protected bag restored LOCKED before first unlock":"softkeybag-init: no-passcode bag restored");
 return 0;
}
int main(int argc,char **argv) {
 if(argc==2&&!strcmp(argv[1],"--check"))return restore(1,0);
 if(argc==2&&!strcmp(argv[1],"--restore-unprotected-test"))return restore(0,1);
 if(getpid()!=1||argc<2){fputs("Use only as the PID 1 launchd handoff\n",stderr);return 1;}
 int on=enabled();
 if(on<0||(on&&restore(0,0))) {
  /* Exiting PID 1 panics; never continue to an unprotected UI on an error. */
  for(;;)pause();
 }
 char *launch=argv[1];
 if(!strcmp(launch,"/sbin/launchd.ios")&&access(launch,X_OK)&&!access("/sbin/launchd",X_OK))launch="/sbin/launchd";
 argv[1]=launch;execv(launch,argv+1);
 perror("softkeybag-init: exec launchd");for(;;)pause();
}

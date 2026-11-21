/* Root command arms one short appearance test. Does not lock, wake or reboot. */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <notify.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "preview_token.h"
#include "config.h"
int main(int argc,char **argv){@autoreleasepool{
    if(getuid()==0&&argc==2&&(!strcmp(argv[1],"enable")||!strcmp(argv[1],"disable")))
        return rw_aod_set(@"Enabled",!strcmp(argv[1],"enable"))?0:1;
    if(getuid()==0&&argc==2&&(!strcmp(argv[1],"wallpaper-on")||!strcmp(argv[1],"wallpaper-off")))
        return rw_aod_set(@"ShowWallpaper",!strcmp(argv[1],"wallpaper-on"))?0:1;
    if(getuid()==0 && argc==2 && !strcmp(argv[1],"status")){
        void *framework=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_NOW);
        int (*state)(CFDictionaryRef)=framework?dlsym(framework,"MKBGetDeviceLockState"):NULL;
        printf("native_keybag_state=%d\n",state?state(NULL):-1);
        io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleKeyStore"));
        if(service){io_connect_t connection=0;
            if(IOServiceOpen(service,mach_task_self(),0,&connection)==KERN_SUCCESS){
                uint64_t bag=0,flags=0;uint32_t count=1;
                if(IOConnectCallScalarMethod(connection,7,&bag,1,&flags,&count)==KERN_SUCCESS&&count==1)
                    printf("raw_keybag_flags=%#llx\n",(unsigned long long)flags);
                IOServiceClose(connection);
            }IOObjectRelease(service);
        }
        int display=-1;uint64_t awake=0;
        if(notify_register_check("com.apple.iokit.hid.displayStatus",&display)==NOTIFY_STATUS_OK){
            if(notify_get_state(display,&awake)==NOTIFY_STATUS_OK)printf("display_awake=%llu\n",(unsigned long long)awake);
            notify_cancel(display);
        }
        int token=-1;uint64_t value=0;
        if(notify_register_check(RW_AOD_STATUS,&token)==NOTIFY_STATUS_OK){notify_get_state(token,&value);notify_cancel(token);}
        const char *names[]={"not-loaded","ready","unsupported-native-method","appearance-requested","normal-appearance-requested","declined-lockscreen-state","exception","native-service-took-over"};
        printf("preview_status=%s\n",value<sizeof(names)/sizeof(names[0])?names[value]:"unknown");
        token=-1;value=0;
        if(notify_register_check(RW_AOD_SERVICE_STATUS,&token)==NOTIFY_STATUS_OK){notify_get_state(token,&value);notify_cancel(token);}
        printf("native_service_ready=%u configured_enabled=%u native_policy_enabled=%u requested_state=%u os_support=%u os_native_support=%u os_observed=%u\n",
            (unsigned)(value&1),(unsigned)((value>>1)&1),(unsigned)((value>>2)&1),(unsigned)((value>>8)&15),(unsigned)((value>>4)&1),(unsigned)((value>>5)&1),(unsigned)((value>>6)&1));
        printf("requested_observed=%u completed_observed=%u completed_state=%u idle_suppressed=%u\n",
            !!(value&RW_AOD_REQUEST_VALID),!!(value&RW_AOD_COMPLETED_VALID),(unsigned)((value>>12)&15),rw_aod_idle_suppressed(value));
        printf("saved_enabled=%u saved_wallpaper=%u\n",rw_aod_value(rw_aod_config(),@"Enabled")?1:0,rw_aod_value(rw_aod_config(),@"ShowWallpaper")?1:0);
        return state?0:1;
    }
    if(getuid()!=0||argc!=2||(strcmp(argv[1],"begin")&&strcmp(argv[1],"end"))){fprintf(stderr,"usage (root): aod-preview begin|end|status|enable|disable|wallpaper-on|wallpaper-off\n");return 1;}
    if(!strcmp(argv[1],"begin")&&rw_aod_value(rw_aod_config(),@"Enabled")){fprintf(stderr,"Disable native AOD before using the manual appearance preview.\n");return 1;}
    umask(0077);
    if(mkdir(RW_AOD_DIRECTORY,0755)&&errno!=EEXIST)return 1;
    struct stat st;if(lstat(RW_AOD_DIRECTORY,&st)||!S_ISDIR(st.st_mode)||st.st_uid||(st.st_mode&0022))return 1;
    if(chmod(RW_AOD_DIRECTORY,0755))return 1;
    int parent=open(RW_AOD_DIRECTORY,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK);if(parent<0)return 1;
    if(!strcmp(argv[1],"end")){unlinkat(parent,"arm",0);close(parent);return notify_post(RW_AOD_END)==NOTIFY_STATUS_OK?0:1;}
    int fd=openat(parent,"arm",O_WRONLY|O_CREAT|O_NOFOLLOW|O_NONBLOCK,0644);close(parent);if(fd<0)return 1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid||st.st_nlink!=1||fchmod(fd,0644)||ftruncate(fd,0)){close(fd);return 1;}
    struct rw_aod_token token={.magic=RW_AOD_MAGIC,.expires=NSProcessInfo.processInfo.systemUptime+30};
    do {arc4random_buf(&token.nonce,sizeof(token.nonce));}while(!token.nonce);
    BOOL ok=write(fd,&token,sizeof(token))==sizeof(token)&&!fsync(fd);close(fd);
    if(!ok)return 1;
    if(notify_post(RW_AOD_BEGIN)!=NOTIFY_STATUS_OK)return 1;
    puts("Requested one 12-second native appearance preview; this does not confirm it rendered. Touch or 'end' restores normal appearance.");
}return 0;}

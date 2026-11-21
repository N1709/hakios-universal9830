#ifndef REDWOOD_ALWAYS_ON_CONFIG_H
#define REDWOOD_ALWAYS_ON_CONFIG_H
#import <Foundation/Foundation.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <notify.h>
#ifndef RW_AOD_CONFIG
#define RW_AOD_CONFIG "/var/mobile/Library/Preferences/com.redwood.alwayson.plist"
#endif
#define RW_AOD_CHANGED "com.redwood.alwayson.changed"
#include "state.h"
static inline BOOL rw_aod_target(void){
    char args[2048]={0};size_t n=sizeof(args)-1;
    if(sysctlbyname("kern.bootargs",args,&n,NULL,0))return NO;
    for(char *p=args;(p=strstr(p,"cam_s20=1"));p++)if((p==args||p[-1]==' ')&&(p[9]==0||p[9]==' '))return YES;
    return NO;
}
static inline NSDictionary *rw_aod_config(void){
    int fd=open(RW_AOD_CONFIG,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return @{};
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||(st.st_uid!=501&&st.st_uid!=0)||
        (st.st_mode&0022)||st.st_size<=0||st.st_size>4096){close(fd);return @{};}
    NSMutableData *data=[NSMutableData dataWithLength:(NSUInteger)st.st_size];
    ssize_t n=read(fd,data.mutableBytes,data.length);close(fd);if(n!=(ssize_t)data.length)return @{};
    id obj=[NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:NULL];
    return [obj isKindOfClass:NSDictionary.class]?obj:@{};
}
/* Defaults (user, 2026-11-15): Always-On Display on; wallpaper and notifications in AOD off.
 * A missing, malformed or unsafe preference file reads as the defaults. */
static inline BOOL rw_aod_default(NSString *key){return [key isEqualToString:@"Enabled"];}
static inline BOOL rw_aod_value(NSDictionary *d,NSString *key){
    id v=d[key];if([v isKindOfClass:NSNumber.class])return [v boolValue];
    return rw_aod_default(key);
}
static inline BOOL rw_aod_set(NSString *key,BOOL value){
    if(![@[@"Enabled",@"ShowWallpaper",@"ShowNotifications"] containsObject:key])return NO;
    NSDictionary *old=rw_aod_config();NSMutableDictionary *next=[NSMutableDictionary dictionary];
    for(NSString *k in @[@"Enabled",@"ShowWallpaper",@"ShowNotifications"])next[k]=@(rw_aod_value(old,k));
    next[key]=@(value);
    NSData *data=[NSPropertyListSerialization dataWithPropertyList:next format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    int dir=open("/var/mobile/Library/Preferences",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK);if(dir<0)return NO;
    struct stat st;if(fstat(dir,&st)||st.st_uid!=501||!S_ISDIR(st.st_mode)){close(dir);return NO;}
    char name[96];snprintf(name,sizeof(name),"com.redwood.alwayson.%u.tmp",arc4random());
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0){close(dir);return NO;}
    BOOL ok=data&&write(fd,data.bytes,data.length)==(ssize_t)data.length;
    if(geteuid()==0&&fchown(fd,501,501))ok=NO;
    if(fsync(fd))ok=NO;close(fd);
    if(ok)ok=renameat(dir,name,dir,"com.redwood.alwayson.plist")==0;
    if(!ok)unlinkat(dir,name,0);close(dir);
    if(ok)notify_post(RW_AOD_CHANGED);return ok;
}
#endif

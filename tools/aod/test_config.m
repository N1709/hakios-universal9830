/* Host regression: defaults (AOD on, wallpaper/notifications off) and rejection of unsafe/malformed config files. */
#import <Foundation/Foundation.h>
#include <assert.h>
static char configPath[1024];
#define RW_AOD_CONFIG configPath
#include "config.h"
static void save(id plist){
    NSData *d=[NSPropertyListSerialization dataWithPropertyList:plist format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    assert([d writeToFile:@(configPath) atomically:YES]);assert(!chmod(configPath,0600));
}
int main(void){@autoreleasepool{
    char dir[]="/tmp/redwood-aod-test.XXXXXX";assert(mkdtemp(dir));
    snprintf(configPath,sizeof(configPath),"%s/config.plist",dir);
    assert(rw_aod_value(rw_aod_config(),@"Enabled"));
    assert(!rw_aod_value(rw_aod_config(),@"ShowWallpaper"));
    assert(!rw_aod_value(rw_aod_config(),@"ShowNotifications"));
    save(@{@"Enabled":@NO});assert(!rw_aod_value(rw_aod_config(),@"Enabled"));
    save(@{@"Enabled":@YES});assert(rw_aod_value(rw_aod_config(),@"Enabled"));
    assert(!rw_aod_value(rw_aod_config(),@"ShowWallpaper"));
    save(@{@"ShowWallpaper":@YES});assert(rw_aod_value(rw_aod_config(),@"ShowWallpaper"));
    assert(!chmod(configPath,0666));assert(!rw_aod_value(rw_aod_config(),@"ShowWallpaper"));assert(!chmod(configPath,0600));
    assert(!chmod(configPath,0666));assert(rw_aod_value(rw_aod_config(),@"Enabled"));
    save(@{@"Enabled":@"yes"});assert(rw_aod_value(rw_aod_config(),@"Enabled"));
    save(@[@YES]);assert(rw_aod_value(rw_aod_config(),@"Enabled"));
    assert([@"broken plist" writeToFile:@(configPath) atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
    assert(rw_aod_value(rw_aod_config(),@"Enabled"));
    assert(!unlink(configPath));assert(!symlink("/dev/null",configPath));
    assert(rw_aod_value(rw_aod_config(),@"Enabled"));assert(!unlink(configPath));
    assert(!mkfifo(configPath,0600));assert(rw_aod_value(rw_aod_config(),@"Enabled"));
    assert(!unlink(configPath));assert(!rmdir(dir));puts("AOD config regression passed");
}return 0;}

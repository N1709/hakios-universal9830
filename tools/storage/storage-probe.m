/* Local Files diagnostics; no user files are created, removed or moved. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/mount.h>
#include <unistd.h>

static NSString *const provider = @"/System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex";

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "register")) {
            if (access([[provider stringByAppendingPathComponent:@"LocalStorageFileProvider"] fileSystemRepresentation], X_OK)) {
                fprintf(stderr, "Local provider executable is missing\n");
                return 1;
            }
            dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
            Class workspace = NSClassFromString(@"LSApplicationWorkspace");
            SEL instance = sel_registerName("defaultWorkspace"), registration = sel_registerName("registerPlugin:");
            if (!workspace || ![workspace respondsToSelector:instance]) return 2;
            id ws = ((id (*)(id, SEL))objc_msgSend)(workspace, instance);
            if (![ws respondsToSelector:registration]) return 2;
            BOOL ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(ws, registration, [NSURL fileURLWithPath:provider]);
            printf("LaunchServices registration: %s\n", ok ? "accepted" : "rejected");
            return ok ? 0 : 1;
        }
        if (argc != 1) {
            fprintf(stderr, "usage: storage-probe [register]\n");
            return 2;
        }
        for (NSString *path in @[@"/", @"/private/var", @"/var/mobile/Documents", provider]) {
            NSError *error = nil;
            NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfFileSystemForPath:path error:&error];
            struct statfs fs;
            BOOL mounted = statfs(path.fileSystemRepresentation, &fs) == 0;
            printf("%s: fs=%s size=%llu free=%llu error=%s\n", path.UTF8String,
                mounted ? fs.f_fstypename : "unavailable",
                [attrs[NSFileSystemSize] unsignedLongLongValue],
                [attrs[NSFileSystemFreeSize] unsignedLongLongValue],
                error ? error.description.UTF8String : "none");
            for (NSString *key in @[NSURLVolumeTotalCapacityKey, NSURLVolumeAvailableCapacityKey,
                                    NSURLVolumeAvailableCapacityForImportantUsageKey,
                                    NSURLVolumeAvailableCapacityForOpportunisticUsageKey]) {
                id value = nil; NSError *resourceError = nil;
                BOOL ok = [[NSURL fileURLWithPath:path] getResourceValue:&value forKey:key error:&resourceError];
                printf("  %s: %s error=%s\n", key.UTF8String,
                    ok ? [value description].UTF8String : "unavailable",
                    resourceError ? resourceError.description.UTF8String : "none");
            }
        }
        BOOL executable = [[NSFileManager defaultManager] isExecutableFileAtPath:
            [provider stringByAppendingPathComponent:@"LocalStorageFileProvider"]];
        printf("Local provider executable: %s\n", executable ? "present" : "MISSING");
        return 0;
    }
}

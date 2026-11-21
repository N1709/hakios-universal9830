/* Read-only by default. Optional native BackBoard brightness/proximity requests
 * exercise the same service path as Settings and UIKit on iOS 16.5. */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern CFTypeRef MGCopyAnswer(CFStringRef);

static void status(id client)
{
    io_service_t backing = IOServiceGetMatchingService(0, IOServiceNameMatching("RedwoodAmbientLightSensor"));
    printf("ALS registry backing: %u\n", backing);
    if (backing) {
        CFMutableDictionaryRef properties = NULL;
        if (!IORegistryEntryCreateCFProperties(backing, &properties, NULL, 0)) {
            printf("%s\n", [[(id)properties description] UTF8String]);
            CFRelease(properties);
        }
        IOObjectRelease(backing);
    }
    CFTypeRef als = MGCopyAnswer(CFSTR("ambient-light-sensor"));
    printf("ambient-light-sensor: %s\n", [[(id)als description] UTF8String]);
    if (als) CFRelease(als);
    for (NSString *key in @[@"DisplayBrightnessAuto", @"DisplayOn", @"DisplayBrightness"]) {
        id value = ((id (*)(id, SEL, id))objc_msgSend)(client, sel_registerName("copyPropertyForKey:"), key);
        printf("%s: %s\n", key.UTF8String, [[value description] UTF8String]);
        [value release];
    }
    char reply[1024] = {0}; size_t size = sizeof(reply);
    char command[] = "shub.prox";
    if (!sysctlbyname("kern.redwood.poke", reply, &size, command, strlen(command))) puts(reply);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 1 && !(argc == 3 && !strcmp(argv[1], "--auto") &&
            (!strcmp(argv[2], "0") || !strcmp(argv[2], "1"))) &&
            !(argc == 3 && !strcmp(argv[1], "--proximity") && atoi(argv[2]) >= 1 && atoi(argv[2]) <= 30)) {
            fprintf(stderr, "usage: display-sensors [--auto 0|1 | --proximity SECONDS(1..30)]\n");
            return 2;
        }
        dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices", RTLD_NOW);
        dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness", RTLD_NOW);
        id client = [[NSClassFromString(@"BrightnessSystemClient") alloc] init];
        if (!client) return 1;
        if (argc == 3 && !strcmp(argv[1], "--auto")) {
            void (*set)(BOOL) = dlsym(RTLD_DEFAULT, "BKSDisplayBrightnessSetAutoBrightnessEnabled");
            if (!set) return 1;
            set(atoi(argv[2]) != 0);
            sleep(1);
        }
        if (argc == 3 && !strcmp(argv[1], "--proximity")) {
            void (*request)(int) = dlsym(RTLD_DEFAULT, "BKSHIDServicesRequestProximityDetectionMode");
            NSString *(*describe)(int) = dlsym(RTLD_DEFAULT, "NSStringFromBKSHIDServicesProximityDetectionMode");
            if (!request || !describe) return 1;
            for (int mode = 0; mode <= 6; ++mode) printf("native mode %d: %s\n", mode, describe(mode).UTF8String);
            request(3); /* native "active", translated by backboardd to DetectionMode 2 */
            for (int i = 0; i < atoi(argv[2]); ++i) { sleep(1); status(client); }
            request(0);
            sleep(1);
        }
        status(client);
        [client release];
    }
    return 0;
}

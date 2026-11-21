/* Objective-C interface metadata only. No authentication or provisioning calls. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
static void methods(FILE *output, Class cls, const char *name, char kind, BOOL full) {
    unsigned count = 0;
    Method *list = class_copyMethodList(cls, &count);
    for (unsigned i = 0; i < count; ++i) {
        const char *selector=sel_getName(method_getName(list[i]));
        if (!full && !strcasestr(selector,"clientprovided") && !strcasestr(selector,"clientdata") &&
            !strcasestr(selector,"anisette") && !strcasestr(selector,"clientinfo") && !strcasestr(selector,"grandslam")) continue;
        fprintf(output,"%c[%s %s] %s\n", kind, name,selector,method_getTypeEncoding(list[i]));
    }
    free(list);
}
static int dump(FILE *output) { @autoreleasepool {
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit", RTLD_NOW);
#if TARGET_OS_OSX
    dlopen("/System/Library/PrivateFrameworks/AOSKit.framework/AOSKit", RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount", RTLD_NOW);
#endif
    int count = objc_getClassList(NULL, 0);
    if (count <= 0) return 1;
    Class *classes = (Class *)calloc((size_t)count, sizeof(Class));
    if (!classes) return 1;
    int found = objc_getClassList(classes, count);
    for (int i = 0; i < found && i < count; ++i) {
        const char *name = class_getName(classes[i]);
        BOOL full=strstr(name,"Anisette") || strstr(name,"SRP") || !strcmp(name,"AOSUtilities") || !strcmp(name,"AKAppleIDSession") ||
            !strcmp(name,"AAAbsintheSigner") || !strcmp(name,"AASigningSession") || !strcmp(name,"AAAbsintheContext");
        full=full || !strcmp(name,"AKAppleIDAuthenticationContext") || !strcmp(name,"AKAppleIDAuthenticationController");
        full=full || !strcmp(name,"AADeviceProvisioningSession") || !strcmp(name,"AADeviceProvisioningInfo") || !strcmp(name,"AKDevice") || !strcmp(name,"AADeviceInfo");
        if(!full && strncmp(name,"AK",2))continue;
        methods(output,classes[i], name, '-',full);
        methods(output,object_getClass(classes[i]), name, '+',full);
        if (!strcmp(name, "AKAnisetteData")) {
            unsigned n = 0;
            objc_property_t *properties = class_copyPropertyList(classes[i], &n);
            for (unsigned j = 0; j < n; ++j)
                fprintf(output,"property %s.%s %s\n", name, property_getName(properties[j]), property_getAttributes(properties[j]));
            free(properties);
        }
    }
    free(classes);
    return 0;
}}
#ifdef AUTH_METADATA_DYLIB
__attribute__((constructor)) static void start(void) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        int fd=open("/var/mobile/Library/RedwoodAppleAccountLab/auth-methods.txt",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
        if(fd<0)return;FILE *output=fdopen(fd,"w");if(!output){close(fd);return;}
        dump(output);fclose(output);
    });
}
#else
int main(void){return dump(stdout);}
#endif

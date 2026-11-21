/* Read-only iOS capability probe. Never evaluates a policy, starts a match,
 * enrolls, changes keybags or requests a credential. */
#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <stdio.h>

static BOOL service(const char *name) {
    io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching(name));
    if(!s)return NO;
    IOObjectRelease(s);return YES;
}
int main(void) {
    @autoreleasepool {
        LAContext *c=[LAContext new];
        NSError *bioError=nil,*deviceError=nil;
        BOOL bio=[c canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics error:&bioError];
        BOOL device=[c canEvaluatePolicy:LAPolicyDeviceOwnerAuthentication error:&deviceError];
        void *m=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_NOW);
        int (*state)(CFDictionaryRef)=m?dlsym(m,"MKBGetDeviceLockState"):NULL;
        int bag=state?state(NULL):-1;
        NSMutableDictionary *r=[@{
            @"probe":@"read-only capabilities; no match/enrollment/authentication attempted",
            @"local_auth_biometrics_available":@(bio),
            @"local_auth_biometry_type":@(c.biometryType),
            @"local_auth_biometrics_error":@(bioError?bioError.code:0),
            @"local_auth_device_owner_available":@(device),
            @"local_auth_device_owner_error":@(deviceError?deviceError.code:0),
            @"native_keybag_state":@(bag),
            @"apple_mesa_service":@(service("AppleMesa")),
            @"apple_pearl_sep_service":@(service("ApplePearlSEPDriver")),
            @"apple_keystore_service":@(service("AppleKeyStore"))
        } mutableCopy];
        CFTypeRef present=NULL;
        io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleKeyStore"));
        if(s){present=IORegistryEntryCreateCFProperty(s,CFSTR("RedwoodSoftwarePasscode"),NULL,0);IOObjectRelease(s);}
        r[@"software_passcode_backend"] = @(present && CFGetTypeID(present)==CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)present));
        if(present)CFRelease(present);
        NSData *json=[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil];
        if(!json)return 1;
        fwrite(json.bytes,1,json.length,stdout);putchar('\n');[c invalidate];
    }
    return 0;
}

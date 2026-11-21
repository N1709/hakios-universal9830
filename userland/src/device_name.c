/* Inspect/apply the project's default name through configd, preserving network settings.
 * Usage: device_name [--set-default]
 * Fresh images seed the same preferences only when no preferences file exists.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static void print_value(const char *key, CFTypeRef value)
{
    char text[256] = "(unset)";
    if (value && CFGetTypeID(value) == CFStringGetTypeID())
        CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8);
    printf("%s: %s\n", key, text);
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--set-default"))) {
        fprintf(stderr, "usage: device_name [--set-default]\n");
        return 2;
    }
    SCPreferencesRef (*create)(CFAllocatorRef, CFStringRef, CFStringRef) = dlsym(RTLD_DEFAULT, "SCPreferencesCreate");
    Boolean (*lock)(SCPreferencesRef, Boolean) = dlsym(RTLD_DEFAULT, "SCPreferencesLock");
    Boolean (*commit)(SCPreferencesRef) = dlsym(RTLD_DEFAULT, "SCPreferencesCommitChanges");
    Boolean (*apply)(SCPreferencesRef) = dlsym(RTLD_DEFAULT, "SCPreferencesApplyChanges");
    Boolean (*unlock)(SCPreferencesRef) = dlsym(RTLD_DEFAULT, "SCPreferencesUnlock");
    CFPropertyListRef (*path_value)(SCPreferencesRef, CFStringRef) = dlsym(RTLD_DEFAULT, "SCPreferencesPathGetValue");
    if (!create || !lock || !commit || !apply || !unlock || !path_value) {
        fprintf(stderr, "SystemConfiguration name APIs unavailable\n"); return 1;
    }
    SCPreferencesRef preferences = create(NULL, CFSTR("Jesvi IOS device name"), NULL);
    if (!preferences) { fprintf(stderr, "SCPreferencesCreate: %s\n", SCErrorString(SCError())); return 1; }
    if (argc == 2) {
        Boolean (*computer)(SCPreferencesRef, CFStringRef, CFStringEncoding) =
            dlsym(RTLD_DEFAULT, "SCPreferencesSetComputerName");
        Boolean (*local)(SCPreferencesRef, CFStringRef) = dlsym(RTLD_DEFAULT, "SCPreferencesSetLocalHostName");
        if (!computer || !local || !lock(preferences, TRUE)) {
            fprintf(stderr, "Name APIs unavailable or preferences lock failed: %s\n", SCErrorString(SCError()));
            CFRelease(preferences); return 1;
        }
        Boolean saved = computer(preferences, CFSTR("Jesvi IOS"), kCFStringEncodingUTF8) &&
            local(preferences, CFSTR("Jesvi-IOS")) && commit(preferences);
        Boolean applied = saved && apply(preferences);
        unlock(preferences);
        if (!saved || !applied) {
            fprintf(stderr, "Name saved=%d applied=%d: %s\n", saved, applied, SCErrorString(SCError()));
            CFRelease(preferences); return 1;
        }
    }
    CFDictionaryRef computer = path_value(preferences, CFSTR("/System/System"));
    CFDictionaryRef local = path_value(preferences, CFSTR("/System/Network/HostNames"));
    print_value("ComputerName", computer ? CFDictionaryGetValue(computer, CFSTR("ComputerName")) : NULL);
    print_value("LocalHostName", local ? CFDictionaryGetValue(local, CFSTR("LocalHostName")) : NULL);
    CFRelease(preferences);
    return 0;
}

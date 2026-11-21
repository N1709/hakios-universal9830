/* Register the local Files host as a containerized user app, so Finder's
 * user-app sharing query includes it. Keep its existing identifier and data.
 * Copy the immutable system bundle once; never replace user documents. */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
@interface MCMAppContainer : NSObject
+ (id)containerWithIdentifier:(NSString *)identifier createIfNecessary:(BOOL)create
                     existed:(BOOL *)existed error:(NSError **)error;
- (NSURL *)url;
@end
@interface LSApplicationWorkspace : NSObject
+ (id)defaultWorkspace;
- (BOOL)registerApplication:(NSURL *)url;
@end
@interface MCMContainer : NSObject
+ (id)containerWithIdentifier:(NSString *)identifier createIfNecessary:(BOOL)create
    existed:(BOOL *)existed error:(NSError **)error;
- (NSURL *)url;
@end
@interface LSApplicationWorkspace (Record)
- (BOOL)registerApplicationDictionary:(NSDictionary *)record;
@end
extern int SecStaticCodeCreateWithPathAndAttributes(CFURLRef, uint32_t, CFDictionaryRef, void **);
extern int SecCodeCopySigningInformation(void *, uint32_t, CFDictionaryRef *);

static NSString *mcm_path(NSString *cls, NSString *identifier) {
    NSError *error = nil; BOOL existed = NO;
    return [[[NSClassFromString(cls) containerWithIdentifier:identifier createIfNecessary:YES
        existed:&existed error:&error] url] path];
}

static NSDictionary *entitlements(NSString *executable) {
    void *code = NULL; CFDictionaryRef info = NULL; NSDictionary *found = nil;
    if (SecStaticCodeCreateWithPathAndAttributes((__bridge CFURLRef)[NSURL fileURLWithPath:executable], 0, NULL, &code) == 0 && code) {
        if (SecCodeCopySigningInformation(code, 1u << 2, &info) == 0 && info) {
            found = [(__bridge NSDictionary *)info objectForKey:@"entitlements-dict"];
            CFRelease(info);
        }
        CFRelease(code);
    }
    return found ?: @{};
}

/* The record of a bundle for LaunchServices: an app (owner nil) or one of its extensions. */
static NSDictionary *record(NSString *path, NSString *owner) {
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
    NSString *identifier = info[@"CFBundleIdentifier"], *executable = info[@"CFBundleExecutable"];
    if (!identifier || !executable) return nil;
    NSDictionary *rights = entitlements([path stringByAppendingPathComponent:executable]);
    NSMutableDictionary *r = [@{
        /* (A, 2026-11-20) the host as a System app: SpringBoard ignored SBAppTags=hidden on this User app and kept
         * a dead "Jesvi Files" icon on the home screen (iOS honours the tag for system apps). Cost: Finder's
         * File Sharing lists user apps only. */
        @"ApplicationType": owner ? @"PluginKitPlugin" : @"System",
        @"CFBundleIdentifier": identifier, @"CodeInfoIdentifier": identifier, @"CompatibilityState": @0,
        @"Path": path, @"Entitlements": rights, @"IsDeletable": @NO,
        @"SignerOrganization": @"Apple Inc.", @"SignatureVersion": @132352,
        @"SignerIdentity": @"Apple iPhone OS Application Signing", @"IsAdHocSigned": @YES,
        @"LSInstallType": @1, @"HasMIDBasedSINF": @0, @"MissingSINF": @0, @"FamilyID": @0,
        @"IsOnDemandInstallCapable": @0 } mutableCopy];
    NSString *home = mcm_path(owner ? @"MCMPluginKitPluginDataContainer" : @"MCMAppDataContainer", identifier);
    if (home) {
        r[@"Container"] = home; r[@"IsContainerized"] = @YES;
        r[@"EnvironmentVariables"] = @{ @"CFFIXED_USER_HOME": home, @"HOME": home,
            @"TMPDIR": [home stringByAppendingPathComponent:@"tmp"] };
    }
    NSMutableDictionary *groups = [NSMutableDictionary dictionary];
    id names = rights[@"com.apple.security.application-groups"];
    if ([names isKindOfClass:NSArray.class]) for (NSString *name in names) {
        NSString *shared = mcm_path(@"MCMSharedDataContainer", name);
        if (shared) groups[name] = shared;
    }
    r[@"GroupContainers"] = groups;
    /* (SpringBoard takes the tags from the record, not from Info.plist: without them the hidden
     * host gets an icon on the home screen) */
    if (info[@"SBAppTags"]) r[@"SBAppTags"] = info[@"SBAppTags"];
    if (owner) { r[@"PluginOwnerBundleID"] = owner; return r; }
    NSMutableDictionary *plugins = [NSMutableDictionary dictionary];
    NSString *dir = [path stringByAppendingPathComponent:@"PlugIns"];
    for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil] ?: @[]) {
        if (![name.pathExtension isEqual:@"appex"]) continue;
        NSDictionary *plugin = record([dir stringByAppendingPathComponent:name], identifier);
        if (plugin) plugins[plugin[@"CFBundleIdentifier"]] = plugin;
    }
    r[@"_LSBundlePlugins"] = plugins;
    return r;
}

/* Finder's file sharing shows one folder: the app's Documents. Make that folder the phone's own
 * storage, "On My iPhone" (the local provider's File Provider Storage), so a Mac sees Downloads and
 * the rest and not an empty folder of this hidden app. afcd follows the link (checked 2026-11-13).
 * What was in a real Documents folder is moved into the storage first; nothing is deleted. */
static void share_storage(NSString *home) {
    NSString *group = mcm_path(@"MCMSharedDataContainer", @"group.com.apple.FileProvider.LocalStorage");
    if (!home || !group) return;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *storage = [group stringByAppendingPathComponent:@"File Provider Storage"];
    NSString *documents = [home stringByAppendingPathComponent:@"Documents"];
    if (![fm fileExistsAtPath:storage]) {       /* (the provider makes it at its first start) */
        if (mkdir(storage.fileSystemRepresentation, 0755)) return;
        chown(storage.fileSystemRepresentation, 501, 501);
    }
    NSDictionary *is = [fm attributesOfItemAtPath:documents error:nil];
    if ([is.fileType isEqual:NSFileTypeSymbolicLink]) {
        if ([[fm destinationOfSymbolicLinkAtPath:documents error:nil] isEqual:storage]) return;
        unlink(documents.fileSystemRepresentation);
    } else if (is) {
        for (NSString *name in [fm contentsOfDirectoryAtPath:documents error:nil] ?: @[])
            [fm moveItemAtPath:[documents stringByAppendingPathComponent:name]
                        toPath:[storage stringByAppendingPathComponent:name] error:nil];
        if (rmdir(documents.fileSystemRepresentation)) return;      /* a name was taken: left as it is */
    }
    if (symlink(storage.fileSystemRepresentation, documents.fileSystemRepresentation) == 0)
        lchown(documents.fileSystemRepresentation, 501, 501);
    NSLog(@"Jesvi Files: Finder shares %@", storage);
}

/* A standalone registerPlugin: makes a proxy without entitlements or container
 * metadata on this port. FileProvider then omits the local domain. Build the
 * registration host from the immutable image automatically on a clean volume;
 * SBAppTags is supplied in both Info.plist and the LS record, so it has no icon.
 * Apple's framework bundles stay in place. No user documents are copied here. */
static NSArray *provider_names(void) {
    return @[@"LocalStorageFileProvider", @"SaveToFiles", @"com.apple.DocumentManager.Service"];
}
static BOOL host_complete(NSString *path) {
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm isExecutableFileAtPath:[path stringByAppendingPathComponent:@"LocalFilesHost"]]) return NO;
    for (NSString *name in provider_names()) {
        NSString *plugin = [[path stringByAppendingPathComponent:@"PlugIns"]
            stringByAppendingPathComponent:[name stringByAppendingString:@".appex"]];
        if (![fm isExecutableFileAtPath:[plugin stringByAppendingPathComponent:name]] ||
            ![NSDictionary dictionaryWithContentsOfFile:[plugin stringByAppendingPathComponent:@"Info.plist"]][@"CFBundleIdentifier"]) return NO;
    }
    return YES;
}
static BOOL prepare_hidden_host(void) {
    NSString *destination = @"/var/mobile/Library/HakiosLocalFiles/LocalFilesHost.app";
    NSFileManager *fm = NSFileManager.defaultManager;
    if (host_complete(destination)) return YES;
    if ([fm fileExistsAtPath:destination]) {
        NSString *saved = [destination stringByAppendingFormat:@".incomplete-%@", NSUUID.UUID.UUIDString];
        if (![fm moveItemAtPath:destination toPath:saved error:nil]) return NO;
    }
    if (access("/redwood/bin/LocalFilesHost", X_OK)) {
        NSLog(@"Files: image is missing LocalFilesHost; cannot prepare provider metadata");
        return NO;
    }
    NSArray *paths = @[
        @"/System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex",
        @"/System/Library/PrivateFrameworks/DocumentManagerUICore.framework/PlugIns/SaveToFiles.appex",
        @"/System/Library/PrivateFrameworks/DocumentManagerUICore.framework/PlugIns/com.apple.DocumentManager.Service.appex" ];
    NSString *parent = destination.stringByDeletingLastPathComponent;
    NSString *staging = [parent stringByAppendingPathComponent:[@".prepare-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    NSError *error = nil;
    BOOL ok = [fm createDirectoryAtPath:[staging stringByAppendingPathComponent:@"PlugIns"]
        withIntermediateDirectories:YES attributes:nil error:&error];
    if (ok) ok = [fm copyItemAtPath:@"/redwood/bin/LocalFilesHost"
        toPath:[staging stringByAppendingPathComponent:@"LocalFilesHost"] error:&error];
    for (NSString *path in paths) {
        if (!ok) break;
        ok = [fm copyItemAtPath:path toPath:[[staging stringByAppendingPathComponent:@"PlugIns"]
            stringByAppendingPathComponent:path.lastPathComponent] error:&error];
    }
    NSDictionary *info = @{ @"CFBundleIdentifier": @"org.hakios.LocalFilesHost",
        @"CFBundleExecutable": @"LocalFilesHost", @"CFBundleName": @"Local Storage",
        @"CFBundleDisplayName": @"Local Storage", @"CFBundlePackageType": @"APPL",
        @"CFBundleVersion": @"1", @"CFBundleShortVersionString": @"1.0",
        @"LSRequiresIPhoneOS": @YES, @"MinimumOSVersion": @"16.0", @"UIDeviceFamily": @[@1, @2],
        @"UIRequiredDeviceCapabilities": @[@"arm64"], @"SBAppTags": @[@"hidden"],
        @"UIFileSharingEnabled": @YES };
    if (ok) ok = [info writeToFile:[staging stringByAppendingPathComponent:@"Info.plist"] atomically:YES];
    if (ok) ok = [fm moveItemAtPath:staging toPath:destination error:&error];
    if (!ok) {
        [fm removeItemAtPath:staging error:nil];
        NSLog(@"Files: preparing hidden provider host failed: %@", error);
    }
    return ok;
}

static BOOL plugin_ready(Class proxy, NSString *identifier) {
    id p = [proxy performSelector:@selector(pluginKitProxyForIdentifier:) withObject:identifier];
    if (!p) return NO;
    NSURL *data = [p respondsToSelector:@selector(dataContainerURL)] ?
        [p performSelector:@selector(dataContainerURL)] : nil;
    NSDictionary *groups = [p respondsToSelector:@selector(groupContainerURLs)] ?
        [p performSelector:@selector(groupContainerURLs)] : nil;
    NSDictionary *rights = [p respondsToSelector:@selector(entitlements)] ?
        [p performSelector:@selector(entitlements)] : nil;
    NSArray *names = rights[@"com.apple.security.application-groups"];
    return data != nil && (![identifier isEqual:@"com.apple.FileProvider.LocalStorage"] ||
        (groups[@"group.com.apple.FileProvider.LocalStorage"] != nil &&
         [names containsObject:@"group.com.apple.FileProvider.LocalStorage"]));
}

int main(void) {
    alarm(30);
    @autoreleasepool {
        NSString *identifier = @"org.hakios.LocalFilesHost";
        if (access("/Applications/HakiosLocalFiles.app/Info.plist", F_OK) != 0 &&
            !host_complete(@"/var/mobile/Library/HakiosLocalFiles/LocalFilesHost.app")) {
            if (!prepare_hidden_host()) return 1;
        }
        dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
        dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
        Class containers = NSClassFromString(@"MCMAppContainer");
        Class workspace = NSClassFromString(@"LSApplicationWorkspace");
        if (!containers || !workspace) return 1;
        NSError *error = nil; BOOL existed = NO;
        id container = [containers containerWithIdentifier:identifier createIfNecessary:YES existed:&existed error:&error];
        NSURL *url = [container url];
        if (!url) { NSLog(@"Finder host container unavailable: %@", error); return 1; }
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *destination = [url.path stringByAppendingPathComponent:@"LocalFilesHost.app"];
        if (!host_complete(destination)) {
            if ([fm fileExistsAtPath:destination]) {
                NSString *saved = [destination stringByAppendingFormat:@".incomplete-%@", NSUUID.UUID.UUIDString];
                if (![fm moveItemAtPath:destination toPath:saved error:&error]) return 1;
            }
            NSString *source = nil;
            for (NSString *candidate in @[@"/Applications/HakiosLocalFiles.app",
                 @"/var/mobile/Library/HakiosLocalFiles/LocalFilesHost.app"]) {
                NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[candidate stringByAppendingPathComponent:@"Info.plist"]];
                if ([info[@"CFBundleIdentifier"] isEqual:identifier]) { source = candidate; break; }
            }
            NSString *staging = [url.path stringByAppendingPathComponent:
                [@".jesvi-host-" stringByAppendingString:NSUUID.UUID.UUIDString]];
            BOOL copied = source && [fm copyItemAtPath:source toPath:staging error:&error];
            if (!copied || ![fm moveItemAtPath:staging toPath:destination error:&error]) {
                if (copied) [fm removeItemAtPath:staging error:nil];
                NSLog(@"Finder host copy unavailable: %@", error); return 1;
            }
        }
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[destination stringByAppendingPathComponent:@"Info.plist"]];
        if (![info[@"CFBundleIdentifier"] isEqual:identifier] || ![info[@"UIFileSharingEnabled"] boolValue]) return 1;
        /* Always supply the complete record. Besides plugin containers, this
         * keeps SBAppTags=hidden in LaunchServices even if an earlier plain
         * registerApplication: succeeded without importing those app tags. */
        BOOL ok = NO;
        {
            /* Discard only this port's incomplete standalone records. Otherwise
             * PlugInKit can elect them instead of the fully described plugins. */
            id ws = [workspace defaultWorkspace];
            for (NSString *name in [fm contentsOfDirectoryAtPath:[destination stringByAppendingPathComponent:@"PlugIns"] error:nil] ?: @[]) {
                NSString *path = [[destination stringByAppendingPathComponent:@"PlugIns"] stringByAppendingPathComponent:name];
                NSString *pid = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]][@"CFBundleIdentifier"];
                id proxy = [NSClassFromString(@"LSPlugInKitProxy") performSelector:@selector(pluginKitProxyForIdentifier:) withObject:pid];
                NSURL *old = [proxy respondsToSelector:@selector(bundleURL)] ? [proxy performSelector:@selector(bundleURL)] : nil;
                if (old && !plugin_ready(NSClassFromString(@"LSPlugInKitProxy"), pid) && [ws respondsToSelector:@selector(unregisterPlugin:)])
                    ((BOOL (*)(id, SEL, id))objc_msgSend)(ws, @selector(unregisterPlugin:), old);
            }
            NSDictionary *whole = record(destination, nil);
            ok = whole && [[workspace defaultWorkspace] registerApplicationDictionary:whole];
            NSLog(@"Jesvi Files record: data %@, %lu extensions", whole[@"Container"],
                (unsigned long)[whole[@"_LSBundlePlugins"] count]);
        }
        if (ok) {
            ok = host_complete(destination);
            for (NSString *name in provider_names()) {
                NSString *path = [[destination stringByAppendingPathComponent:@"PlugIns"]
                    stringByAppendingPathComponent:[name stringByAppendingString:@".appex"]];
                NSString *pid = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]][@"CFBundleIdentifier"];
                if (!pid || !plugin_ready(NSClassFromString(@"LSPlugInKitProxy"), pid)) ok = NO;
            }
        }
        NSLog(@"Files provider metadata: %@", ok ? @"complete (hidden host)" : @"retry needed");
        if (ok) share_storage(mcm_path(@"MCMAppDataContainer", identifier));
        return ok ? 0 : 1;
    }
}

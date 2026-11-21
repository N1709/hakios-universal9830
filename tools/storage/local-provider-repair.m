#import <Foundation/Foundation.h>
#include <sys/mount.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <objc/runtime.h>
@interface PKManager : NSObject
+ (instancetype)defaultManager;
- (void)registerPlugInAtURL:(NSURL *)url result:(void (^)(id))result;
@end
@interface FPProviderDomain : NSObject
+ (NSArray *)providerDomainsWithError:(NSError **)error;
- (void)setEnabled:(BOOL)enabled completionHandler:(void (^)(NSError *))completion;
@end
@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerPlugin:(NSURL *)url;
@end
int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    @autoreleasepool {
        NSString *path = @"/System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex";
        if (argc != 2) return 2;
        if (!strcmp(argv[1], "mount")) {
            if ([[NSFileManager defaultManager] isExecutableFileAtPath:[path stringByAppendingPathComponent:@"LocalStorageFileProvider"]]) {
                puts("Provider executable already present; mount skipped"); return 0;
            }
            if (mount("ramfs", path.fileSystemRepresentation, 0, NULL)) { perror("mount provider overlay"); return 1; }
            puts("Mounted writable provider overlay"); return 0;
        }
        if (!strcmp(argv[1], "enable")) {
            dlopen("/System/Library/Frameworks/FileProvider.framework/FileProvider", RTLD_NOW);
            Class cls = NSClassFromString(@"FPProviderDomain"); NSError *error = nil;
            NSArray *domains = [cls providerDomainsWithError:&error];
            if (!domains) { NSLog(@"domains: %@", error); return 1; }
            for (FPProviderDomain *domain in domains) {
                printf("domain: %s\n", domain.description.UTF8String);
                if ([domain.description containsString:@"com.apple.FileProvider.LocalStorage"]) {
                    __block BOOL finished = NO; __block NSError *failure = nil;
                    [domain setEnabled:YES completionHandler:^(NSError *err) { failure = err; finished = YES; }];
                    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:15];
                    while (!finished && end.timeIntervalSinceNow > 0) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
                    NSLog(@"enable finished=%d error=%@", finished, failure);
                    return finished && !failure ? 0 : 1;
                }
            }
            return 5;
        }
        if (!strcmp(argv[1], "inspect")) {
            dlopen("/System/Library/Frameworks/FileProvider.framework/FileProvider", RTLD_NOW);
            for (NSString *name in @[@"FPDaemonConnection", @"FPProviderDomain", @"NSFileProviderManager"]) {
                Class cls = NSClassFromString(name); printf("%s %p\n", name.UTF8String, (__bridge void *)cls);
                for (unsigned meta = 0; meta < 2; meta++) {
                    unsigned count = 0; Method *methods = class_copyMethodList(meta ? object_getClass(cls) : cls, &count);
                    for (unsigned i = 0; i < count; i++) if (strcasestr(sel_getName(method_getName(methods[i])), "domain") || strcasestr(sel_getName(method_getName(methods[i])), "enable") || meta) printf("%c %s\n", meta ? '+' : '-', sel_getName(method_getName(methods[i])));
                    free(methods);
                }
            }
            return 0;
        }
        if (!strcmp(argv[1], "register")) {
            path = @"/var/mobile/Library/LocalStorageRepair/LocalStorageFileProvider.appex";
            dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
            Class cls = NSClassFromString(@"LSApplicationWorkspace");
            printf("LSApplicationWorkspace=%p\n", (__bridge void *)cls);
            if (!cls) return 3;
            LSApplicationWorkspace *ws = [cls defaultWorkspace];
            dlopen("/System/Library/PrivateFrameworks/PlugInKit.framework/PlugInKit", RTLD_NOW);
            Class manager = NSClassFromString(@"PKManager");
            __block BOOL finished = NO;
            [[manager defaultManager] registerPlugInAtURL:[NSURL fileURLWithPath:path] result:^(id result) { NSLog(@"register plugin result: %@", result); finished = YES; }];
            NSDate *end = [NSDate dateWithTimeIntervalSinceNow:15];
            while (!finished && end.timeIntervalSinceNow > 0) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
            return finished ? 0 : 1;
        }
        return 2;
    }
}

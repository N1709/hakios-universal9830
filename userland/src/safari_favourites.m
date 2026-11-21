/* First-boot additions through iOS 16.5 WebBookmarks, never direct SQLite writes.
 * Selector signatures verified in the target 20F66 arm64 cache. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <unistd.h>

@protocol RWBookmark
- (int)identifier;
- (NSString *)address;
- (id)initWithTitle:(NSString *)title address:(NSString *)address parentID:(int)parent collectionType:(NSInteger)type;
@end
@protocol RWConfiguration
- (NSInteger)collectionType;
@end
@protocol RWCollection
- (id<RWBookmark>)favoritesFolder;
- (NSArray *)descendantsOfBookmarkFolder:(id)folder;
- (id<RWConfiguration>)configuration;
- (BOOL)saveBookmark:(id)bookmark;
@end
@interface NSObject (RWBookmarkFactory)
+ (id<RWCollection>)safariBookmarkCollection;
+ (BOOL)lockSync;
+ (void)unlockSync;
@end

static NSString *normalized(NSString *address) {
    if (![address isKindOfClass:NSString.class]) return nil;
    NSURLComponents *url = [NSURLComponents componentsWithString:address];
    if (!url.host.length) return nil;
    url.scheme = url.scheme.lowercaseString;
    url.host = url.host.lowercaseString;
    if (!url.path.length) url.path = @"/";
    return url.string;
}

static BOOL apply_favourites(Class collectionClass, Class bookmarkClass) {
    id<RWCollection> collection = [collectionClass safariBookmarkCollection];
    for (NSString *name in @[@"favoritesFolder", @"descendantsOfBookmarkFolder:", @"configuration", @"saveBookmark:"])
        if (![(id)collection respondsToSelector:NSSelectorFromString(name)]) return NO;
    id<RWBookmark> folder = [collection favoritesFolder];
    id<RWConfiguration> configuration = [collection configuration];
    if (![(id)folder respondsToSelector:@selector(identifier)] ||
        ![(id)configuration respondsToSelector:@selector(collectionType)]) return NO;
    int parent = [folder identifier];
    if (parent <= 0) return NO;
    NSArray *existing = [collection descendantsOfBookmarkFolder:folder];
    if (![existing isKindOfClass:NSArray.class]) return NO;
    NSMutableSet *addresses = [NSMutableSet set];
    for (id bookmark in existing) {
        if (![bookmark respondsToSelector:@selector(address)]) continue;
        NSString *address = normalized([bookmark address]);
        if (address) [addresses addObject:address];
    }
    NSArray *defaults = @[
        @[@"TrollStore", @"https://github.com/opa334/TrollStore"],
        @[@"AltStore", @"https://altstore.io/"],
        @[@"SideStore", @"https://sidestore.io/"],
        @[@"Jesvi iOS", @"https://github.com/jesvi-eulah/Jesvi-iOS"]
    ];
    unsigned added = 0;
    for (NSArray *entry in defaults) {
        NSString *address = normalized(entry[1]);
        if ([addresses containsObject:address]) continue;
        id<RWBookmark> bookmark = [[bookmarkClass alloc] initWithTitle:entry[0]
            address:entry[1] parentID:parent collectionType:[configuration collectionType]];
        if (!bookmark || ![collection saveBookmark:bookmark]) return NO;
        [addresses addObject:address];
        added++;
    }
    /* Re-query the native collection before recording completion. */
    existing = [collection descendantsOfBookmarkFolder:folder];
    if (![existing isKindOfClass:NSArray.class]) return NO;
    [addresses removeAllObjects];
    for (id bookmark in existing) {
        if (![bookmark respondsToSelector:@selector(address)]) continue;
        NSString *address = normalized([bookmark address]);
        if (address) [addresses addObject:address];
    }
    for (NSArray *entry in defaults)
        if (![addresses containsObject:normalized(entry[1])]) return NO;
    NSLog(@"Safari favourites: %u added; all four verified", added);
    return YES;
}

int main(void) {
    @autoreleasepool {
        /* launchd runs this as mobile; refuse root's Safari database. */
        if (getuid() != 501) { NSLog(@"Safari favourites: requires mobile"); return 1; }
        NSString *directory = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Preferences"];
        NSString *marker = [directory stringByAppendingPathComponent:@".redwood-safari-favourites-v1"];
        if ([NSFileManager.defaultManager fileExistsAtPath:marker]) return 0;
        if (!dlopen("/System/Library/PrivateFrameworks/WebBookmarks.framework/WebBookmarks", RTLD_NOW)) return 1;
        Class collection = NSClassFromString(@"WebBookmarkCollection");
        Class bookmark = NSClassFromString(@"WebBookmark");
        if (![collection respondsToSelector:@selector(safariBookmarkCollection)] ||
            ![collection respondsToSelector:@selector(lockSync)] ||
            ![collection respondsToSelector:@selector(unlockSync)] ||
            ![bookmark instancesRespondToSelector:@selector(initWithTitle:address:parentID:collectionType:)]) return 1;
        BOOL locked = NO, ok = NO;
        @try {
            locked = [collection lockSync];
            if (locked) ok = apply_favourites(collection, bookmark);
        } @catch (NSException *exception) {
            /* Do not dump bookmark contents or the user's account details. */
            NSLog(@"Safari favourites: native API exception (%@); retry later", exception.name);
        } @finally {
            if (locked) [collection unlockSync];
        }
        if (!ok) { NSLog(@"Safari favourites: not complete; retry later"); return 1; }
        if (![NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil]) return 1;
        return [@"1\n" writeToFile:marker atomically:YES encoding:NSUTF8StringEncoding error:nil] ? 0 : 1;
    }
}

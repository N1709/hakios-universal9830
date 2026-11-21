/* MIT. S20 Cellular Settings UI, independent of the radio driver.
 * Loaded only in Preferences by the port's existing tweak loader. Shows the live state that
 * redwood-cellulard publishes (/var/tmp/redwood-cell/state) and drives mobile data through its pipe.
 */
#include <sys/stat.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <notify.h>
#include <time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Small declarations for the runtime API; no extracted Apple headers. */
@interface PSSpecifier : NSObject
+ (instancetype)preferenceSpecifierNamed:(NSString *)name target:(id)target
    set:(SEL)set get:(SEL)get detail:(Class)detail cell:(NSInteger)cell edit:(Class)edit;
- (id)propertyForKey:(NSString *)key;
- (void)setProperty:(id)value forKey:(NSString *)key;
@end

@interface PSListController : UIViewController
- (NSArray *)specifiers;
- (void)reloadSpecifiers;
@end

static NSString *const RWCellularID = @"MOBILE_DATA_SETTINGS_ID";
static CFStringRef const RWDomain = CFSTR("com.redwood.cellular");
static CFStringRef const RWDataKey = CFSTR("CellularDataEnabled");

static void diagnostic(const char *event)
{
    FILE *file = fopen("/var/mobile/Library/Preferences/com.redwood.cellular-ui.log", "a");
    if (file) { fprintf(file, "[%d] %s\n", getpid(), event); fclose(file); }
}

/* PSListController's table helpers consult its backing array as well as the
 * getter. Resolve the ivar by name, never by a private-layout offset. */
static void store_specifiers(id controller, NSArray *items)
{
    Ivar ivar = class_getInstanceVariable(PSListController.class, "_specifiers");
    if (ivar && ivar_getTypeEncoding(ivar)[0] == '@') {
        object_setIvarWithStrongDefault(controller, ivar, items);
    }
}

@interface RWCellularSettingsController : PSListController
@end

/* Live state from redwood-cellulard (/var/tmp/redwood-cell/state, world-readable; written every 20 s and on
 * change). Fresh = updated within two minutes, as the status bar uses it. */
static NSDictionary *cell_state(BOOL *fresh)
{
    NSString *s = [NSString stringWithContentsOfFile:@"/var/tmp/redwood-cell/state" encoding:NSUTF8StringEncoding error:nil];
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    for (NSString *line in [s componentsSeparatedByString:@"\n"]) {
        NSRange r = [line rangeOfString:@" "];
        if (r.location != NSNotFound) d[[line substringToIndex:r.location]] = [line substringFromIndex:r.location + 1];
    }
    long updated = [d[@"updated"] longLongValue];
    if (fresh) *fresh = updated && time(NULL) - updated < 120;
    return d;
}

/* data on/off goes to the modem service's command pipe (mode 0622); never blocks if nobody reads it */
static BOOL cell_command(const char *cmd)
{
    int fd = open("/var/tmp/redwood-cell/cmd", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return NO;
    BOOL ok = write(fd, cmd, strlen(cmd)) == (ssize_t)strlen(cmd);
    close(fd);
    return ok;
}

static PSSpecifier *value_row(NSString *name, id target, SEL get)
{
    return [PSSpecifier preferenceSpecifierNamed:name target:target set:NULL get:get detail:Nil cell:4 edit:Nil];
}

static int ims_state(void);
static BOOL provider_alive(void);
static NSString *page_signature(NSDictionary *st)
{
    return [NSString stringWithFormat:@"%@|%d|%d", [st description], ims_state(), provider_alive()];
}

@implementation RWCellularSettingsController
{
    NSTimer *refresh;
    NSString *last;
}
- (NSArray *)specifiers
{
    static dispatch_once_t page_once;
    dispatch_once(&page_once, ^{ diagnostic("cellular-page-opened"); });
    BOOL fresh = NO;
    NSDictionary *st = cell_state(&fresh);
    last = page_signature(st);

    PSSpecifier *top = [PSSpecifier preferenceSpecifierNamed:nil target:nil set:NULL get:NULL detail:Nil cell:0 edit:Nil];
    [top setProperty:fresh ? @"Mobile data uses the S20's own modem (LTE). Turning it off keeps the signal but stops internet over cellular."
                           : @"The modem is starting or off. Settings here apply as soon as it is up."
              forKey:@"footerText"];
    PSSpecifier *data = [PSSpecifier preferenceSpecifierNamed:@"Cellular Data" target:self
        set:@selector(setCellularData:specifier:) get:@selector(cellularData:) detail:Nil cell:6 edit:Nil];
    [data setProperty:@"redwood.cellular.data" forKey:@"id"];

    PSSpecifier *netg = [PSSpecifier preferenceSpecifierNamed:@"NETWORK" target:nil set:NULL get:NULL detail:Nil cell:0 edit:Nil];
    PSSpecifier *simg = [PSSpecifier preferenceSpecifierNamed:@"SIM" target:nil set:NULL get:NULL detail:Nil cell:0 edit:Nil];
    PSSpecifier *svcg = [PSSpecifier preferenceSpecifierNamed:@"SERVICES" target:nil set:NULL get:NULL detail:Nil cell:0 edit:Nil];
    [svcg setProperty:@"Calls and text messages go over VoLTE (IMS), through the S20's own modem." forKey:@"footerText"];

    NSArray *items = @[top, data,
        netg, value_row(@"Network", self, @selector(network:)), value_row(@"Signal", self, @selector(signal:)),
              value_row(@"Technology", self, @selector(technology:)),
        simg, value_row(@"Carrier", self, @selector(carrier:)), value_row(@"SIM", self, @selector(sim:)),
              value_row(@"Network Code", self, @selector(plmn:)),
        svcg, value_row(@"VoLTE", self, @selector(volte:)), value_row(@"Calls", self, @selector(calls:)),
              value_row(@"Text Messages", self, @selector(sms:))];
    self.title = @"Cellular";
    store_specifiers(self, items);
    return items;
}
- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    [refresh invalidate];
    __weak typeof(self) weak = self;
    refresh = [NSTimer scheduledTimerWithTimeInterval:3 repeats:YES block:^(NSTimer *t) {
        (void)t;
        typeof(self) me = weak;
        if (me && ![page_signature(cell_state(NULL)) isEqualToString:me->last]) [me reloadSpecifiers];
    }];
}
- (void)viewWillDisappear:(BOOL)animated
{
    [super viewWillDisappear:animated];
    [refresh invalidate];
    refresh = nil;
}
- (NSNumber *)cellularData:(PSSpecifier *)specifier
{
    (void)specifier;
    NSString *on = cell_state(NULL)[@"data_on"];
    if (on) return @([on intValue] != 0);
    Boolean valid = false, enabled = CFPreferencesGetAppBooleanValue(RWDataKey, RWDomain, &valid);
    return @(!valid || enabled);
}
- (void)setCellularData:(NSNumber *)value specifier:(PSSpecifier *)specifier
{
    (void)specifier;
    CFPreferencesSetAppValue(RWDataKey, value.boolValue ? kCFBooleanTrue : kCFBooleanFalse, RWDomain);
    CFPreferencesAppSynchronize(RWDomain);
    notify_post("com.redwood.cellular.data-changed");
    BOOL sent = cell_command(value.boolValue ? "data\n" : "nodata\n");
    diagnostic(value.boolValue ? (sent ? "data-on-sent" : "data-on-not-sent") : (sent ? "data-off-sent" : "data-off-not-sent"));
}
- (NSString *)network:(PSSpecifier *)specifier
{
    (void)specifier;
    BOOL fresh = NO;
    NSDictionary *st = cell_state(&fresh);
    if (!fresh) return @"No Service";
    return [st[@"ios_reg"] intValue] == 2 ? (st[@"operator"] ?: @"Registered") : @"Searching…";
}
- (NSString *)signal:(PSSpecifier *)specifier
{
    (void)specifier;
    BOOL fresh = NO;
    NSDictionary *st = cell_state(&fresh);
    if (!fresh || !st[@"dbm"]) return @"—";
    return [NSString stringWithFormat:@"%@ of %@ bars (%@ dBm)", st[@"bars"] ?: @"0", st[@"max"] ?: @"4", st[@"dbm"]];
}
- (NSString *)technology:(PSSpecifier *)specifier
{
    (void)specifier;
    BOOL fresh = NO;
    NSDictionary *st = cell_state(&fresh);
    if (!fresh) return @"—";
    NSString *rat = st[@"rat"] ?: @"LTE";
    return [st[@"data_on"] intValue] ? [rat stringByAppendingString:@" · data on"] : [rat stringByAppendingString:@" · data off"];
}
- (NSString *)carrier:(PSSpecifier *)specifier
{
    (void)specifier;
    BOOL fresh = NO;
    NSDictionary *st = cell_state(&fresh);
    return fresh && [st[@"operator"] length] ? st[@"operator"] : @"—";
}
- (NSString *)sim:(PSSpecifier *)specifier
{
    (void)specifier;
    BOOL fresh = NO;
    NSDictionary *st = cell_state(&fresh);
    return fresh && [st[@"plmn"] length] && ![st[@"plmn"] isEqualToString:@"000000"] ? @"Ready" : @"—";
}
- (NSString *)plmn:(PSSpecifier *)specifier
{
    (void)specifier;
    BOOL fresh = NO;
    NSDictionary *st = cell_state(&fresh);
    NSString *p = st[@"plmn"];
    if (!fresh || p.length < 5) return @"—";
    return [NSString stringWithFormat:@"%@ %@", [p substringToIndex:3], [p substringFromIndex:3]];
}
/* VoLTE state from the call engine itself (redwood-ims rewrites call-ui/calls at least every 2 s: "ims PID TIME
 * registered 0|1"), not from a marker file that outlives a stopped engine. 0 = no engine, 1 = up, not registered,
 * 2 = registered. Calls also need the phone's telephony provider (redwood-telephonyd touches call-ui/alive). */
static int ims_state(void)
{
    FILE *f = fopen("/var/tmp/redwood-cell/call-ui/calls", "r");
    long pid = 0, t = 0; int reg = 0;
    if (!f) return 0;
    int n = fscanf(f, "ims %ld %ld registered %d", &pid, &t, &reg);
    fclose(f);
    if (n != 3 || time(NULL) - t > 15) return 0;
    return reg ? 2 : 1;
}
static BOOL provider_alive(void)
{
    struct stat st;
    return !stat("/var/tmp/redwood-cell/call-ui/alive", &st) && time(NULL) - st.st_mtime < 20;
}
- (NSString *)volte:(PSSpecifier *)specifier
{
    (void)specifier;
    switch (ims_state()) {
    case 2: return @"Registered";
    case 1: return @"Registering…";
    default: return @"Off";
    }
}
- (NSString *)calls:(PSSpecifier *)specifier
{
    (void)specifier;
    if (ims_state() != 2) return @"Not available";
    return provider_alive() ? @"Available (VoLTE)" : @"Starting…";
}
- (NSString *)sms:(PSSpecifier *)specifier
{
    (void)specifier;
    return ims_state() == 2 ? @"Available (over IMS)" : @"Not available";
}
@end

static NSArray *(*root_specifiers)(id, SEL);
static Class hooked_class;
static BOOL hooked;
static atomic_bool install_queued;

static void refresh_root(UIViewController *controller, Class cls, unsigned depth)
{
    if (!controller || depth > 16) return;
    if ([controller isKindOfClass:cls] && [controller respondsToSelector:@selector(reloadSpecifiers)]) {
        [(PSListController *)controller reloadSpecifiers];
        return;
    }
    for (UIViewController *child in controller.childViewControllers) refresh_root(child, cls, depth + 1);
}

static UIImage *cellular_icon(void)
{
    static UIImage *icon;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc]
            initWithSize:CGSizeMake(29, 29)];
        icon = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
            (void)context;
            [UIColor.systemGreenColor setFill];
            [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, 29, 29) cornerRadius:6] fill];
            UIImageSymbolConfiguration *configuration = [UIImageSymbolConfiguration
                configurationWithPointSize:20 weight:UIImageSymbolWeightRegular];
            UIImage *symbol = [[UIImage systemImageNamed:@"antenna.radiowaves.left.and.right"
                withConfiguration:configuration] imageWithTintColor:UIColor.whiteColor
                renderingMode:UIImageRenderingModeAlwaysOriginal];
            [symbol drawInRect:CGRectMake((29 - symbol.size.width) / 2,
                (29 - symbol.size.height) / 2, symbol.size.width, symbol.size.height)];
        }];
    });
    return icon;
}

static NSArray *cellular_root_specifiers(id self, SEL command)
{
    static dispatch_once_t entered;
    dispatch_once(&entered, ^{ diagnostic("root-specifiers-entered"); });
    NSArray *original = root_specifiers(self, command);
    if (![original isKindOfClass:NSArray.class]) return original;
    NSMutableArray *items = [original mutableCopy];
    NSUInteger insert = NSNotFound;
    for (NSUInteger i = 0; i < items.count; i++) {
        id item = items[i];
        if (![item respondsToSelector:@selector(propertyForKey:)]) continue;
        NSString *identifier = [item propertyForKey:@"id"];
        if ([identifier isEqual:RWCellularID]) {
            insert = i;
            [items removeObjectAtIndex:i];
            break;
        }
        if ([identifier isEqual:@"Bluetooth"]) insert = i + 1;
    }
    if (insert == NSNotFound) {
        /* Avoid an unexpected layout when the root list isn't initialized yet. */
        return original;
    }
    PSSpecifier *cell = [PSSpecifier preferenceSpecifierNamed:@"Cellular" target:nil
        set:NULL get:NULL detail:RWCellularSettingsController.class cell:1 edit:Nil];
    [cell setProperty:RWCellularID forKey:@"id"];
    [cell setProperty:@YES forKey:@"enabled"];
    [cell setProperty:cellular_icon() forKey:@"iconImage"];
    [items insertObject:cell atIndex:MIN(insert, items.count)];
    store_specifiers(self, items);
    static dispatch_once_t row_once;
    dispatch_once(&row_once, ^{ diagnostic("cellular-row-ready"); });
    return items;
}

static void install_root_hook(void)
{
    if (hooked) return;
    /* iOS 16.5 uses PSUIPrefsListController for the visible iPhone root list.
     * PSUIPrefsRootController exists too, but isn't the displayed controller. */
    Class cls = objc_getClass("PSUIPrefsListController");
    SEL selector = @selector(specifiers);
    Method method = cls ? class_getInstanceMethod(cls, selector) : NULL;
    if (!method || method_getNumberOfArguments(method) != 2) return;
    char type[16] = {0};
    method_getReturnType(method, type, sizeof(type));
    if (type[0] != '@') return;
    IMP previous = method_getImplementation(method);
    /* A subclass may inherit this method. Add an override instead of replacing
     * PSListController's implementation for every Settings page. */
    root_specifiers = (void *)previous;
    if (!class_addMethod(cls, selector, (IMP)cellular_root_specifiers, method_getTypeEncoding(method))) {
        method_setImplementation(class_getInstanceMethod(cls, selector), (IMP)cellular_root_specifiers);
    }
    hooked = YES;
    hooked_class = cls;
    diagnostic("menu-hook-installed");
    NSLog(@"RedwoodCellular: Settings Cellular menu installed (radio unavailable)");
    /* The first root list may already exist by the time this main-queue task
     * runs. Refresh only Settings' own root, not every preferences controller. */
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) {
            refresh_root(window.rootViewController, cls, 0);
        }
    }
}

static void image_added(const struct mach_header *header, intptr_t slide)
{
    (void)header;
    (void)slide;
    if (atomic_exchange(&install_queued, true)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store(&install_queued, false);
        install_root_hook();
    });
}

__attribute__((constructor)) static void cellular_settings_init(void)
{
    if (strcmp(getprogname(), "Preferences")) return;
    /* PreferencesUI may load later. No UIKit/Objective-C work under dyld's lock. */
    _dyld_register_func_for_add_image(image_added);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (!hooked_class) return;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class]) continue;
            for (UIWindow *window in ((UIWindowScene *)scene).windows) {
                refresh_root(window.rootViewController, hooked_class, 0);
            }
        }
    });
}

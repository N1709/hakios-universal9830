/* Shared presentation compatibility for ordinary UIKit applications.
 * Metal discovery is published centrally by the graphics startup job. This
 * library does not hook IOKit, patch code, alter opacity or change orientation. */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <dlfcn.h>

static void prepare_layer(CALayer *layer, unsigned depth, unsigned *remaining, CGFloat scale)
{
    if (!layer || depth > 32 || !*remaining) return;
    --*remaining;
    if ([layer isKindOfClass:CAMetalLayer.class]) {
        CALayer *parent = layer.superlayer;
        if (parent && ![parent isKindOfClass:CAMetalLayer.class]) {
            if (!parent.shouldRasterize) parent.shouldRasterize = YES;
            if (parent.rasterizationScale != scale) parent.rasterizationScale = scale;
        }
    }
    for (CALayer *child in layer.sublayers) prepare_layer(child, depth + 1, remaining, scale);
}

static void prepare_windows(void)
{
    if (UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    unsigned remaining = 4000;
    for (UIWindowScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *window in scene.windows) {
            if (!window.hidden) prepare_layer(window.layer, 0, &remaining, window.screen.scale);
        }
    }
    [CATransaction commit];
}

/* Coalesce lifecycle bursts. All state and traversal stay on the main queue. */
static void schedule_windows(void)
{
    static BOOL pending;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (pending) return;
        pending = YES;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250*NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            pending = NO;
            prepare_windows();
        });
    });
}

__attribute__((constructor)) static void start(void)
{
    /* Keep the existing MPS fallback policy without stop-all-threads hooks. */
    void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW|RTLD_GLOBAL);
    void (*set_mode)(uint64_t,uint32_t) = metal ? dlsym(metal,"MTLFailureTypeSetErrorModeType") : NULL;
    if (set_mode) for (uint64_t t=0;t<5;t++) set_mode(t,4);
    NSNotificationCenter *center=NSNotificationCenter.defaultCenter;
    for (NSString *name in @[UIApplicationDidBecomeActiveNotification,
                             UIWindowDidBecomeVisibleNotification,
                             UIDeviceOrientationDidChangeNotification]) {
        [center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue
                        usingBlock:^(__unused NSNotification *note) {
            schedule_windows();
        }];
    }
    /* Bounded startup sweep covers layers attached after the first visible window.
     * No repeating background timer and no filesystem diagnostics. */
    for (unsigned n=1;n<=30;n++)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,n*NSEC_PER_SEC),
                       dispatch_get_main_queue(),^{prepare_windows();});
}

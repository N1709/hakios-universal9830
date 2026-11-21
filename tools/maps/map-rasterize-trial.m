/* Apple Maps-only workaround for the portrait compositor failure. Keep the
 * exact tested VKMapView rasterization, without layer dumps or coordinates.
 * This is a scoped workaround; it does not repair the global compositor. */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <string.h>
#include <stdlib.h>

static void prepare(CALayer *layer, unsigned depth, unsigned *remaining)
{
    if (!layer || depth > 30 || !*remaining) return;
    --*remaining;
    if ([NSStringFromClass(layer.class) isEqualToString:@"VKMapView"]) {
        layer.shouldRasterize = YES;
        layer.rasterizationScale = UIScreen.mainScreen.scale;
    }
    for (CALayer *child in layer.sublayers) prepare(child, depth + 1, remaining);
}

__attribute__((constructor)) static void start(void)
{
    if (strcmp(getprogname(), "Maps")) return;
    /* Retain the tested bounded startup scan; no background timer or disk log. */
    for (unsigned n = 1; n <= 60; ++n) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)n * 2 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            unsigned remaining = 3000;
            for (UIWindowScene *scene in UIApplication.sharedApplication.connectedScenes) {
                if (![scene isKindOfClass:UIWindowScene.class]) continue;
                for (UIWindow *window in scene.windows) prepare(window.layer, 0, &remaining);
            }
        });
    }
}

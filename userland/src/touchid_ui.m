/*
 * libredwood-touchid-ui: Touch ID's on-screen place on the S20, whose sensor is under the display (iOS's Touch ID
 * screens were made for a home button). Loaded into SpringBoard and Preferences (kern.exec_inject).
 *
 *   Preferences: Settings' enrolment screen (BKUIFingerprintEnrollViewController) draws its fingerprint mid-screen;
 *                it is moved so its centre is on the sensor. The screen's view tree is written once to
 *                /tmp/enroll-views.txt (classes and frames) to keep the choice of view checkable.
 *   SpringBoard: fp_ring.m, the ring on the sensor while redwood-fpd waits for a finger (lock screen, app prompts).
 *
 * The sensor: bsd/dev/redwood_teegris.c captures inside x 27500-38000, y 46500-55500 of 0-65535 (centre 32768, 50790).
 */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#define SENSOR_X (32768.0 / 65535.0)
#define SENSOR_Y (50790.0 / 65535.0)

static BOOL dumped;

static void
dumpTree(UIView *v, NSMutableString *out, int depth)
{
	CGRect f = [v convertRect:v.bounds toView:nil];
	[out appendFormat:@"%*s%@ %.0f,%.0f %.0fx%.0f%@\n", depth * 2, "", NSStringFromClass(v.class), f.origin.x, f.origin.y,
	    f.size.width, f.size.height, v.hidden ? @" hidden" : @""];
	for (UIView *s in v.subviews) {
		dumpTree(s, out, depth + 1);
	}
}

/* the fingerprint: a view whose class names it, else the largest square-ish non-control view in the screen's middle */
static UIView *
findFingerprint(UIView *root)
{
	__block UIView *named = nil, *best = nil;
	__block CGFloat bestArea = 0;
	CGRect screen = UIScreen.mainScreen.bounds;
	__block void (^walk)(UIView *) = nil;
	void (^w)(UIView *) = ^(UIView *v) {
		NSString *n = NSStringFromClass(v.class);
		if (!named && ([n rangeOfString:@"Finger" options:NSCaseInsensitiveSearch].location != NSNotFound ||
		    [n rangeOfString:@"Glyph"].location != NSNotFound || [n rangeOfString:@"Ridge"].location != NSNotFound) &&
		    ![v isKindOfClass:UIControl.class] && ![v isKindOfClass:UILabel.class] && v.bounds.size.width > 60) {
			named = v;
		}
		CGRect f = [v convertRect:v.bounds toView:nil];
		CGFloat ratio = f.size.height > 0 ? f.size.width / f.size.height : 0;
		if (!v.hidden && ![v isKindOfClass:UIControl.class] && ![v isKindOfClass:UILabel.class] &&
		    ![v isKindOfClass:UIScrollView.class] && ratio > 0.75 && ratio < 1.33 && f.size.width > 100 &&
		    f.size.width < screen.size.width * 0.9 && fabs(CGRectGetMidX(f) - screen.size.width / 2) < 40) {
			CGFloat area = f.size.width * f.size.height;
			if (area > bestArea) {
				bestArea = area;
				best = v;
			}
		}
		for (UIView *s in v.subviews) {
			walk(s);
		}
	};
	walk = w;
	walk(root);
	return named ?: best;
}

static void (*origLayout)(id, SEL);

static void
enrollLayout(UIViewController *self, SEL _cmd)
{
	origLayout(self, _cmd);
	UIView *root = self.view;
	if (!dumped && root.window) {
		dumped = YES;
		NSMutableString *t = [NSMutableString string];
		dumpTree(root, t, 0);
		[t writeToFile:@"/tmp/enroll-views.txt" atomically:YES encoding:NSUTF8StringEncoding error:NULL];
	}
	UIView *fp = findFingerprint(root);
	if (!fp || !root.window) {
		return;
	}
	CGRect screen = root.window.bounds;
	CGPoint want = CGPointMake(screen.size.width * SENSOR_X, screen.size.height * SENSOR_Y);
	fp.transform = CGAffineTransformIdentity;
	CGRect f = [fp convertRect:fp.bounds toView:nil];
	CGPoint have = CGPointMake(CGRectGetMidX(f), CGRectGetMidY(f));
	fp.transform = CGAffineTransformMakeTranslation(want.x - have.x, want.y - have.y);
}

__attribute__((constructor)) static void
touchidUIInit(void)
{
	if (![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.Preferences"]) {
		return;
	}
	/* BiometricKitUI loads with the Touch ID pane: hook its class once it exists */
	static dispatch_source_t timer;
	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC / 2, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler(timer, ^{
		Class c = objc_getClass("BKUIFingerprintEnrollViewController");
		if (!c) {
			return;
		}
		Method m = class_getInstanceMethod(c, @selector(viewDidLayoutSubviews));
		if (m) {
			origLayout = (void (*)(id, SEL))method_getImplementation(m);
			class_replaceMethod(c, @selector(viewDidLayoutSubviews), (IMP)enrollLayout, method_getTypeEncoding(m));
		}
		dispatch_source_cancel(timer);
	});
	dispatch_resume(timer);
}

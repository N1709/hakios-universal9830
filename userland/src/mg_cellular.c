/*
 * libredwood-mgcell: "cellular-data" as true where Personal Hotspot is drawn (Settings) (A, 2026-11-19).
 *
 * MobileGestalt works "cellular-data" out of an Apple baseband (HasBaseband), which the S20 has not: its modem is
 * Samsung's, run by redwood-cellulard, and the phone does have cellular data (LTE, and IPv4 through CLAT46).
 * Settings' Personal Hotspot row (PreferencesUI's Settings.plist, id INTERNET_TETHERING) requires the capabilities
 * "personal-hotspot" (true here) and "cellular-data" (false: no row). Settings asks through
 * -[PSCapabilityManager capabilityAnswer:]; only that answer, for that one key, changes. The row, its pane and
 * misd behind them are iOS's own. (dyld interposing of MGGetBoolAnswer does not reach calls made inside the
 * shared cache, so the hook is on the Objective-C method.)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <mach-o/dyld.h>
#include <stdbool.h>

static id (*orig_capabilityAnswer)(id, SEL, id);

static id
rw_capabilityAnswer(id self, SEL _cmd, id key)
{
	if (key != NULL && CFGetTypeID((CFTypeRef)key) == CFStringGetTypeID() &&
	    CFStringCompare((CFStringRef)key, CFSTR("cellular-data"), 0) == kCFCompareEqualTo) {
		return (id)kCFBooleanTrue;
	}
	return orig_capabilityAnswer(self, _cmd, key);
}

static bool hooked;

static void
try_hook(void)
{
	Class c = hooked ? Nil : objc_getClass("PSCapabilityManager");
	Method m = c != Nil ? class_getInstanceMethod(c, sel_registerName("capabilityAnswer:")) : NULL;

	if (m != NULL) {
		hooked = true;
		orig_capabilityAnswer = (id (*)(id, SEL, id))method_setImplementation(m, (IMP)rw_capabilityAnswer);
		os_log(OS_LOG_DEFAULT, "rw-mgcell: cellular-data answered true for Settings (Personal Hotspot)");
	}
}

/* Preferences.framework may not be loaded yet when this library starts: try again as images come */
static void
image_added(const struct mach_header *mh, intptr_t slide)
{
	(void)mh;
	(void)slide;
	try_hook();
}

__attribute__((constructor)) static void
mgcell_init(void)
{
	try_hook();
	if (!hooked) {
		_dyld_register_func_for_add_image(image_added);
	}
}

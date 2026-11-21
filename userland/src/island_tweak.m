/*
 * RedwoodIsland.dylib: the Dynamic Island's pill sized and placed for the S20's camera hole.
 *
 * iOS 16.5 does not take the pill from the hardware: SpringBoardUIServices returns it from two small
 * Objective-C methods with the iPhone 14 Pro's numbers in them (20F66, iPhone10,3 cache, unslid):
 *   IMP 0x1a33f7598  the collapsed pill's size, {125.0 or 125.333, 36.667} pt
 *   IMP 0x1a33f7954  its gap from the top of the screen, 11.333 pt
 * Their selectors are not known here (ipsw does not read this cache's Objective-C data), so the methods
 * are found by those addresses (plus the shared cache's slide) among the classes of SpringBoardUIServices,
 * and their implementations replaced.
 *
 * Default (no file): the pill's top at 5 pt (Apple: 11.333), which puts it over the top of the S20's hole
 * (user-checked 2026-11-12); its size stays Apple's. /var/jb/etc/redwood-island.conf, read once when
 * SpringBoard starts (tuning takes a respring, no reboot), overrides that:
 *     w=81 h=36.667 y=5        (points; any left out keep Apple's value)
 *     off                      (nothing hooked: Apple's pill) Goes into SpringBoard through the tweak loader
 * (src/inject_tweaks.c; RedwoodIsland.plist filters on SpringBoard). docs/devices/x1slte/dynamic-island.md.
 */
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#include <stdarg.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONF            "/var/jb/etc/redwood-island.conf"
#define LOG             "/var/mobile/Library/Logs/redwood-island.log"
#define SBUIS           "/System/Library/PrivateFrameworks/SpringBoardUIServices.framework/SpringBoardUIServices"
/* unslid addresses in the 20F66 iPhone10,3 shared cache */
#define ADDR_ANCHOR     0x1dd7b9568ULL  /* _OBJC_CLASS_$_SBUISystemApertureLayoutMetrics (gives the slide) */
#define ADDR_SIZE       0x1a33f7598ULL  /* -> CGSize, the collapsed pill */
#define ADDR_TOP        0x1a33f7954ULL  /* -> CGFloat, the pill's top inset */

#define DEFAULT_Y       5.0

static double want_w = -1, want_h = -1, want_y = DEFAULT_Y;
static int want_probe;
static CGSize (*orig_size)(id, SEL);
static CGFloat (*orig_top)(id, SEL);

static void
say(const char *fmt, ...)
{
	FILE *f = fopen(LOG, "a");
	va_list ap;

	if (f == NULL) {
		return;
	}
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static CGSize
island_size(id self, SEL cmd)
{
	CGSize s = orig_size(self, cmd);

	if (want_w > 0) {
		s.width = want_w;
	}
	if (want_h > 0) {
		s.height = want_h;
	}
	return s;
}

static CGFloat
island_top(id self, SEL cmd)
{
	return want_y >= 0 ? want_y : orig_top(self, cmd);
}

static int
read_conf(void)
{
	char buf[256];
	FILE *f = fopen(CONF, "r");
	size_t n;

	if (f == NULL) {
		return 1;       /* no file: the defaults */
	}
	want_y = -1;    /* a file says everything it wants */
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	for (char *p = strtok(buf, " \t\r\n"); p; p = strtok(NULL, " \t\r\n")) {
		if (strcmp(p, "off") == 0) {
			return 0;
		} else if (strcmp(p, "probe") == 0) {
			want_probe = 1;     /* diagnostics: list SpringBoard's island classes into the log */
		} else if (strncmp(p, "w=", 2) == 0) {
			want_w = atof(p + 2);
		} else if (strncmp(p, "h=", 2) == 0) {
			want_h = atof(p + 2);
		} else if (strncmp(p, "y=", 2) == 0) {
			want_y = atof(p + 2);
		}
	}
	return 1;
}

/* Replace the method (instance or class) of an SBUIS class whose implementation is at addr. */
static IMP
hook_by_address(uintptr_t addr, IMP replacement, const char *what)
{
	unsigned nclasses = 0;
	const char **names = objc_copyClassNamesForImage(SBUIS, &nclasses);
	IMP found = NULL;

	for (unsigned i = 0; names && i < nclasses && !found; i++) {
		Class cls = objc_getClass(names[i]);

		for (int meta = 0; cls && meta < 2 && !found; meta++) {
			Class c = meta ? object_getClass((id)cls) : cls;
			unsigned nm = 0;
			Method *ms = class_copyMethodList(c, &nm);

			for (unsigned j = 0; j < nm; j++) {
				if ((uintptr_t)method_getImplementation(ms[j]) == addr) {
					say("island: %s = %c[%s %s], hooked", what, meta ? '+' : '-', names[i],
					    sel_getName(method_getName(ms[j])));
					found = method_setImplementation(ms[j], replacement);
					break;
				}
			}
			free(ms);
		}
	}
	free(names);
	if (!found) {
		say("island: %s: no method at %#lx (not the 20F66 cache?), left alone", what, (unsigned long)addr);
	}
	return found;
}

/*
 * Nothing is touched while the program is still starting: loaded that early (a cold boot), forcing
 * SpringBoardUIServices in made SpringBoardUI throw "This process should not be linking or loading
 * SpringBoardUI.framework" and SpringBoard died at every start (2026-11-12). The hooks go in from the
 * main queue, once SpringBoard runs, and only if SpringBoardUIServices is already there on its own.
 */
static void
island_hook(void)
{
	void *anchor;
	uintptr_t slide;

	anchor = (__bridge void *)objc_lookUpClass("SBUISystemApertureLayoutMetrics");
	if (anchor == NULL) {
		say("island: SpringBoardUIServices not loaded, left alone");
		return;
	}
	slide = (uintptr_t)anchor - (uintptr_t)ADDR_ANCHOR;
	say("island: conf w=%g h=%g y=%g (negative = Apple's), slide %#lx", want_w, want_h, want_y, (unsigned long)slide);
	if (want_w > 0 || want_h > 0) {
		orig_size = (CGSize (*)(id, SEL))hook_by_address(ADDR_SIZE + slide, (IMP)island_size, "pill size");
	}
	if (want_y >= 0) {
		orig_top = (CGFloat (*)(id, SEL))hook_by_address(ADDR_TOP + slide, (IMP)island_top, "pill top");
	}
}


/* "probe" in the conf: the island's classes in SpringBoard and their state-ish methods, into the log. */
static void
island_probe(void)
{
	unsigned n = 0;
	const char **names = objc_copyClassNamesForImage([[[NSBundle mainBundle] executablePath] UTF8String], &n);

	for (unsigned i = 0; names && i < n; i++) {
		if (strstr(names[i], "SystemAperture") == NULL && strstr(names[i], "Jindo") == NULL &&
		    strstr(names[i], "Activit") == NULL) {
			continue;
		}
		Class cls = objc_getClass(names[i]);
		unsigned nm = 0;
		Method *ms = class_copyMethodList(cls, &nm);
		char line[3000] = "";
		size_t len = 0;

		for (unsigned j = 0; j < nm && len < sizeof(line) - 80; j++) {
			const char *sel = sel_getName(method_getName(ms[j]));
			if (strcasestr(sel, "dump") || strcasestr(sel, "state") || strcasestr(sel, "element") ||
			    strcasestr(sel, "shared") || strcasestr(sel, "enabled") || strcasestr(sel, "activit")) {
				len += (size_t)snprintf(line + len, sizeof(line) - len, " %s", sel);
			}
		}
		free(ms);
		say("probe: %s:%s", names[i], line);
	}
	free(names);
}

__attribute__((constructor)) static void
island_init(void)
{
	if (!read_conf()) {
		return;
	}
	dispatch_async(dispatch_get_main_queue(), ^{
		island_hook();
		if (want_probe) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
				island_probe();
			});
		}
	});
}

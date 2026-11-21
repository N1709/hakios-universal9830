/*
 * redwood-defaults: this port's defaults for a fresh data volume, applied ONCE (2026-11-20).
 *
 * A clean install starts the data volume empty, so preferences that make the phone work as shipped were missing:
 * Location Services off (Compass, Maps, GPS dead, and no switch in Settings to find), keyboard haptics off, Live
 * Photo on. Run at boot (com.redwood.defaults); the marker /var/root/.redwood-defaults-v1 makes it a once-only step,
 * so whatever the user changes later is never overwritten. Bump the marker's version to apply a new default.
 * Keys found by diffing the preference files around the Settings switches, and in iOS 16.5's CameraUI:
 *   Location Services: +[CLLocationManager setLocationServicesEnabled:YES], as Settings does (needs the
 *                     com.apple.locationd.preferences entitlement: tools/redwood-defaults.entitlements). Writing
 *                     LocationServicesEnabledIn7.0 into a preferences file is ignored by iOS 16.5's locationd: the
 *                     switch stayed off and Settings hid its Location Services row (PUIPrivacyController).
 *   mobile com.apple.keyboard.preferences KeyboardVisceral = 2                  (Keyboard Feedback: Haptic)
 *   mobile com.apple.camera               CAMUserPreferencePreserveLivePhoto = true, ..DesiredIrisMode = 0
 *                                                                               (Live Photo off, and kept off)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MARKER "/var/root/.redwood-defaults-v1"
#define CAMERA_MARKER "/var/root/.redwood-camera-defaults-v2"

/* libproc (in libSystem; no header in the iOS SDK) */
int proc_listallpids(void *buffer, int buffersize);
int proc_pidpath(int pid, void *buffer, uint32_t buffersize);

static int
set(CFStringRef user, const char *domain, const char *key, CFPropertyListRef value)
{
	CFStringRef d = CFStringCreateWithCString(NULL, domain, kCFStringEncodingUTF8);
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFPreferencesSetValue(k, value, d, user, kCFPreferencesAnyHost);
	Boolean ok = CFPreferencesSynchronize(d, user, kCFPreferencesAnyHost);
	printf("redwood-defaults: %s %s = set%s\n", domain, key, ok ? "" : " (NOT written)");
	CFRelease(k);
	CFRelease(d);
	return ok ? 0 : 1;
}

static int
set_int(CFStringRef user, const char *domain, const char *key, int v)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
	int r = set(user, domain, key, n);
	CFRelease(n);
	return r;
}

/* CameraUI 16.5 resets IrisMode on its first launch unless ForcedLivePhotoOnTenTwo
 * exists (0x1cdccd898). Its next migration can upgrade On to Auto (0x1cdccd948).
 * Seed both migration flags, not just the desired mode. A separate marker repairs
 * v1 installs once without resetting location, keyboard feedback or region. */
static int
camera_defaults(void)
{
	if (access(CAMERA_MARKER, F_OK) == 0) return 0;
	int r = set(CFSTR("mobile"), "com.apple.camera", "CAMUserPreferenceForcedLivePhotoOnTenTwo", kCFBooleanTrue);
	r |= set(CFSTR("mobile"), "com.apple.camera", "CAMUserPreferenceDidPerformAutoLivePhotoUpgrade", kCFBooleanTrue);
	r |= set(CFSTR("mobile"), "com.apple.camera", "CAMUserPreferencePreserveLivePhoto", kCFBooleanTrue);
	r |= set_int(CFSTR("mobile"), "com.apple.camera", "CAMUserPreferenceDesiredIrisMode", 0);
	if (r == 0) {
		int fd = open(CAMERA_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) return 1;
		close(fd);
		printf("redwood-defaults: Live Photo off; Camera migrations seeded\n");
	}
	return r;
}

/* Location Services on through CoreLocation, as Settings does; 0 when it reads back on */
static int
location_services_on(void)
{
	Class L = objc_getClass("CLLocationManager");
	SEL get = sel_registerName("locationServicesEnabled"), set_ = sel_registerName("setLocationServicesEnabled:");

	if (L == Nil || !class_respondsToSelector(object_getClass(L), set_)) {
		printf("redwood-defaults: CoreLocation has no setLocationServicesEnabled:\n");
		return 1;
	}
	((void (*)(Class, SEL, BOOL))objc_msgSend)(L, set_, 1);
	for (int i = 0; i < 10; i++) {
		if (((BOOL (*)(Class, SEL))objc_msgSend)(L, get)) {
			printf("redwood-defaults: Location Services on\n");
			return 0;
		}
		usleep(500000);
	}
	printf("redwood-defaults: Location Services did NOT turn on\n");
	return 1;
}

/* locationd reads its switch at start: restart it (launchd starts it again) */
static void
restart_locationd(void)
{
	pid_t pids[2048];
	int n = proc_listallpids(pids, sizeof(pids));

	for (int i = 0; i < n; i++) {
		char path[4096];
		if (proc_pidpath(pids[i], path, sizeof(path)) > 0 && strcmp(path, "/usr/libexec/locationd") == 0) {
			kill(pids[i], SIGKILL);
			printf("redwood-defaults: locationd restarted\n");
		}
	}
}

/*
 * The region from the SIM (A, 2026-11-20). A clean install skips the setup assistant, so there is no AppleLocale and
 * iOS takes en_US: Phone and Messages put +1 in front of local numbers. Once per data volume (its own marker), and only
 * when the user has chosen no region (India when there is no SIM country): wait for the modem service's MCC (/var/tmp/redwood-cell/call-ui/mcc, written
 * once the SIM is registered) and set AppleLocale to the language with the SIM's country, e.g. en_IN for MCC 404-406.
 */
#define REGION_MARKER "/var/root/.redwood-region-v1"
static const char *
mcc_country(int m)
{
	static const struct { int lo, hi; const char *cc; } t[] = {
		{ 404, 406, "IN" }, { 310, 316, "US" }, { 302, 302, "CA" }, { 234, 235, "GB" }, { 262, 262, "DE" },
		{ 208, 208, "FR" }, { 222, 222, "IT" }, { 214, 214, "ES" }, { 204, 204, "NL" }, { 505, 505, "AU" },
		{ 530, 530, "NZ" }, { 440, 441, "JP" }, { 450, 450, "KR" }, { 460, 460, "CN" }, { 525, 525, "SG" },
		{ 502, 502, "MY" }, { 510, 510, "ID" }, { 515, 515, "PH" }, { 520, 520, "TH" }, { 452, 452, "VN" },
		{ 424, 424, "AE" }, { 420, 420, "SA" }, { 427, 427, "QA" }, { 419, 419, "KW" }, { 422, 422, "OM" },
		{ 426, 426, "BH" }, { 410, 410, "PK" }, { 470, 470, "BD" }, { 413, 413, "LK" }, { 429, 429, "NP" },
		{ 655, 655, "ZA" }, { 621, 621, "NG" }, { 639, 639, "KE" }, { 602, 602, "EG" }, { 724, 724, "BR" },
		{ 334, 334, "MX" }, { 722, 722, "AR" }, { 250, 250, "RU" }, { 255, 255, "UA" }, { 286, 286, "TR" },
		{ 260, 260, "PL" }, { 240, 240, "SE" }, { 242, 242, "NO" }, { 238, 238, "DK" }, { 244, 244, "FI" },
		{ 228, 228, "CH" }, { 232, 232, "AT" }, { 206, 206, "BE" }, { 272, 272, "IE" }, { 268, 268, "PT" },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (m >= t[i].lo && m <= t[i].hi) return t[i].cc;
	return NULL;
}

static void
region_from_sim(void)
{
	struct stat st;
	char buf[16] = "";
	const char *cc = NULL;

	if (stat(REGION_MARKER, &st) == 0) return;
	CFPropertyListRef have = CFPreferencesCopyValue(CFSTR("AppleLocale"), kCFPreferencesAnyApplication, CFSTR("mobile"),
	    kCFPreferencesAnyHost);
	if (have) {                     /* the user (or an earlier setup) chose a region: leave it */
		CFRelease(have);
		close(open(REGION_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0644));
		return;
	}
	for (int i = 0; i < 90 && !cc; i++) {           /* up to 3 minutes for the SIM to register */
		FILE *f = fopen("/var/tmp/redwood-cell/call-ui/mcc", "r");
		if (f) {
			if (fgets(buf, sizeof(buf), f)) cc = mcc_country(atoi(buf));
			fclose(f);
			if (!cc && buf[0]) break;               /* an MCC this table does not know: keep iOS's default */
		}
		if (!cc) sleep(2);
	}
	if (!cc) {              /* no SIM, or a country this table lacks: India, not iOS's US default (user, 2026-11-20) */
		cc = "IN";
		printf("redwood-defaults: region: no SIM country (MCC '%s'): India by default\n", buf);
	}
	char locale[16];
	snprintf(locale, sizeof(locale), "en_%s", cc);
	CFStringRef v = CFStringCreateWithCString(NULL, locale, kCFStringEncodingUTF8);
	CFPreferencesSetValue(CFSTR("AppleLocale"), v, kCFPreferencesAnyApplication, CFSTR("mobile"), kCFPreferencesAnyHost);
	Boolean ok = CFPreferencesSynchronize(kCFPreferencesAnyApplication, CFSTR("mobile"), kCFPreferencesAnyHost);
	CFRelease(v);
	printf("redwood-defaults: region %s from the SIM (MCC %d)%s\n", locale, atoi(buf), ok ? "" : " NOT written");
	if (ok) close(open(REGION_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0644));
}

int
main(void)
{
	struct stat st;
	int r = 0;

	if (stat(MARKER, &st) == 0) {
		r = camera_defaults();
		region_from_sim();
		return r;               /* other defaults already applied: the user's choices stand */
	}
	/* cfprefsd must be up to write mobile's preferences: wait for it (up to a minute after boot) */
	for (int i = 0; i < 30; i++) {
		CFPropertyListRef probe = CFPreferencesCopyValue(CFSTR("probe"), CFSTR("com.redwood.defaults"), CFSTR("mobile"),
		    kCFPreferencesAnyHost);
		if (probe) CFRelease(probe);
		if (access("/var/mobile/Library/Preferences", W_OK) == 0) break;
		sleep(2);
	}
	r |= location_services_on();
	r |= set_int(CFSTR("mobile"), "com.apple.keyboard.preferences", "KeyboardVisceral", 2);
	r |= camera_defaults();
	if (r == 0) {
		int fd = open(MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) close(fd);
		printf("redwood-defaults: applied (marker %s)\n", MARKER);
		region_from_sim();
	} else {
		printf("redwood-defaults: some not written; tries again at the next boot\n");
	}
	return r;
}

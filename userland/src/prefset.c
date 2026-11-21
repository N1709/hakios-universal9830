/*
 * prefset: set boolean preferences of the user mobile, as Settings would, without Settings' UI.
 *   prefset [-u USER] DOMAIN KEY 0|1 [KEY 0|1 ...]      (USER: mobile by default; root for daemons such as locationd)
 * x1slte (2026-11-06): Settings > Safari aborts (SafariCore, start page folder), so WebKit's feature flags
 * (VP9DecoderEnabled, VP9SWDecoderEnabledOnBattery, VP8DecoderEnabled: WebKit's software VP9/VP8 decoder registered
 * with VideoToolbox, as no hardware decoder is driven here) are set through CFPreferences from a boot job instead.
 * Run as root, it writes mobile's domain (CFPreferencesSetValue with user "mobile"), through cfprefsd.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
	CFStringRef user = CFSTR("mobile");
	if (argc > 2 && strcmp(argv[1], "-u") == 0) {   /* the preferences of another user: root for root daemons */
		user = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
		argv += 2;
		argc -= 2;
	}
	if (argc < 4 || (argc - 2) % 2 != 0) {
		fprintf(stderr, "usage: prefset [-u USER] DOMAIN KEY 0|1 [KEY 0|1 ...]\n");
		return 1;
	}
	CFStringRef domain = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
	for (int i = 2; i + 1 < argc; i += 2) {
		CFStringRef key = CFStringCreateWithCString(NULL, argv[i], kCFStringEncodingUTF8);
		CFPreferencesSetValue(key, strcmp(argv[i + 1], "0") ? kCFBooleanTrue : kCFBooleanFalse, domain, user,
		    kCFPreferencesAnyHost);
		CFRelease(key);
	}
	Boolean ok = CFPreferencesSynchronize(domain, user, kCFPreferencesAnyHost);
	printf("prefset: %s: %d key(s) %s\n", argv[1], (argc - 2) / 2, ok ? "written" : "NOT written");
	CFRelease(domain);
	return ok ? 0 : 2;
}

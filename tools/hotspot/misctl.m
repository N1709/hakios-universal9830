/*
 * misctl on|off|state: iOS's own Personal Hotspot switch from the command line (A, 2026-11-19), the call Settings'
 * "Allow Others to Join" makes: -[MISManager setState:] (WirelessModemSettings.bundle), 1023 on, 1022 off. Signed
 * with Settings' MobileInternetSharing entitlements (misctl.ent). For automatic tests without hands on the phone.
 * misctl wifi on|off, discover on|off: wifid's side (MobileWiFi's WiFiManagerClientSetMISState and
 * ...SetMISDiscoveryState, what Settings' pane asks while it is open: the access point visible).
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <CoreFoundation/CoreFoundation.h>

extern void *WiFiManagerClientCreate(CFAllocatorRef, int);
extern void WiFiManagerClientSetMISState(void *, int);
extern void WiFiManagerClientSetMISDiscoveryState(void *, int);
extern int WiFiManagerClientGetMISState(void *);
extern int WiFiManagerClientGetMISDiscoveryState(void *);

int
main(int argc, char **argv)
{
	@autoreleasepool {
		const char *what = argc > 1 ? argv[1] : "state";
		if ((strcmp(what, "wifi") == 0 || strcmp(what, "discover") == 0) && argc > 2) {
			void *w = WiFiManagerClientCreate(kCFAllocatorDefault, 0);
			int on = strcmp(argv[2], "on") == 0;
			if (w == NULL) {
				fprintf(stderr, "misctl: no WiFiManagerClient\n");
				return 1;
			}
			if (strcmp(what, "wifi") == 0) {
				WiFiManagerClientSetMISState(w, on);
			} else {
				WiFiManagerClientSetMISDiscoveryState(w, on);
			}
			[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2]];
			printf("misctl: wifid MIS state %d, discovery %d\n", WiFiManagerClientGetMISState(w), WiFiManagerClientGetMISDiscoveryState(w));
			return 0;
		}
		if (dlopen("/System/Library/PreferenceBundles/WirelessModemSettings.bundle/WirelessModemSettings", RTLD_NOW) == NULL) {
			fprintf(stderr, "misctl: %s\n", dlerror());
			return 1;
		}
		Class c = NSClassFromString(@"MISManager");
		id m = ((id (*)(Class, SEL))objc_msgSend)(c, sel_registerName("sharedManager"));
		if (m == nil) {
			fprintf(stderr, "misctl: no MISManager\n");
			return 1;
		}
		[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
		if (strcmp(what, "on") == 0 || strcmp(what, "off") == 0) {
			((void (*)(id, SEL, int))objc_msgSend)(m, sel_registerName("setState:"), strcmp(what, "on") == 0 ? 1023 : 1022);
			[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:3]];
		}
		int state = 0, reason = 0;
		((void (*)(id, SEL, int *, int *))objc_msgSend)(m, sel_registerName("getState:andReason:"), &state, &reason);
		printf("misctl: state %d (%s), reason %d\n", state, state == 1023 ? "on" : state == 1022 ? "off" : "unavailable", reason);
	}
	return 0;
}

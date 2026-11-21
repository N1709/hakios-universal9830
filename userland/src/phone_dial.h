/*
 * The Phone app's calls go out over VoLTE (A, 2026-11-14). There is no CommCenter: the telephony call provider has
 * nothing behind it, so a dial from the Phone app (and Contacts, Recents) is handed to RedwoodPhone instead, the
 * CallKit provider of redwood-ims (tools/cellular/RedwoodPhone): -[TUDialRequest provider] answers RedwoodPhone's
 * provider wherever it would answer the telephony one. callservicesd then sends RedwoodPhone a CXStartCallAction.
 * In MobilePhone and InCallService (dials made from the in-call screen), from libredwood.dylib's constructor.
 */
#include <objc/message.h>
#include <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <notify.h>

#define PHONE_PROVIDER_ID "com.redwood.phone"

static IMP phone_orig_provider;

static id
phone_our_provider(void)
{
	static id ours;
	if (ours) return ours;
	Class cc = objc_getClass("TUCallCenter");
	id center = cc ? ((id (*)(id, SEL))objc_msgSend)((id)cc, sel_registerName("sharedInstance")) : nil;
	id mgr = center ? ((id (*)(id, SEL))objc_msgSend)(center, sel_registerName("providerManager")) : nil;
	if (!mgr) return nil;
	id idstr = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), PHONE_PROVIDER_ID);
	const char *sels[] = { "providerWithIdentifier:", "providerForIdentifier:" };
	for (unsigned i = 0; i < 2 && !ours; i++) {
		SEL s = sel_registerName(sels[i]);
		if (class_respondsToSelector(object_getClass(mgr), s)) ours = ((id (*)(id, SEL, id))objc_msgSend)(mgr, s, idstr);
	}
	return ours;
}

static id
phone_provider(id self, SEL _cmd)
{
	id p = ((id (*)(id, SEL))phone_orig_provider)(self, _cmd);
	SEL tel = sel_registerName("isTelephonyProvider");
	if (p == nil || (class_respondsToSelector(object_getClass(p), tel) && ((BOOL (*)(id, SEL))objc_msgSend)(p, tel))) {
		id ours = phone_our_provider();
		if (ours) return ours;
	}
	return p;
}

static void
phone_dial_hook(void)
{
	static int tries;
	Class c = objc_getClass("TUDialRequest");
	Method m = c ? class_getInstanceMethod(c, sel_registerName("provider")) : NULL;
	if (!m) {
		if (tries++ < 20) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ phone_dial_hook(); });
		return;
	}
	if (!phone_orig_provider) phone_orig_provider = method_setImplementation(m, (IMP)phone_provider);
}

/*
 * RedwoodPhone's calls are the phone's own calls: in InCallService and SpringBoard its provider answers YES to
 * isSystemProvider / isTelephonyProvider, so iOS gives them its full call screen (as a third-party CallKit app's
 * calls they were kept in the Dynamic Island, the user, 2026-11-14 04:22). Not in MobilePhone: there the telephony
 * test above is what sends a dial to RedwoodPhone.
 */
static IMP phone_orig_system, phone_orig_tel;

static int
phone_is_ours(id provider)
{
	SEL ident = sel_registerName("identifier");
	if (!class_respondsToSelector(object_getClass(provider), ident)) return 0;
	id s = ((id (*)(id, SEL))objc_msgSend)(provider, ident);
	const char *c = s ? ((const char *(*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String")) : NULL;
	return c && !strcmp(c, PHONE_PROVIDER_ID);
}
/*
 * Which of the two answers are given is switchable at run time (notify state of com.redwood.call.spoof, set from a
 * shell: bit 0 isSystemProvider, bit 1 isTelephonyProvider, bit 7 set = use bits 0..1; 0 = this process's default),
 * so the full-screen call screen can be worked out without a new boot image each try.
 */
static int phone_spoof_default = 3;
static int
phone_spoof_flags(void)
{
	static int tok = -1;
	uint64_t st = 0;
	if (tok < 0) notify_register_check("com.redwood.call.spoof", &tok);
	if (tok >= 0) notify_get_state(tok, &st);
	return (st & 0x80) ? (int)(st & 3) : phone_spoof_default;
}
static BOOL phone_system(id self, SEL _cmd) { return (phone_spoof_flags() & 1) && phone_is_ours(self) ? YES : ((BOOL (*)(id, SEL))phone_orig_system)(self, _cmd); }
static BOOL phone_tel(id self, SEL _cmd) { return (phone_spoof_flags() & 2) && phone_is_ours(self) ? YES : ((BOOL (*)(id, SEL))phone_orig_tel)(self, _cmd); }

static void
phone_system_hook(void)
{
	static int tries;
	Class c = objc_getClass("TUCallProvider");
	Method a = c ? class_getInstanceMethod(c, sel_registerName("isSystemProvider")) : NULL;
	Method b = c ? class_getInstanceMethod(c, sel_registerName("isTelephonyProvider")) : NULL;
	if (!a || !b) {
		if (tries++ < 20) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ phone_system_hook(); });
		return;
	}
	if (!phone_orig_system) phone_orig_system = method_setImplementation(a, (IMP)phone_system);
	if (!phone_orig_tel) phone_orig_tel = method_setImplementation(b, (IMP)phone_tel);
}

/*
 * The call screen's Speaker button (A, 2026-11-14). The audio plug-in has no receiver port iOS knows, so iOS saw one
 * route and greyed the button. In InCallService, TURouteController offers two routes of ours, "iPhone" (the earpiece)
 * and "Speaker"; picking one posts com.redwood.call.speaker / .earpiece, which redwood-ims follows
 * (kern.redwood.snd_route). Every call starts on the earpiece (redwood-ims posts com.redwood.call.started).
 */
#include <notify.h>
#include <CoreFoundation/CoreFoundation.h>
static int phone_speaker;
static id phone_rcv, phone_spk;

static id
phone_route(CFStringRef uid, CFStringRef name, int speaker)
{
	Class c = objc_getClass("TURoute");
	id r = ((id (*)(id, SEL))objc_msgSend)((id)c, sel_registerName("alloc"));
	r = ((id (*)(id, SEL, id, id))objc_msgSend)(r, sel_registerName("initWithUniqueIdentifier:name:"), (id)uid, (id)name);
	((void (*)(id, SEL, BOOL))objc_msgSend)(r, sel_registerName(speaker ? "setSpeaker:" : "setReceiver:"), YES);
	return r;
}
static void
phone_routes_refresh(void)
{
	if (!phone_rcv) {
		phone_rcv = phone_route(CFSTR("com.redwood.route.receiver"), CFSTR("iPhone"), 0);
		phone_spk = phone_route(CFSTR("com.redwood.route.speaker"), CFSTR("Speaker"), 1);
	}
	((void (*)(id, SEL, BOOL))objc_msgSend)(phone_rcv, sel_registerName("setCurrentlyPicked:"), !phone_speaker);
	((void (*)(id, SEL, BOOL))objc_msgSend)(phone_spk, sel_registerName("setCurrentlyPicked:"), phone_speaker);
}
static void
phone_set_speaker(int on)
{
	phone_speaker = on;
	phone_routes_refresh();
	notify_post(on ? "com.redwood.call.speaker" : "com.redwood.call.earpiece");
}
static id phone_routes(id self, SEL _cmd)
{
	phone_routes_refresh();
	CFTypeRef v[2] = { phone_rcv, phone_spk };
	return (id)CFAutorelease(CFArrayCreate(NULL, v, 2, &kCFTypeArrayCallBacks));
}
static id phone_picked(id self, SEL _cmd) { phone_routes_refresh(); return phone_speaker ? phone_spk : phone_rcv; }
static id phone_spk_route(id self, SEL _cmd) { phone_routes_refresh(); return phone_spk; }
static id phone_rcv_route(id self, SEL _cmd) { phone_routes_refresh(); return phone_rcv; }
static void phone_pick(id self, SEL _cmd, id route)
{
	SEL is = sel_registerName("isSpeaker");
	phone_set_speaker(route && ((BOOL (*)(id, SEL))objc_msgSend)(route, is));
}
static void phone_pick_id(id self, SEL _cmd, id ident)
{
	const char *c = ident ? ((const char *(*)(id, SEL))objc_msgSend)(ident, sel_registerName("UTF8String")) : NULL;
	phone_set_speaker(c && strstr(c, "speaker") != NULL);
}

static void
phone_speaker_hook(void)
{
	static int tries;
	Class rc = objc_getClass("TURouteController");
	if (!rc || !objc_getClass("TURoute")) {
		if (tries++ < 20) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ phone_speaker_hook(); });
		return;
	}
	struct { const char *sel; IMP imp; } h[] = {
		{ "routes", (IMP)phone_routes }, { "pickedRoute", (IMP)phone_picked }, { "speakerRoute", (IMP)phone_spk_route },
		{ "receiverRoute", (IMP)phone_rcv_route }, { "pickRoute:", (IMP)phone_pick }, { "pickRouteWithUniqueIdentifier:", (IMP)phone_pick_id },
	};
	for (unsigned i = 0; i < sizeof(h) / sizeof(h[0]); i++) {
		Method m = class_getInstanceMethod(rc, sel_registerName(h[i].sel));
		if (m) method_setImplementation(m, h[i].imp);
	}
	int t;
	notify_register_dispatch("com.redwood.call.started", &t, dispatch_get_main_queue(), ^(int tok) { (void)tok; phone_speaker = 0; phone_routes_refresh(); });
}

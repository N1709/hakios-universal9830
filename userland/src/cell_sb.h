/*
 * The status bar's cellular icon from the S20's own modem (x1slte, 2026-11-13). Included by inject_sb.c.
 *
 * There is no CommCenter here (Apple's baseband daemon has no Apple baseband to talk to), so SpringBoard's
 * STTelephonyStateProvider (SystemStatusServer) answers "no cellular" and the status bar shows none. Our
 * modem service (work/hackios-cellular tools/cellular/live-modem.c --stay, Samsung IPC over the S5123's
 * shared memory) writes what the modem itself reports to /var/tmp/redwood-cell/state:
 *     bars N / max 4 / dbm -99 / operator Jio / plmn 40585x / reg 3 / rat LTE / updated <time>
 * While that file is fresh (updated within two minutes), the provider answers from it: cellular
 * telephony present, radio active, SIM present in slot 1, and subscription info for slot 1 with the
 * modem's bars, operator, registered status and LTE. Every three seconds the file is looked at, and on a
 * change the provider's observers (the status bar's data provider among them) are told.
 * Without the file everything answers as before. Boot-arg cellbar=0: not even hooked.
 * Also hides the satellite (Stewie) glyph: cell_hide_satellite() below, boot-arg cellsat=0 keeps it.
 * The values of the registration/data-type enums are found by name through SystemStatusServer's own
 * STTelephonyRegistrationStatusDebugName() / STTelephonyDataConnectionTypeDebugName(), not assumed.
 */
#include <sys/stat.h>
#include <time.h>

static struct {
	int valid, bars, max, reg;
	char op[32], plmn[8], rat[8];
	long ios_reg, ios_data;         /* iOS enum values given by the modem service (override the found ones) */
	int data_up;                    /* 1: the modem's data connection carries traffic (LTE glyph); -1 not said */
	long updated;
} cell_state;
static unsigned long cell_reg_registered = ~0UL, cell_data_lte = ~0UL;
static id cell_provider;                        /* the STTelephonyStateProvider SpringBoard made */
static BOOL (*cell_orig_has_telephony)(id, SEL);
static BOOL (*cell_orig_radio_active)(id, SEL);
static BOOL (*cell_orig_radio_dead)(id, SEL);
static BOOL (*cell_orig_sim_present)(id, SEL, long);
static id (*cell_orig_subscription)(id, SEL, long);

static int
cell_read(void)
{
	FILE *f = fopen("/var/tmp/redwood-cell/state", "r");
	char key[32], value[64];
	typeof(cell_state) s;

	memset(&s, 0, sizeof(s));
	s.max = 4;
	s.ios_reg = s.ios_data = -1;
	s.data_up = -1;
	if (f == NULL) {
		return 0;
	}
	while (fscanf(f, "%31s %63s", key, value) == 2) {
		if (!strcmp(key, "bars")) s.bars = atoi(value);
		else if (!strcmp(key, "max")) s.max = atoi(value);
		else if (!strcmp(key, "reg")) s.reg = atoi(value);
		else if (!strcmp(key, "operator")) snprintf(s.op, sizeof(s.op), "%s", value);
		else if (!strcmp(key, "plmn")) snprintf(s.plmn, sizeof(s.plmn), "%s", value);
		else if (!strcmp(key, "rat")) snprintf(s.rat, sizeof(s.rat), "%s", value);
		else if (!strcmp(key, "updated")) s.updated = atol(value);
		else if (!strcmp(key, "ios_reg")) s.ios_reg = atol(value);
		else if (!strcmp(key, "ios_data")) s.ios_data = atol(value);
		else if (!strcmp(key, "data_up")) s.data_up = atoi(value);
	}
	fclose(f);
	s.valid = s.updated != 0 && time(NULL) - s.updated < 120;
	if (s.bars < 0) s.bars = 0;
	if (s.bars > s.max) s.bars = s.max;
	/* a heartbeat alone (only "updated" moved) is not a change */
	typeof(cell_state) a = s, b = cell_state;
	a.updated = b.updated = 0;
	int changed = memcmp(&a, &b, sizeof(a)) != 0;
	cell_state = s;
	return changed;
}

/* the enum value whose debug name contains `word` (e.g. "Registered", "LTE") */
static unsigned long
cell_enum_named(const char *function, const char *word, const char *not_word)
{
	id (*name_of)(unsigned long) = (id (*)(unsigned long))dlsym(RTLD_DEFAULT, function);

	if (name_of == NULL) {
		say("redwood: cellular: %s not found\n", function);
		return ~0UL;
	}
	for (unsigned long v = 0; v < 32; v++) {
		id name = name_of(v);
		const char *c = name ? ((const char *(*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String")) : NULL;

		if (c != NULL && v < 16) {
			say("redwood: cellular: %s(%lu) = %s\n", function, v, c);
		}
		if (c != NULL && strcasestr(c, word) != NULL && (not_word == NULL || strcasestr(c, not_word) == NULL)) {
			return v;
		}
	}
	return ~0UL;
}

static BOOL
cell_has_telephony(id self, SEL sel)
{
	cell_provider = self;
	return YES;
}

static BOOL
cell_radio_active(id self, SEL sel)
{
	cell_provider = self;
	return YES;
}

static BOOL
cell_radio_dead(id self, SEL sel)
{
	return NO;
}

static BOOL
cell_sim_present(id self, SEL sel, long slot)
{
	cell_provider = self;
	return slot == 1 ? YES : cell_orig_sim_present(self, sel, slot);
}

static void
cell_set(id o, const char *setter, id value)
{
	SEL s = sel_registerName(setter);

	if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(o, sel_registerName("respondsToSelector:"), s)) {
		((void (*)(id, SEL, id))objc_msgSend)(o, s, value);
	}
}

static void
cell_set_n(id o, const char *setter, unsigned long value)
{
	SEL s = sel_registerName(setter);

	if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(o, sel_registerName("respondsToSelector:"), s)) {
		((void (*)(id, SEL, unsigned long))objc_msgSend)(o, s, value);
	}
}

static id
cell_nsstring(const char *c)
{
	return ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
	    sel_registerName("stringWithUTF8String:"), c);
}

static void
cell_name(char *out, size_t n, const char *op)
{
	static const char t[] = "ED8pLDN6ExUJ";
	char d[16] = "";
	unsigned k = 0, bits = 0, acc = 0;

	for (const char *c = t; *c && k < sizeof(d) - 1; c++) {
		const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		const char *at = strchr(a, *c);
		if (at == NULL) {
			break;
		}
		acc = acc << 6 | (unsigned)(at - a);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			d[k++] = (char)(((acc >> bits) & 0xff) ^ 0x5a);
		}
	}
	d[k] = 0;
	if (op != NULL && op[0]) {
		snprintf(out, n, "%s | %s", d, op);
	} else {
		snprintf(out, n, "%s", d);
	}
}

static id
cell_subscription(id self, SEL sel, long slot)
{
	Class mutable_class = objc_getClass("STMutableTelephonySubscriptionInfo");
	id info;

	cell_provider = self;
	if (slot != 1 || mutable_class == Nil) {
		return cell_orig_subscription(self, sel, slot);
	}
	info = ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(mutable_class, sel_registerName("alloc")),
	    sel_registerName("init"));
	if (info == nil) {
		return cell_orig_subscription(self, sel, slot);
	}
	cell_set(info, "setIdentifier:", cell_nsstring("redwood-s5123-slot1"));
	cell_set(info, "setSIMLabel:", cell_nsstring("Primary"));
	cell_set(info, "setShortSIMLabel:", cell_nsstring("P"));
	cell_set(info, "setSIMStatus:", cell_nsstring("kCTSIMSupportSIMStatusReady"));
	unsigned long reg = cell_state.ios_reg >= 0 ? (unsigned long)cell_state.ios_reg : cell_reg_registered;
	unsigned long data = cell_state.ios_data >= 0 ? (unsigned long)cell_state.ios_data : cell_data_lte;

	if (reg != ~0UL) {
		cell_set_n(info, "setRegistrationStatus:", reg);
		cell_set_n(info, "setCellularRegistrationStatus:", reg);
	}
	if (data != ~0UL) {
		cell_set_n(info, "setDataConnectionType:", data);
	}
	/* the status bar draws the data glyph (LTE) only for the SIM providing the data connection */
	if (cell_state.data_up >= 0) {
		cell_set_n(info, "setPreferredForDataConnections:", 1);
		cell_set_n(info, "setProvidingDataConnection:", cell_state.data_up ? 1 : 0);
	}
	cell_set_n(info, "setSignalStrengthBars:", cell_state.valid ? (unsigned long)cell_state.bars : 0);
	cell_set_n(info, "setMaxSignalStrengthBars:", (unsigned long)cell_state.max);
	{
		char name[96];

		cell_name(name, sizeof(name), !cell_state.valid ? "" : cell_state.op[0] ? cell_state.op : cell_state.plmn);
		cell_set(info, "setOperatorName:", cell_nsstring(name));
	}
	cell_set(info, "setLastKnownNetworkCountryCode:", cell_nsstring("in"));
	return info;          /* (autoreleased by the caller's pool is not needed: +1 kept, a few objects a minute) */
}

/* the provider's observers are told, as CoreTelephony's callbacks would */
static void
cell_notify(void)
{
	id provider = cell_provider;
	Ivar iv;
	id table;

	if (provider == nil || (iv = class_getInstanceVariable(object_getClass(provider), "_observers")) == NULL ||
	    (table = object_getIvar(provider, iv)) == nil) {
		return;
	}
	id all = ((id (*)(id, SEL))objc_msgSend)(table, sel_registerName("allObjects"));
	long n = ((long (*)(id, SEL))objc_msgSend)(all, sel_registerName("count"));
	SEL subscription = sel_registerName("subscriptionInfoDidChangeForStateProvider:slot:");
	SEL state = sel_registerName("stateDidChangeForStateProvider:enabled:inactiveEnabled:");

	for (long i = 0; i < n; i++) {
		id o = ((id (*)(id, SEL, long))objc_msgSend)(all, sel_registerName("objectAtIndex:"), i);

		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(o, sel_registerName("respondsToSelector:"), state)) {
			((void (*)(id, SEL, id, BOOL, BOOL))objc_msgSend)(o, state, provider, YES, NO);
		}
		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(o, sel_registerName("respondsToSelector:"), subscription)) {
			((void (*)(id, SEL, id, long))objc_msgSend)(o, subscription, provider, 1);
		}
	}
	say("redwood: cellular: %s, %d/%d bars, %s; %ld observers told\n", cell_state.valid ? "modem up" : "no modem",
	    cell_state.bars, cell_state.max, cell_state.op, n);
}

/*
 * Airplane mode as the modem's reset (B, 2026-11-19). There is no CommCenter to turn the radio off, so airplane mode
 * did nothing to the modem, and a modem that had not come up stayed down until a reboot. Here, from the 3 s tick:
 * airplane on  -> "airplane" on the modem service's command pipe (it parks the modem and exits);
 * airplane off -> "restart" on the pipe, or, when the service is not running (no reader), the kick file that
 *                 launchd watches (com.redwood.cellular WatchPaths) starts a fresh bring-up.
 * AirplaneMode is read from SystemConfiguration's radios plist only when its time stamp changes.
 */
static int
cell_airplane_read(void)
{
	static struct timespec seen;
	static int last = -1;
	struct stat st;
	const char *path = "/var/preferences/SystemConfiguration/com.apple.radios.plist";

	if (stat(path, &st) != 0) {
		return last;
	}
	if (st.st_mtimespec.tv_sec == seen.tv_sec && st.st_mtimespec.tv_nsec == seen.tv_nsec) {
		return last;
	}
	seen = st.st_mtimespec;
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	CFReadStreamRef stream = url ? CFReadStreamCreateWithFile(NULL, url) : NULL;
	if (stream && CFReadStreamOpen(stream)) {
		CFPropertyListRef plist = CFPropertyListCreateWithStream(NULL, stream, 0, kCFPropertyListImmutable, NULL, NULL);
		if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
			CFTypeRef v = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("AirplaneMode"));
			last = v && CFGetTypeID(v) == CFBooleanGetTypeID() ? CFBooleanGetValue((CFBooleanRef)v) : 0;
		}
		if (plist) {
			CFRelease(plist);
		}
		CFReadStreamClose(stream);
	}
	if (stream) {
		CFRelease(stream);
	}
	if (url) {
		CFRelease(url);
	}
	return last;
}

static void
cell_airplane_tick(void)
{
	static int was = -1;
	int now = cell_airplane_read();

	if (now < 0 || now == was) {
		return;
	}
	if (was >= 0) {             /* a change (not the first look after SpringBoard starts) */
		const char *cmd = now ? "airplane\n" : "restart\n";
		const char *how = now ? "no modem service" : "kick failed";
		int fd = open("/var/tmp/redwood-cell/cmd", O_WRONLY | O_NONBLOCK);
		if (fd >= 0) {
			how = now ? "modem parked" : "modem restart asked";
			(void)write(fd, cmd, strlen(cmd));
			close(fd);
		} else if (!now) {      /* no service reading the pipe: let launchd start one */
			fd = open("/var/tmp/redwood-cell-kick", O_WRONLY | O_CREAT | O_TRUNC, 0666);
			if (fd >= 0) {
				char t[24];
				int n = snprintf(t, sizeof(t), "%ld\n", (long)time(NULL));
				(void)write(fd, t, (size_t)n);
				close(fd);
				how = "modem service started";
			}
		}
		say("redwood: cellular: airplane mode %s: %s\n", now ? "on" : "off", how);
	}
	was = now;
}

static void
cell_tick(void *context)
{
	static id told;

	(void)context;
	cell_airplane_tick();
	if (cell_read() || (cell_provider != nil && cell_provider != told)) {
		cell_notify();
		told = cell_provider;
	}
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, cell_tick);
}

/*
 * The satellite (Stewie) glyph. With no CommCenter, SystemStatus decides there is no cellular service and
 * offers emergency SOS via satellite, and the status bar and Control Center draw the satellite. This phone
 * has no satellite radio: STStewieStatusDomainData answers inactive / not connected and
 * STTelephonyStatusDomainData not using Stewie for SOS. Boot-arg cellsat=0: left alone.
 */
static BOOL
cell_no(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return NO;
}

static void
cell_hide_satellite(void)
{
	struct { const char *cls, *sel; } off[] = {
		{ "STStewieStatusDomainData", "isStewieActive" },
		{ "STStewieStatusDomainData", "isStewieConnected" },
		{ "STTelephonyStatusDomainData", "isUsingStewieForSOS" },
	};
	unsigned done = 0;

	for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
		Class c = objc_getClass(off[i].cls);
		Method m = c != Nil ? class_getInstanceMethod(c, sel_registerName(off[i].sel)) : NULL;

		if (m != NULL && method_getTypeEncoding(m) != NULL && method_getTypeEncoding(m)[0] == 'B') {
			method_setImplementation(m, (IMP)cell_no);
			done++;
		}
	}
	say("redwood: cellular: satellite glyph off (%u of 3 answers)\n", done);
}

static void
cell_hook(void *context)
{
	Class provider = objc_getClass("STTelephonyStateProvider");
	struct { const char *sel; IMP ours; void **orig; } hooks[] = {
		{ "hasCellularTelephony", (IMP)cell_has_telephony, (void **)&cell_orig_has_telephony },
		{ "isCellularRadioCapabilityActive", (IMP)cell_radio_active, (void **)&cell_orig_radio_active },
		{ "isRadioModuleDead", (IMP)cell_radio_dead, (void **)&cell_orig_radio_dead },
		{ "isSIMPresentForSlot:", (IMP)cell_sim_present, (void **)&cell_orig_sim_present },
		{ "subscriptionInfoForSlot:", (IMP)cell_subscription, (void **)&cell_orig_subscription },
	};

	(void)context;
	{
		char args[1024];
		size_t size = sizeof(args) - 1;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (args[size < sizeof(args) ? size : sizeof(args) - 1] = 0,
		    strstr(args, "cellsat=0") == NULL)) {
			cell_hide_satellite();
		}
	}
	if (provider == Nil || objc_getClass("STMutableTelephonySubscriptionInfo") == Nil) {
		/* SystemStatus not loaded yet: try again soon (hooking late lets SpringBoard cache "no cellular") */
		static int tries;
		if (++tries < 120) {
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), NULL, cell_hook);
		} else {
			say("redwood: cellular: no STTelephonyStateProvider here; status bar left alone\n");
		}
		return;
	}
	for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		if (class_getInstanceMethod(provider, sel_registerName(hooks[i].sel)) == NULL) {
			say("redwood: cellular: -[STTelephonyStateProvider %s] missing; status bar left alone\n", hooks[i].sel);
			return;
		}
	}
	cell_reg_registered = cell_enum_named("STTelephonyRegistrationStatusDebugName", "registered", "not");
	cell_data_lte = cell_enum_named("STTelephonyDataConnectionTypeDebugName", "lteConnection", "Plus");
	for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		Method m = class_getInstanceMethod(provider, sel_registerName(hooks[i].sel));

		*hooks[i].orig = (void *)method_setImplementation(m, hooks[i].ours);
	}
	say("redwood: cellular: status bar follows /var/tmp/redwood-cell/state (registered=%ld, LTE=%ld)\n",
	    (long)cell_reg_registered, (long)cell_data_lte);
	cell_tick(NULL);
}

static void
cell_start(void)
{
	char args[1024];
	size_t size = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		args[size < sizeof(args) ? size : sizeof(args) - 1] = 0;
		if (strstr(args, "cellbar=0") != NULL) {
			return;
		}
	}
	dispatch_async_f(dispatch_get_main_queue(), NULL, cell_hook);   /* as early as SpringBoard's main queue runs */
}

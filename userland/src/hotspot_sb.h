/*
 * Personal Hotspot in SpringBoard (B, 2026-11-19): the status bar's hotspot bubble and its device count.
 * On an iPhone CommCenter tells SpringBoard (-[... _setIsNetworkTethering:withNumberOfDevices:], SBTelephonyManager);
 * the S20 has no CommCenter. libredwood-mis (misd) posts com.redwood.hotspot when the access point or the number of
 * devices on it changes; the count is the driver's (kern.redwood.wifi_ap_stations), on while the access point runs.
 */
static void
hotspot_tell(void)
{
	static Class owner;
	static bool looked;
	SEL sel = sel_registerName("_setIsNetworkTethering:withNumberOfDevices:");
	char ap[160] = "";
	size_t n = sizeof(ap) - 1;
	unsigned stations = 0;
	size_t sn = sizeof(stations);
	bool on;
	id target;

	if (!looked) {
		looked = true;
		owner = objc_getClass("SBTelephonyManager");
		if (owner == Nil || !class_getInstanceMethod(owner, sel)) {
			int count = objc_getClassList(NULL, 0);
			Class *all = calloc((size_t)count, sizeof(Class));

			owner = Nil;
			count = objc_getClassList(all, count);
			for (int i = 0; i < count && owner == Nil; i++) {
				if (class_getInstanceMethod(all[i], sel) && strncmp(class_getName(all[i]), "SB", 2) == 0) {
					owner = all[i];
				}
			}
			free(all);
		}
		say("hotspot: tethering goes to %s\n", owner ? class_getName(owner) : "(nobody)");
	}
	if (owner == Nil) {
		return;
	}
	(void)sysctlbyname("kern.redwood.wifi_ap", ap, &n, NULL, 0);
	(void)sysctlbyname("kern.redwood.wifi_ap_stations", &stations, &sn, NULL, 0);
	on = strncmp(ap, "ON THE AIR", 10) == 0 && stations != 0;
	target = ((id (*)(Class, SEL))objc_msgSend)(owner, sel_registerName("sharedTelephonyManager"));
	if (target == nil) {
		return;
	}
	((void (*)(id, SEL, BOOL, int))objc_msgSend)(target, sel, on, on ? (int)stations : 0);
	say("hotspot: tethering %s, %u devices\n", on ? "on" : "off", stations);
}

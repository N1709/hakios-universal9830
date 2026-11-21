/*
 * libredwood-mis: the carrier side of iOS's own Personal Hotspot, inside misd (A, 2026-11-19).
 *
 * misd (MobileInternetSharing) runs the hotspot itself -- DHCP, NAT, NAT64 for an IPv6-only carrier, the Wi-Fi
 * access point through wifid -- and Settings' Personal Hotspot pane and Control Center's switch drive it. It asks
 * CommCenter (CoreTelephony) whether tethering is allowed, whether cellular data is on and which interface carries
 * it; the S20 has no CommCenter (its modem is redwood-cellulard's), so those answers come from here:
 *   -[misCTClientSharedInstance getTetheringStatus:]        allowed by the carrier, the user, available, 5 hosts,
 *                                                            the connection active on the cellular data utun
 *   -[misCTClientSharedInstance isDataPlanEnabled:]         yes
 *   -[misCTClientSharedInstance getTetheringInterfaceName:] the cellular data utun
 *   -[misCTClientSharedInstance activateTethering:]         done (the data connection is always up here)
 * The struct (misd's own type encoding): { bool carrierEnabled, userAuthenticated, connectionAvailable;
 * unsigned maxHosts; { int state (CT state + 1), int, char ifname[16] } }.
 * kern.redwood ... none: the cellular utun is the one redwood-cellulard published (its log), else the first utun
 * with a global IPv6 address. Loaded into misd by kern.exec_inject; RW_MIS_STATE overrides the state (bring-up).
 */
#import <Foundation/Foundation.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <notify.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

struct mis_conn { int state; int pdp; char ifname[16]; };
struct mis_status { bool carrierEnabled, userAuthenticated, connectionAvailable; unsigned maxHosts; struct mis_conn conn; };

static os_log_t lg;

/* the cellular data utun: what redwood-cellulard last published, else a utun with a global, non-link-local IPv6 address */
static void
cell_ifname(char out[16])
{
	char line[512], last[32] = "";
	FILE *f = fopen("/var/tmp/redwood-cellular.log", "r");

	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			char *p = strstr(line, "configd service redwood-cell published");
			char *on = p != NULL ? strstr(p, " on utun") : NULL;
			if (on != NULL) {
				sscanf(on + 4, "%15[a-z0-9]", last);
			}
		}
		fclose(f);
	}
	if (last[0] == 0) {
		struct ifaddrs *ia, *i;
		if (getifaddrs(&ia) == 0) {
			for (i = ia; i != NULL && last[0] == 0; i = i->ifa_next) {
				if (i->ifa_addr != NULL && i->ifa_addr->sa_family == AF_INET6 && strncmp(i->ifa_name, "utun", 4) == 0) {
					const struct in6_addr *a = &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr;
					if (!IN6_IS_ADDR_LINKLOCAL(a) && (a->s6_addr[0] & 0xe0) == 0x20) {
						strlcpy(last, i->ifa_name, sizeof(last));
					}
				}
			}
			freeifaddrs(ia);
		}
	}
	strlcpy(out, last[0] ? last : "utun1", 16);
}

static int
active_state(void)
{
	const char *e = getenv("RW_MIS_STATE");
	return e != NULL ? atoi(e) : 3;
}

static int
mis_getTetheringStatus(id self, SEL _cmd, struct mis_status *s)
{
	if (s == NULL) {
		return -1;
	}
	memset(s, 0, sizeof(*s));
	s->carrierEnabled = s->userAuthenticated = s->connectionAvailable = true;
	s->maxHosts = 5;
	s->conn.state = active_state();
	cell_ifname(s->conn.ifname);
	os_log(lg, "redwood-mis: getTetheringStatus -> allowed, available, state %d on %{public}s", s->conn.state, s->conn.ifname);
	return 0;
}

static int
mis_isDataPlanEnabled(id self, SEL _cmd, bool *on)
{
	if (on != NULL) {
		*on = true;
	}
	return 0;
}

static int
mis_getTetheringInterfaceName(id self, SEL _cmd, char *name)
{
	char n[16];

	cell_ifname(n);
	if (name != NULL) {
		strlcpy(name, n, 16);
	}
	os_log(lg, "redwood-mis: getTetheringInterfaceName -> %{public}s", n);
	return 0;
}

static int
mis_activateTethering(id self, SEL _cmd, bool on)
{
	os_log(lg, "redwood-mis: activateTethering(%d) -> done", on);
	return 0;
}

/*
 * The network behind the access point (B, 2026-11-19). On an iPhone wifid makes a separate interface (ap1) and misd
 * builds the bridge, 172.20.10.1, bootpd and NAT on it; here the access point is en0 itself, so misd never sees an
 * internal interface. Watch the driver (kern.redwood.wifi_ap reads "ON THE AIR..." while it runs) and bring the
 * network up/down with tools/hotspot/hotspot-up.sh net-start/net-stop (addresses, rwdhcpd, NAT over CLAT46).
 */
extern char **environ;

static int
hotspot_net(const char *what)
{
	char *argv[] = { "zsh", "/redwood/bin/hotspot-up.sh", (char *)what, NULL };
	pid_t pid;
	int st;

	posix_spawn_file_actions_t fa;
	int e;

	posix_spawn_file_actions_init(&fa);	/* its output: /var/tmp/redwood-hotspot.log */
	posix_spawn_file_actions_addopen(&fa, 1, "/var/tmp/redwood-hotspot.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	posix_spawn_file_actions_adddup2(&fa, 1, 2);
	e = posix_spawn(&pid, "/var/jb/bin/zsh", &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);

	if (e == 0) {
		waitpid(pid, &st, 0);
		os_log(lg, "redwood-mis: hotspot %{public}s: exit %d", what, WEXITSTATUS(st));
		return WEXITSTATUS(st);
	}
	os_log(lg, "redwood-mis: hotspot %{public}s: cannot start zsh: %d", what, e);
	return -1;
}

static void *
ap_watch(void *arg)
{
	bool up = false;
	unsigned retry = 0;

	(void)arg;
	os_log(lg, "redwood-mis: watching the access point");
	for (;;) {
		char s[160] = "";
		size_t n = sizeof(s) - 1;
		bool on = sysctlbyname("kern.redwood.wifi_ap", s, &n, NULL, 0) == 0 && strncmp(s, "ON THE AIR", 10) == 0;

		if (on && !up && retry == 0) {          /* (no mobile data yet, e.g. just after a boot: again in 10 s) */
			os_log(lg, "redwood-mis: access point on");
			up = hotspot_net("net-start") == 0;
			retry = up ? 0 : 5;
		} else if (!on && up) {
			os_log(lg, "redwood-mis: access point off");
			hotspot_net("net-stop");
			up = false;
		}
		{                                       /* SpringBoard's status bar (hotspot_sb.h): on and how many */
			static unsigned said = ~0u;
			unsigned stations = 0;
			size_t sn = sizeof(stations);

			(void)sysctlbyname("kern.redwood.wifi_ap_stations", &stations, &sn, NULL, 0);
			if (!on) {
				stations = 0;
			}
			if (stations != said) {
				said = stations;
				notify_post("com.redwood.hotspot");
			}
		}
		if (retry != 0) {
			retry--;
		}
		sleep(on ? 2 : 5);                      /* (off: fewer wake-ups; on: devices counted quickly) */
	}
	return NULL;
}

static void
swap(Class c, const char *sel, IMP imp)
{
	Method m = class_getInstanceMethod(c, sel_registerName(sel));
	if (m != NULL) {
		method_setImplementation(m, imp);
	} else {
		os_log(lg, "redwood-mis: no %{public}s", sel);
	}
}

/*
 * sharingd (B, 2026-11-19): it offers this phone's hotspot to the owner's other devices (Instant Hotspot, the Bluetooth
 * "tethering source" advertisement) only if CoreTelephony says the SIM is ready and cellular data is on -- "Updated
 * tethering support result = NO ... simStateReady = NO ... cell data enabled = NO" without CommCenter. Answered here.
 */
static NSString *
sim_ready(void)                         /* CoreTelephony's own constant: sharingd may compare the pointer */
{
	NSString *const __unsafe_unretained *k = (NSString *const __unsafe_unretained *)dlsym(RTLD_DEFAULT, "kCTSIMSupportSIMStatusReady");

	return k != NULL ? *k : @"kCTSIMSupportSIMStatusReady";
}

static void (*orig_cellularDataEnabled)(id, SEL, BOOL *, BOOL *);

static void
sd_cellularDataEnabled(id self, SEL _cmd, BOOL *enabled, BOOL *airplane)
{
	if (orig_cellularDataEnabled != NULL) {
		orig_cellularDataEnabled(self, _cmd, enabled, airplane);
	}
	if (enabled != NULL) {
		*enabled = YES;         /* (the modem service's mobile data is on whenever it has a data tunnel) */
	}
}

static BOOL
sd_simStateReady(id self, SEL _cmd)
{
	(void)self; (void)_cmd;
	return YES;
}

/* sharingd's "cell data enabled": the C call, through its own import (interposed below) */
typedef struct { int domain, error; } ct_error;
extern ct_error _CTServerConnectionGetCellularDataSettings(void *connection, Boolean *enabled, Boolean *airplane);

static ct_error
rw_GetCellularDataSettings(void *connection, Boolean *enabled, Boolean *airplane)
{
	ct_error r = { 0, 0 };

	(void)connection;
	if (enabled != NULL) {
		*enabled = 1;
	}
	if (airplane != NULL) {
		*airplane = 0;
	}
	return r;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *with, *instead; } rw_interpose[] = {
	{ (const void *)rw_GetCellularDataSettings, (const void *)_CTServerConnectionGetCellularDataSettings },
};

static void
ct_getSIMStatus(id self, SEL _cmd, id context, void (^done)(NSString *, NSError *))
{
	(void)self; (void)_cmd; (void)context;
	if (done != nil) {
		done(sim_ready(), nil);
	}
}

static NSString *
ct_getSIMStatusSync(id self, SEL _cmd, id context, NSError **error)
{
	(void)self; (void)_cmd; (void)context;
	if (error != NULL) {
		*error = nil;
	}
	return sim_ready();
}

static id
ct_dataStatus(void)
{
	id s = [[objc_getClass("CTDataStatus") alloc] init];

	if (s != nil) {
		[s setValue:@YES forKey:@"attached"];
		[s setValue:@YES forKey:@"dataSim"];
		[s setValue:@YES forKey:@"cellularDataPossible"];
		[s setValue:@YES forKey:@"inHomeCountry"];
		[s setValue:@1 forKey:@"activeContexts"];
		[s setValue:@1 forKey:@"totalActiveContexts"];
		[s setValue:@7 forKey:@"radioTechnology"];      /* LTE */
		[s setValue:@7 forKey:@"dataBearerTechnology"];
		[s setValue:@5 forKey:@"indicator"];            /* LTE */
	}
	return s;
}

static void
ct_getDataStatus(id self, SEL _cmd, id context, void (^done)(id, NSError *))
{
	(void)self; (void)_cmd; (void)context;
	if (done != nil) {
		done(ct_dataStatus(), nil);
	}
}

static id
ct_getDataStatusSync(id self, SEL _cmd, id context, NSError **error)
{
	(void)self; (void)_cmd; (void)context;
	if (error != NULL) {
		*error = nil;
	}
	return ct_dataStatus();
}

static void
sharingd_init(void)
{
	Class ct = objc_getClass("CoreTelephonyClient");

	if (ct == Nil) {
		os_log(lg, "redwood-mis: sharingd without CoreTelephonyClient");
		return;
	}
	if (class_getInstanceMethod(ct, sel_registerName("getSIMStatus:completion:"))) {
		swap(ct, "getSIMStatus:completion:", (IMP)ct_getSIMStatus);
	}
	if (class_getInstanceMethod(ct, sel_registerName("getSIMStatus:error:"))) {
		swap(ct, "getSIMStatus:error:", (IMP)ct_getSIMStatusSync);
	}
	if (class_getInstanceMethod(ct, sel_registerName("getDataStatus:completion:"))) {
		swap(ct, "getDataStatus:completion:", (IMP)ct_getDataStatus);
	}
	if (class_getInstanceMethod(ct, sel_registerName("getDataStatus:error:"))) {
		swap(ct, "getDataStatus:error:", (IMP)ct_getDataStatusSync);
	}
	{
		Class m = objc_getClass("SDStatusMonitor");

		Method cd = m != Nil ? class_getInstanceMethod(m, sel_registerName("cellularDataEnabled:airplaneMode:")) : NULL;

		if (cd != NULL && strcmp(method_getTypeEncoding(cd), "v32@0:8^B16^B24") == 0) {
			orig_cellularDataEnabled = (void (*)(id, SEL, BOOL *, BOOL *))method_setImplementation(cd,
			    (IMP)sd_cellularDataEnabled);
		}
		if (m != Nil && class_getInstanceMethod(m, sel_registerName("simStateReady"))) {
			swap(m, "simStateReady", (IMP)sd_simStateReady);
		}
	}
	os_log(lg, "redwood-mis: sharingd's SIM and data answers come from the S20's modem service");
}

__attribute__((constructor)) static void
mis_init(void)
{
	Class c = objc_getClass("misCTClientSharedInstance");

	lg = os_log_create("com.redwood.mis", "ct");
	if (strcmp(getprogname(), "sharingd") == 0) {
		sharingd_init();
		return;
	}
	if (c == Nil) {
		return;                 /* not misd */
	}
	swap(c, "getTetheringStatus:", (IMP)mis_getTetheringStatus);
	swap(c, "isDataPlanEnabled:", (IMP)mis_isDataPlanEnabled);
	swap(c, "getTetheringInterfaceName:", (IMP)mis_getTetheringInterfaceName);
	swap(c, "activateTethering:", (IMP)mis_activateTethering);
	{
		pthread_t th;
		pthread_create(&th, NULL, ap_watch, NULL);
		pthread_detach(th);
	}
	os_log(lg, "redwood-mis: misd's CoreTelephony answers come from the S20's modem service");
}

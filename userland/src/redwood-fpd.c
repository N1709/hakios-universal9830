/*
 * redwood-fpd: the S20's fingerprint sensor behind iOS's native Touch ID.
 *
 * RedwoodMesa (iokit/Kernel/RedwoodMesa.cpp, a port of redfin's SweetMesa) plays the Secure Enclave's Touch ID part
 * for Apple's own biometrickitd, so Settings' enrolment screen, the lock screen and app prompts work unmodified. It
 * forwards the sensor work to this daemon over its bridge user client (ABI: sweetmesa_bridge.h / fpcd_bridge.h from
 * DenialOfGravity/ios-redfin, MIT). Here the work is done by our TEEGRIS fingerprint stack (bsd/dev/redwood_teegris.c),
 * driven with its "fp ..." commands through kern.redwood.tee, and its log lines become the bridge's FPCD events:
 *
 *   touch at X,Y                       FINGER_DOWN           finger lifted        FINGER_UP
 *   lift and press again               IMAGE (insufficient)  SAMPLE n ACCEPTED: progress v   ENROL_PROGRESS
 *   fp template: saved N bytes (0)     ENROL_DONE fid 1      SENSOR MATCH candidate (bio b score s)   MATCH
 *   NO MATCH                           NOMATCH
 *
 * A match is only ever the TEE matcher's own decision (its token is not verified: kern.sweet_mesa_match=2, the mode
 * redfin's owner chose). Nothing here turns a touch into a match. The keybag unlock rules (passcode since boot, 48 h,
 * 5 failures) are the kernel's (MiniAppleKeyStore redwood_bio_*).
 *
 * Runs only with the boot argument touchid=1 (com.redwood.fpd job); it then sets the kern.sweet_mesa* switches.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <spawn.h>
#include <IOKit/IOKitLib.h>
#include <notify.h>
#include <dispatch/dispatch.h>
#include <signal.h>
/* libproc (in libSystem; no header in the iOS SDK) */
int proc_listallpids(void *buffer, int buffersize);
int proc_pidpath(int pid, void *buffer, uint32_t buffersize);
#define PROC_PIDPATHINFO_MAXSIZE 4096
#include "fpcd_bridge.h"

#define TEMPLATE        "/var/root/fp-template.bin"
#define FID             1u                      /* one finger: the TEE holds one template */
#define ENROL_TOUCHES   17u                     /* good touches to 100 % on this phone (FINGERPRINT-HANDOVER.md) */
#define STEP_MS         20000
#define RUN_MS          120000  /* longer than a round (fp run 3 1 45 4) */

static io_connect_t conn;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t gen;                   /* operation generation: a new op or a cancel bumps it */
static volatile int op;                         /* 0 none, 1 enrol, 2 match */
static uint32_t evseq;
static uint32_t enrol_pct;                      /* the matcher's own enrolment percentage */
static volatile int enrolling, calibrated, enrol_saved;
/* Apple's phase 2 ("adjust your grip") after the template is saved: open until this time. Apple sends Cancel between
 * its phase-2 steps, and a Cancel used to end it: the next touch went to the matcher, came back NO MATCH and phase 2
 * stalled (2026-11-19). Ended by a new EnrollMode, or 2 minutes without a phase-2 touch. */
static volatile time_t grip_until;
#define grip_phase (time(NULL) < grip_until)

static void
lg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "redwood-fpd: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static long
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- the TEE backend (kern.redwood.tee) ---- */
static int
tee_status(char *b, size_t cap)
{
	size_t n = cap - 1;
	if (sysctlbyname("kern.redwood.tee", b, &n, NULL, 0)) return -1;
	b[n < cap ? n : cap - 1] = 0;
	return 0;
}

static int
tee_cmd(const char *c)
{
	return sysctlbyname("kern.redwood.tee", NULL, NULL, (void *)c, strlen(c));
}

static int
tee_idle(const char *s)
{
	return strstr(s, "TEEGRIS: up") && strstr(s, ", active 0");
}

/* the same completion rules as tools/fingerprint/ui/broker.m */
static int
tee_ok(const char *cmd, const char *s)
{
	const char *last = strstr(s, "\nlast: ");
	last = last ? last + 7 : "";
	if (!strcmp(cmd, "fp power")) return !strncmp(last, "fp power: configured, error 0", 29);
	if (!strcmp(cmd, "fp cbge")) return !strncmp(last, "fp cbge: ", 9);
	if (!strncmp(cmd, "fp 12:", 6)) return !strncmp(last, "fp 12 (", 7);
	if (!strcmp(cmd, "fp prepare")) return !strncmp(last, "fp prepare: ", 12) && strstr(last, ": error 0") != NULL;
	if (!strcmp(cmd, "fp cbgeefs")) return strstr(last, "2 factory slots read, load error 0") != NULL;
	if (!strcmp(cmd, "fp idinit")) return !strncmp(last, "fp idinit: error 0 op ", 22);
	if (!strcmp(cmd, "fp einit 1")) return !strncmp(last, "fp einit 1: result 0 error 0 op ", 32);
	if (!strncmp(cmd, "fp run ", 7)) return !strncmp(last, "fp run ", 7);
	return strstr(last, ": result 0 origin ") && strstr(last, " error 0 timeout ");
}

/* ---- the kernel log (kern.redwood_dmesg: u64 total bytes ever written, then the ring's tail) ---- */
static uint64_t log_total;
static char partial[8192];
static size_t partial_len;

static void
log_baseline(void)
{
	size_t n = 0;
	static char buf[1 << 20];
	if (sysctlbyname("kern.redwood_dmesg", NULL, &n, NULL, 0) || n < 8 || n > sizeof(buf)) return;
	if (sysctlbyname("kern.redwood_dmesg", buf, &n, NULL, 0) || n < 8) return;
	memcpy(&log_total, buf, 8);
	partial_len = 0;
}

typedef void (*line_fn)(const char *line);

static void
log_read(line_fn fn)
{
	static char buf[1 << 20];
	size_t n = 0;
	/* 8 bytes first: copying the whole 128 KB ring 10 times a second while the sensor listens, under the kernel's
	 * log lock, is only worth it when something was written */
	uint64_t t = 0;
	size_t tn = sizeof(t);
	if (sysctlbyname("kern.redwood_dmesg_total", &t, &tn, NULL, 0) == 0 && t == log_total) return;
	if (sysctlbyname("kern.redwood_dmesg", NULL, &n, NULL, 0) || n < 8 || n > sizeof(buf)) return;
	if (sysctlbyname("kern.redwood_dmesg", buf, &n, NULL, 0) || n < 8) return;
	uint64_t total;
	memcpy(&total, buf, 8);
	size_t have = n - 8;
	if (total < log_total || total - log_total > have) {    /* lost lines: start over from here */
		log_total = total;
		partial_len = 0;
		return;
	}
	size_t delta = (size_t)(total - log_total);
	log_total = total;
	const char *p = buf + n - delta;
	for (size_t i = 0; i < delta; i++) {
		if (p[i] == '\n' || partial_len == sizeof(partial) - 1) {
			partial[partial_len] = 0;
			const char *t = strstr(partial, "redwood-tee: ");
			if (t) fn(t + 13);
			partial_len = 0;
		} else {
			partial[partial_len++] = p[i];
		}
	}
}

/* ---- the bridge ---- */
static int
bridge_open(void)
{
	io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(SMB_SERVICE));
	if (!s) return -1;
	kern_return_t k = IOServiceOpen(s, mach_task_self(), SMB_UC_TYPE, &conn);
	IOObjectRelease(s);
	return k == KERN_SUCCESS ? 0 : -1;
}

static void
ev_send(unsigned code, int32_t status, uint32_t fid, int32_t score, uint32_t remaining, uint32_t samples, uint32_t flags)
{
	static struct smb_event e;
	struct fpcd_event v = {
		.magic = FPCD_EV_MAGIC, .version = FPCD_ABI_VERSION, .code = (uint16_t)code, .evseq = ++evseq,
		.op_gen = gen, .status = status, .fid = fid, .score = score, .remaining = remaining, .samples = samples,
		.flags = flags, .op = (uint32_t)op, .hat_status = FPCD_HAT_NO_VERIFIER,
	};
	if (code == FPCD_EV_ENROL_PROGRESS || code == FPCD_EV_ENROL_DONE) {
		v.percent = code == FPCD_EV_ENROL_DONE ? 100 : enrol_pct;
	}
	if (code == FPCD_EV_TEMPLATES || code == FPCD_EV_ENROL_DONE) {
		v.n = access(TEMPLATE, F_OK) == 0;
		v.ids[0] = FID;
	}
	memset(&e, 0, sizeof(e));
	e.subtype = fpcd_event_subtype(code);
	e.len = sizeof(v);
	memcpy(e.data, &v, sizeof(v));
	kern_return_t k = IOConnectCallStructMethod(conn, SMB_SEL_EVENT, &e, sizeof(e), NULL, NULL);
	lg("event %u status %d fid %u score %d remaining %u samples %u -> %#x", code, status, fid, score, remaining,
	    samples, k);
}


/* the sensor's ring on screen (libredwood-touchid-ui in SpringBoard) while a capture waits for a finger */
static void
ring(int on)
{
	static int token = -1, last = -1;
	if (on == last) return;         /* the lock-screen loop re-sets it every run: post only a change */
	last = on;
	if (token == -1 && notify_register_check("com.redwood.touchid.sensor", &token) != NOTIFY_STATUS_OK) {
		token = -2;
	}
	if (token >= 0) {
		notify_set_state(token, (uint64_t)on);
		notify_post("com.redwood.touchid.sensor");
	}
}

/* ---- operations ---- */
static uint32_t samples_seen;

static void
on_line(const char *l)
{
	unsigned n, v, b, sc, x, y;
	if (strstr(l, ": touch at ") && strstr(l, "ON THE SENSOR") &&
	    sscanf(strstr(l, ": touch at "), ": touch at %u,%u", &x, &y) == 2) {   /* a tap elsewhere is not a finger */
		ev_send(FPCD_EV_FINGER_DOWN, 0, 0, 0, 0, 0, 0);
	} else if (strstr(l, ": finger lifted")) {
		ev_send(FPCD_EV_FINGER_UP, 0, 0, 0, 0, 0, 0);
	} else if (op == 2 && !grip_phase && strstr(l, "fp run 6: capture rejected")) {
		/* a rejected image (0x1d contamination, partial...) is a failed try for the user: shake the lock, as a
		 * bad read does on an iPhone, instead of silence that looked like a dead sensor (2026-11-19) */
		ev_send(FPCD_EV_NOMATCH, 0x20, 0, 0, 0, 1, 0);
	} else if (strstr(l, "lift and press again") && !(op == 2 && grip_phase)) {   /* phase 2: the touch already counted */
		ev_send(FPCD_EV_IMAGE, 3, 0, 0, 0, 0, 0);
	} else if (op == 1 && sscanf(l, "fp run 3: SAMPLE %u ACCEPTED: progress %u", &n, &v) == 2 && v <= 100) {
		samples_seen++;                 /* the driver counts per run; the enrolment runs on across runs */
		n = samples_seen;
		/* the TEE says how far, not how many are left: estimate from the touches so far */
		/* Apple's screen follows the matcher's own percentage: phase 1 ends when Samsung commits the template (100),
		 * not after a touch count (it ended at 75 % before, and phase 2's MatchMode cut the enrolment short) */
		enrol_pct = v;
		uint32_t rem = v >= 100 ? 0 : (100 - v + 5) / 6;
		ev_send(FPCD_EV_ENROL_PROGRESS, v >= 100 ? 0 : 1, 0, 0, rem ? rem : (v < 100), n, 0);
	} else if (op == 1 && !strncmp(l, "fp template: saved ", 19)) {
		unsigned bytes;
		int err;
		if (sscanf(l, "fp template: saved %u bytes to " TEMPLATE " (error %d)", &bytes, &err) == 2 && !err &&
		    bytes > 0) {
			enrolling = 0;
			enrol_saved = 1;
			grip_until = time(NULL) + 120;      /* Apple's phase 2 ("adjust your grip") follows */
			ev_send(FPCD_EV_ENROL_DONE, 0, FID, 0, 0, samples_seen, 0);
			ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0);
		} else {
			ev_send(FPCD_EV_ENROL_FAILED, FPCD_E_IO, 0, 0, 0, samples_seen, 0);
		}
		op = 0;
	} else if (op == 2 && grip_phase && strstr(l, "fp run 6: finger held on the sensor")) {
		/* Apple's phase 2 wants edge views; Samsung's template is already complete (100 %). As redfin's extend
		 * enrolment, a real touch on the sensor counts as one: RedwoodMesa hands such matches only to the
		 * enrolling client (Settings), never to an unlock. */
		ev_send(FPCD_EV_MATCH, 0, FID, 0, 0, 1, 0);
		grip_until = time(NULL) + 120;          /* each phase-2 touch keeps it open */
	} else if (op == 2 && sscanf(l, "fp run 6: SENSOR MATCH candidate (bio %u score %u", &b, &sc) == 2) {
		ev_send(FPCD_EV_MATCH, 0, FID, (int32_t)sc, 0, 1, 0);
		op = 0;
	} else if (op == 2 && !strcmp(l, "fp run 6: NO MATCH")) {
		ev_send(FPCD_EV_NOMATCH, 0x20, 0, 0, 0, 1, 0);
	} else if (strstr(l, "invalid response") || strstr(l, "re-arm failed") || strstr(l, "capture error 0x44") ||
	    strstr(l, ": stop")) {
		if (op) ev_send(op == 1 ? FPCD_EV_ENROL_FAILED : FPCD_EV_ERROR, FPCD_E_IO, 0, 0, 0, samples_seen, 0);
		op = 0;
	}
}

/* one command: wait for the backend to be idle, send, wait until it finishes; lines are read meanwhile */
static int
step(const char *cmd, uint32_t g)
{
	char s[2048];
	long until = now_ms() + STEP_MS;
	while (tee_status(s, sizeof(s)) == 0 && !tee_idle(s)) {
		if (now_ms() > until || g != gen) return -1;
		usleep(100000);
	}
	if (tee_cmd(cmd)) {
		lg("%s refused (%d)", cmd, errno);
		return -1;
	}
	until = now_ms() + (strncmp(cmd, "fp run ", 7) ? STEP_MS : RUN_MS);
	usleep(100000);
	for (;;) {
		log_read(on_line);
		if (tee_status(s, sizeof(s))) return -1;
		if (tee_idle(s)) return tee_ok(cmd, s) ? 0 : -1;
		if (g != gen) {                 /* cancelled: stop the backend's run and let it drain */
			tee_cmd("fp stop");
			g = gen;
		}
		if (now_ms() > until) {
			tee_cmd("fp stop");
			ev_send(FPCD_EV_OP_TIMEOUT, 0, 0, 0, 0, 0, 0);
			return -1;
		}
		usleep(100000);
	}
}


/* the opcode the TEE asks for next, from kern.redwood.tee's "last:" line ("... opcode 0x44 ..."), or -1 */
static int
tee_next_op(void)
{
	char s[2048];
	if (tee_status(s, sizeof(s))) return -1;
	const char *l = strstr(s, "\nlast: ");
	const char *o = l ? strstr(l, " opcode ") : NULL;
	return o ? (int)strtol(o + 8, NULL, 0) : -1;
}

/*
 * Background calibration as Samsung's FPBAuthService::new_cbge_mode (libbauthserver 0x15abc0) does when the sensor
 * raises its IPC line (CBGE_REQUIRED): CONTROL 69, then whatever opcode the TEE returns (68, 70 with its 10 ms
 * timeout, 67), then CONTROL 71 until it answers 0. Without it every capture ran with "IPC line low" and the matcher
 * rejected most images (PARTIAL / DUPLICATED / LIGHT_TOUCH); on the phone (2026-11-19) this chain set the line high.
 */
static void __attribute__((unused))
cbge_chain(uint32_t g)
{
	char c[32];
	int op = 69;
	for (int n = 0; n < 40 && op > 0 && op != 71 && g == gen; n++) {
		snprintf(c, sizeof(c), "fp 12:%d", op);
		if (step(c, g) && op != 67) break;              /* 67 answers 0x33 without slot data: go on */
		int next = tee_next_op();
		if (op == 70) usleep(10000);
		if (next == op && op == 67) break;
		op = next;
	}
	for (int n = 0; n < 8 && g == gen; n++) {
		step("fp 12:71", g);
		if (tee_next_op() == 0) break;
	}
	lg("background calibration done");
}

/*
 * Samsung's unlock wraps the identify in BAuth CONTROL operations (libbauthserver, ExtremeROM log of 2026-11-19,
 * docs/devices/x1slte/android-reference.md): 108, 101 (76 = diagnostics) then 74, 49 before the touch, 75, 46 after.
 * Ours sent none of them and got NO MATCH. The lists are read from /private/var/redwood/match-ops when it exists
 * ("pre=108,101 arm=74,49 post=75,46"; an empty list or "off" skips that part), so the order can be tried on the
 * phone without a rebuild. Failures are logged, never fatal: the identify runs either way.
 */
#define MATCH_OPS "/private/var/redwood/match-ops"
static char ops_pre[64] = "108,101,74,49", ops_arm[64] = "", ops_post[64] = "75,46";
/* Samsung enrol (same log): preEnroll 22,43 + 16; before the first touch 49 (then HAT cmd 13, not sent), 75; stored 45,46,75 */
static char ops_epre[64] = "22,43,16", ops_earm[64] = "49,75", ops_epost[64] = "45,46,75";
static void
match_ops_load(void)
{
	char b[256];
	FILE *f = fopen(MATCH_OPS, "r");
	if (!f) return;
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[n] = 0;
	struct { const char *k; char *v; } kv[] = { { "pre=", ops_pre }, { "arm=", ops_arm }, { "post=", ops_post },
		{ "epre=", ops_epre }, { "earm=", ops_earm }, { "epost=", ops_epost } };
	for (unsigned i = 0; i < 6; i++) {
		const char *p = strstr(b, kv[i].k);
		while (p && p != b && p[-1] != ' ' && p[-1] != '\n') p = strstr(p + 1, kv[i].k);   /* "pre=" not inside "epre=" */
		if (!p) continue;
		p += strlen(kv[i].k);
		size_t l = strcspn(p, " \t\n");
		if (l >= 64) l = 63;
		memcpy(kv[i].v, p, l);
		kv[i].v[l] = 0;
		if (!strcmp(kv[i].v, "off")) kv[i].v[0] = 0;
	}
	lg("ops from " MATCH_OPS ": match pre=%s arm=%s post=%s; enrol pre=%s arm=%s post=%s", ops_pre, ops_arm, ops_post,
	    ops_epre, ops_earm, ops_epost);
}
static void
match_ops(const char *list, uint32_t g)
{
	char c[32];
	for (const char *p = list; *p && g == gen; ) {
		int o = (int)strtol(p, (char **)&p, 10);
		if (o <= 0) break;
		snprintf(c, sizeof(c), "fp 12:%d", o);
		if (step(c, g)) lg("%s: not accepted (going on)", c);
		while (*p == ',') p++;
	}
}

static void *
run(void *arg)
{
	int kind = (int)(intptr_t)arg;
	uint32_t g = gen;
	const char *setup[] = { "fp power", "fp prepare", "fp cbgeefs", "fp 10:1", "fp prepare" };
	pthread_mutex_lock(&mx);
	ring(kind == 2 && grip_phase ? 1 : kind);       /* 1 enrolment (and Apple's phase 2), 2 match: SpringBoard shows
	                                                 * a match's ring only where a finger is asked for */
	log_baseline();
	if (kind == 1 && enrol_pct == 0) samples_seen = 0;
	int bad = 0;
	/* a match keeps the background calibration the enrolment ran with (stock calibrates when the sensor asks and
	 * keeps it); reloading the factory slots and PREPARE op 2 before identify gave NO MATCH on the same finger */
	if (kind == 2 && calibrated) bad = step("fp power", g);
	else for (unsigned i = 0; i < sizeof(setup) / sizeof(setup[0]) && !bad; i++) bad = step(setup[i], g);
	/* the kernel's fp cbge runs the same chain with CONTROL 67 per slot (slot byte + 0x96000 output), as Samsung's
	 * update_cbge (0x14c794); the generic chain above left 67 without data and the next capture failed with 0x44 */
	/* only before an enrolment: a calibration taken while a finger rests on the sensor (Apple's phase 2 asks for the
	 * next touch at once) becomes the "background", and identify's contamination check then fails every image (29) */
	if (!bad && kind == 1) { step("fp cbge", g); calibrated = 1; lg("background calibration: fp cbge done"); }
	if (kind == 2) {
		/* iOS sends MatchMode once and waits (the lock screen, Apple's phase 2): listen in 15 s runs while it still
		 * waits, up to 8 runs (2 minutes), then stop until iOS asks again. One run only stopped phase 2 after 3-5
		 * touches (2026-11-19). (The resets once blamed on a loop here were thermal: PMIC WTSR at 109 C.) */
		for (int runs = 0; !bad && op == 2 && g == gen && runs < 8; runs++) {
			if (runs) usleep(300000);
			ring(grip_phase ? 1 : 2);       /* the lock closed phase 2 mid-loop: show the lock-screen ring again */
			if (!runs) match_ops_load();
			match_ops(ops_pre, g);
			bad = step("fp idinit", g);
			if (!bad) match_ops(ops_arm, g);
			if (!bad) bad = step("fp run 6 1 15 0", g);
			match_ops(ops_post, g);
		}
	} else {
		/* a run ends after its step budget; enrolment goes on in the next one until the template is saved (op 0),
		 * as tools/fingerprint/ui/broker.m did (17 touches to 100 % on this phone) */
		match_ops_load();
		match_ops(ops_epre, g);
		for (int rounds = 0; !bad && op == 1 && g == gen && rounds < 200; rounds++) {
			if (!rounds) match_ops(ops_earm, g);
			bad = step("fp einit 1", g);
			if (!bad) bad = step("fp run 3 1 45 4", g);
		}
		if (enrol_saved) {              /* the template was saved: Samsung's 45, 46, 75 (even if iOS moved on) */
			enrol_saved = 0;
			match_ops(ops_epost, gen);
		}
	}
	if (bad && op == kind && g == gen) {
		ev_send(kind == 1 ? FPCD_EV_ENROL_FAILED : FPCD_EV_ERROR, FPCD_E_IO, 0, 0, 0, samples_seen, 0);
	}
	if (g == gen) op = 0;
	ring(0);
	pthread_mutex_unlock(&mx);
	return NULL;
}

static int
start(int kind)
{
	gen++;
	op = kind;
	pthread_t t;
	if (pthread_create(&t, NULL, run, (void *)(intptr_t)kind)) return FPCD_E_BUSY;
	pthread_detach(t);
	return FPCD_OK;
}


static int rep_u32(struct smb_reply *p, uint32_t v);

/* ---- catacomb (Apple's saved list of enrolled fingers; RedwoodMesa 1206, kern.sweet_mesa_catacomb=1) ----
 * biometrickitd keeps its enrolled identities in a catacomb file whose secure part the Secure Enclave seals. Here that
 * part is a small SWCT record (fpcd_bridge.h) naming finger 1; Samsung's template itself stays in TEMPLATE. Without
 * this, biometrickitd never saved the list and forgot the finger at every reboot. */
static uint8_t cat_out[sizeof(struct swct_hdr)], cat_in[4096];
static uint32_t cat_out_len, cat_in_len;

static uint32_t
crc32_le(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
	}
	return ~c;
}

static int
cat_prepare(const struct smb_req *q, struct smb_reply *p)
{
	struct swct_hdr h = { .magic = SWCT_MAGIC, .version = SWCT_VERSION, .hdr_size = sizeof(h), .total = sizeof(h),
	    .user = q->len >= 4 ? *(const uint32_t *)q->data : 501, .dbid = 0x4a45535649544944ull };
	if (access(TEMPLATE, F_OK) == 0) {
		h.nfids = 1;
		h.fids[0] = FID;
	}
	strlcpy(h.ta, "finge", sizeof(h.ta));
	h.crc = crc32_le((const uint8_t *)&h + 16, sizeof(h) - 16);
	memcpy(cat_out, &h, sizeof(h));
	cat_out_len = sizeof(h);
	lg("catacomb: prepared %u bytes (%u finger)", cat_out_len, h.nfids);
	return rep_u32(p, cat_out_len);
}

static int
cat_get(const struct smb_req *q, struct smb_reply *p)
{
	uint32_t off = q->len >= 8 ? ((const uint32_t *)q->data)[0] : 0, max = q->len >= 8 ? ((const uint32_t *)q->data)[1] : 0;
	if (off > cat_out_len) return FPCD_E_INVAL;
	uint32_t n = cat_out_len - off;
	if (n > max) n = max;
	if (n > SMB_DATA_MAX) n = SMB_DATA_MAX;
	memcpy(p->data, cat_out + off, n);
	p->len = n;
	return FPCD_OK;
}

static int
cat_put(const struct smb_req *q, struct smb_reply *p)
{
	(void)p;
	if (q->len < 8) return FPCD_E_INVAL;
	uint32_t total = ((const uint32_t *)q->data)[0], off = ((const uint32_t *)q->data)[1], n = q->len - 8;
	if (total > sizeof(cat_in) || off + n > total) return FPCD_E_INVAL;
	memcpy(cat_in + off, q->data + 8, n);
	cat_in_len = total;
	return FPCD_OK;
}

static int
cat_load(void)
{
	struct swct_hdr h;
	if (cat_in_len < sizeof(h)) return FPCD_E_INVAL;
	memcpy(&h, cat_in, sizeof(h));
	if (h.magic != SWCT_MAGIC || h.crc != crc32_le(cat_in + 16, sizeof(h) - 16)) {
		lg("catacomb: load refused (bad record)");
		return FPCD_E_INVAL;
	}
	lg("catacomb: loaded (%u finger; template %s)", h.nfids, access(TEMPLATE, F_OK) ? "missing" : "present");
	return FPCD_OK;
}

/* ---- requests from the kernel ---- */
static int
rep_u32(struct smb_reply *p, uint32_t v)
{
	memcpy(p->data, &v, 4);
	p->len = 4;
	return FPCD_OK;
}

static int
handle(const struct smb_req *q, struct smb_reply *p)
{
	int has = access(TEMPLATE, F_OK) == 0;
	switch (q->cmd) {
	case 1: return rep_u32(p, 2);                           /* GetCmdProtocolVersion */
	case 83: return rep_u32(p, 1);                          /* IsSensorReady */
	case 15: return rep_u32(p, 1);                          /* RequestMaxIdentityCount: one finger */
	case 65: return rep_u32(p, has ? 0 : 1);                /* GetFreeIdentityCount */
	case 66: {                                              /* GetIdentitiesList */
		struct fpcd_ids ids = { .count = (uint32_t)has, .ids = { FID } };
		memcpy(p->data, &ids, sizeof(ids));
		p->len = sizeof(ids);
		return FPCD_OK;
	}
	case 3: enrolling = 1; grip_until = 0; return start(1);     /* EnrollMode */
	case 4:                                                 /* MatchMode */
		if (enrolling) {                                /* phase 2 before the template was committed: go on */
			if (op != 1) return start(1);
			return FPCD_OK;
		}
		return has ? start(2) : FPCD_E_NOENT;
	case 12:                                                /* Cancel (Apple also sends it between its two phases) */
		gen++;
		op = 0;
		return FPCD_OK;
	case 38: case 69: return FPCD_OK;                       /* finger detect: the runs detect by themselves */
	case 2:                                                 /* ResetSensor: end any run */
		if (op) {
			gen++;
			op = 0;
		}
		return FPCD_OK;
	case 14: return FPCD_OK;                                /* EnrollContinue: the enrol run goes on by itself */
	case 49: return FPCD_OK;                                /* NoCatacomb: the template file stays (nothing to load) */
	case 56: {                                              /* GetCatacombId: a stable id for the template db */
		uint64_t id = has ? 0x4a45535649544944ull : 0;  /* "JESVITID" */
		memcpy(p->data, &id, 8);
		p->len = 8;
		return FPCD_OK;
	}
	case 78: p->len = 0; return FPCD_OK;                    /* GetLastMatchEvent: none kept here */
	case 72: {                                              /* RemoveUserData: every finger (kept aside, not deleted) */
		char bak[96];
		snprintf(bak, sizeof(bak), TEMPLATE ".removed.%ld", (long)time(NULL));
		if (has && rename(TEMPLATE, bak)) return FPCD_E_IO;
		ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0);
		return FPCD_OK;
	}
	case 13: {                                              /* RemoveTemplate */
		char bak[96];
		snprintf(bak, sizeof(bak), TEMPLATE ".removed.%ld", (long)time(NULL));
		if (has && rename(TEMPLATE, bak)) return FPCD_E_IO;    /* kept aside, never deleted */
		ev_send(FPCD_EV_TEMPLATES, 0, 0, 0, 0, 0, 0);
		return FPCD_OK;
	}
	case 61: return cat_prepare(q, p);                      /* PrepareSaveCatacomb */
	case FPCD_CMD_CAT_GET: return cat_get(q, p);
	case 63: return FPCD_OK;                                /* ConfirmSaveCatacomb */
	case FPCD_CMD_CAT_PUT: return cat_put(q, p);
	case 64: return cat_load();                             /* LoadCatacomb */
	case FPCD_CMD_PING:
		p->len = (uint32_t)snprintf((char *)p->data, SMB_DATA_MAX, "redwood-fpd 1") + 1;
		return FPCD_OK;
	default:
		return FPCD_E_UNSUPPORTED;
	}
}


/* TEEGRIS up, as tools/fingerprint/tee-up.sh: the storage daemon, the secure OS, then this phone's driver TAs
 * (/var/root/tee/driver/: Samsung's, pulled from the phone itself, never in the image) */
extern char **environ;
/*
 * A clean data volume (fresh install) has neither the fingerprint TA set (/var/root/tee) nor the sensor's factory
 * calibration (/var/root/fp-calib): the TA failed to open ("fp: open err 2") and enrolment ended at once (2026-11-20).
 * The TAs come from the system image (/System/Library/Redwood/Fingerprint/tee, a local build's copy of the phone's
 * own Samsung vendor/tee; never in a SHARE=1 image), the calibration from the phone's EFS (cell-provision --fp,
 * read-only). Files present are never replaced. docs/clean-install.md.
 */
#define TEE_SEED "/System/Library/Redwood/Fingerprint/tee"
static int
copy_tree_files(const char *from, const char *to)
{
	DIR *d = opendir(from);
	struct dirent *e;
	int n = 0;
	if (!d) return 0;
	mkdir(to, 0700);
	while ((e = readdir(d))) {
		char f[512], t[512];
		struct stat st;
		if (e->d_name[0] == '.') continue;
		snprintf(f, sizeof(f), "%s/%s", from, e->d_name);
		snprintf(t, sizeof(t), "%s/%s", to, e->d_name);
		if (stat(f, &st)) continue;
		if (S_ISDIR(st.st_mode)) { n += copy_tree_files(f, t); continue; }
		if (access(t, F_OK) == 0) continue;
		int in = open(f, O_RDONLY), out = in >= 0 ? open(t, O_WRONLY | O_CREAT | O_EXCL, 0600) : -1;
		char buf[65536];
		ssize_t r;
		while (out >= 0 && (r = read(in, buf, sizeof(buf))) > 0) if (write(out, buf, (size_t)r) != r) break;
		if (in >= 0) close(in);
		if (out >= 0) { close(out); n++; }
	}
	closedir(d);
	return n;
}

static void
fp_provision(void)
{
	if (access("/var/root/tee/00000000-0000-0000-0000-46494e474502", F_OK) != 0) {
		int n = copy_tree_files(TEE_SEED, "/var/root/tee");
		if (n) lg("fingerprint TAs: %d files from the system image", n);
		else lg("fingerprint TAs: none in /var/root/tee and no " TEE_SEED " (a SHARE=1 image: docs/clean-install.md)");
	}
	if (access("/var/root/fp-calib/cbge_0.dat", F_OK) != 0 && access("/redwood/bin/cell-provision", X_OK) == 0) {
		pid_t pid;
		int st = 0;
		char *argv[] = { "cell-provision", "--fp", NULL };
		if (posix_spawn(&pid, "/redwood/bin/cell-provision", NULL, NULL, argv, environ) == 0) waitpid(pid, &st, 0);
		lg("sensor calibration from EFS: %s", WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "done" : "FAILED");
	}
}

static void
tee_up(void)
{
	char s[2048];
	if (tee_status(s, sizeof(s)) == 0 && strstr(s, "TEEGRIS: up")) return;
	fp_provision();
	pid_t pid;
	char *argv[] = { "/redwood/bin/teetsd", NULL };
	if (access(argv[0], X_OK) == 0 && posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) == 0) lg("teetsd started");
	tee_cmd("start");
	sleep(6);
	DIR *d = opendir("/var/root/tee/driver");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char c[320];
		snprintf(c, sizeof(c), "openkeep /var/root/tee/driver/%s", e->d_name);
		tee_cmd(c);
		sleep(3);
	}
	if (d) closedir(d);
	lg("TEEGRIS %s", tee_status(s, sizeof(s)) == 0 && strstr(s, "TEEGRIS: up") ? "up" : "did not come up");
}


/* biometrickitd loads its catacomb when it starts: stop it so launchd starts it again on demand */
static void
restart_biometrickitd(void)
{
	pid_t pids[2048];
	int n = proc_listallpids(pids, sizeof(pids)) ;
	for (int i = 0; i < n; i++) {
		char path[PROC_PIDPATHINFO_MAXSIZE];
		if (proc_pidpath(pids[i], path, sizeof(path)) > 0 && !strcmp(path, "/usr/libexec/biometrickitd")) {
			kill(pids[i], SIGKILL);
			lg("first unlock: biometrickitd (%d) restarted to load its catacomb", pids[i]);
		}
	}
}

static void
set(const char *name, int v)
{
	if (sysctlbyname(name, NULL, NULL, &v, sizeof(v))) lg("%s=%d: errno %d", name, v, errno);
}

int
main(void)
{
	char args[1024] = "";
	size_t n = sizeof(args) - 1;
	sysctlbyname("kern.bootargs", args, &n, NULL, 0);
	if (!strstr(args, "touchid=1")) return 0;               /* off unless asked for at boot */
	if (getuid() != 0) return 1;
	/* no iPhone 8 cache, no Touch ID: biometrickitd would run on the iPhone X cache and SpringBoard wait on it for ever
	 * (src/init.c touchid_early, 2026-11-20) */
	if (access("/usr/lib/libBKDM1.dylib", F_OK) != 0 &&
	    access("/private/var/redwood/iphone8-dsc/dyld_shared_cache_arm64", F_OK) != 0) {
		lg("no ported libBKDM1 (tools/touchid/fetch-bkdm1.sh) nor iPhone 8 cache: Touch ID off");
		return 0;
	}
	umask(0077);
	set("kern.sweet_mesa_match", 2);                        /* the TEE matcher's decision, token unverified */
	set("kern.sweet_mesa_offline", 1);
	set("kern.sweet_mesa_catacomb", 1);                     /* biometrickitd saves its finger list (cat_*) */
	set("kern.sweet_mesa", 3);                              /* publish the persona + translate our events */
	for (int i = 0; bridge_open(); i++) {
		if (i == 50) {
			lg("no %s", SMB_SERVICE);
			return 1;
		}
		usleep(200000);
	}
	lg("bridge open; template %s", access(TEMPLATE, F_OK) ? "none" : "present");
	/* TEEGRIS starts meanwhile: biometrickitd's first questions at boot (catacomb load, identities) need no TEE and
	 * must not meet a daemon still busy with tee_up(), or it answers NotReady and the finger list stays unloaded */
	pthread_t tt;
	if (pthread_create(&tt, NULL, (void *(*)(void *))tee_up, NULL) == 0) pthread_detach(tt);
	/* biometrickitd reads its catacomb (the enrolled fingers) at boot, before the first unlock, when data protection
	 * still hides the file: it answers NoCatacomb and drops the finger (S20, 2026-11-19). On the first unlock the
	 * bridge is opened again, and RedwoodMesa's connect message (0xe3ff800d) makes biometrickitd re-read it. */
	static volatile int first_unlock;
	int fu_token;
	notify_register_dispatch("com.apple.mobile.keybagd.first_unlock", &fu_token,
	    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int t) { (void)t; first_unlock = 1; });
	/* Phase 2's "any touch counts" is for Settings only. Its window (2 min, renewed by every touch) outlived Settings:
	 * locking right after enrolment let ANY touch unlock the phone (score 0, no matching) and hid the lock-screen ring
	 * (2026-11-19 22:00). Locking the phone ends phase 2. */
	int lk_token;
	notify_register_dispatch("com.apple.springboard.lockcomplete", &lk_token,
	    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int t) {
		(void)t;
		if (grip_phase) lg("locked: phase 2 window closed");
		grip_until = 0;
	});
	static struct smb_req q;
	static struct smb_reply p;
	for (;;) {
		if (first_unlock) {
			first_unlock = 0;
			/* (closing and reopening the bridge here left it deaf: requests were never picked up) */
			sleep(2);                       /* the keybag settles; then the catacomb file is readable */
			restart_biometrickitd();
		}
		uint64_t timeout = 1000;
		size_t qn = sizeof(q);
		kern_return_t k = IOConnectCallMethod(conn, SMB_SEL_WAIT, &timeout, 1, NULL, 0, NULL, NULL, &q, &qn);
		if (k == (kern_return_t)0xe00002d6) continue;          /* kIOReturnTimeout */
		if (k != KERN_SUCCESS) {
			lg("wait: %#x", k);
			sleep(1);
			continue;
		}
		memset(&p, 0, offsetof(struct smb_reply, data));
		p.seq = q.seq;
		p.status = handle(&q, &p);
		IOConnectCallStructMethod(conn, SMB_SEL_REPLY, &p, offsetof(struct smb_reply, data) + p.len, NULL, NULL);
	}
}

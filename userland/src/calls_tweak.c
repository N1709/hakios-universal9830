/*
 * libredwood-calls.dylib (A, 2026-11-14): the call hooks of the Phone app and the call screen, kept outside the boot
 * image so they can change without a reboot. Loaded by libredwood.dylib (src/inject.c) in MobilePhone and InCallService
 * from /var/jb/redwood/lib. Build: userland/build-calls.sh; install: copy, then restart InCallService.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "phone_dial.h"

__attribute__((constructor))
static void
calls_init(void)
{
	const char *name = getprogname();
	if (!name) return;
	{   /* which processes have the hooks: /var/tmp/redwood-calls-hooks.log */
		FILE *f = fopen("/var/tmp/redwood-calls-hooks.log", "a");
		if (f) { fprintf(f, "%ld %s[%d] loaded\n", (long)time(NULL), name, getpid()); fclose(f); }
	}
	/* (B, 2026-11-15) calls now come from redwood-telephonyd, the real telephony provider (CommCenter's CallKit
	 * identity): the Phone app dials it natively and iOS treats its calls as phone calls, so the dial redirect and the
	 * isSystemProvider/isTelephonyProvider answers of 2026-11-14 are gone. What stays: the call screen's Speaker
	 * button, until the call's audio goes through iOS's own routes. */
	if (!strcmp(name, "InCallService")) phone_speaker_hook();
}

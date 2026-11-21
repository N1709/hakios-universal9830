/*
 * dsc-exec DIR PROGRAM [ARG...]: run PROGRAM on the dyld shared cache in DIR (DYLD_SHARED_CACHE_DIR).
 * launchd drops DYLD_SHARED_CACHE_DIR from a job's EnvironmentVariables ("Disallowing environment variable"), so the
 * job runs this, which sets it and execs the program: same process, so its Mach services stay checked in. dyld then
 * maps that cache privately for this process alone (our AMFI stand-in allows custom caches). DYLD_SHARED_REGION=private
 * is needed too: with the directory alone dyld still used the system cache (on the S20, 2026-11-18).
 * Used for Touch ID: biometrickitd on the iPhone 8's own cache, where Apple's libBKDM1 lives
 * (userland/com.apple.biometrickitd.mesa.plist, tools/touchid/push-iphone8-cache.sh).
 * If DIR holds no cache, the program runs on the system's cache as usual.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char *argv[])
{
	if (argc < 3) {
		fprintf(stderr, "usage: dsc-exec DIR PROGRAM [ARG...]\n");
		return 2;
	}
	char cache[1024];
	snprintf(cache, sizeof(cache), "%s/dyld_shared_cache_arm64", argv[1]);
	if (access(cache, R_OK) == 0) {
		setenv("DYLD_SHARED_CACHE_DIR", argv[1], 1);
		setenv("DYLD_SHARED_REGION", "private", 1);     /* without it dyld keeps the system's cache (tested) */
	} else {
		fprintf(stderr, "dsc-exec: no %s: %s on the system cache\n", cache, argv[2]);
	}
	execv(argv[2], argv + 2);
	perror(argv[2]);
	return 127;
}

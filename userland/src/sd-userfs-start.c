#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	const char *program = "/System/Library/PrivateFrameworks/UserFS.framework/userfsd";
	CFStringRef domain = CFSTR("com.apple.filesystems.livefileproviderd");

	if (argc == 0 || strcmp(argv[0], program) != 0) {
		return 1;
	}

	/* userfsd's initial scan requires an explicit false before it starts. */
	for (unsigned attempt = 0; ; attempt++) {
		CFPreferencesSetAppValue(CFSTR("LIFS_DA"), kCFBooleanFalse, domain);
		if (CFPreferencesAppSynchronize(domain)) {
			break;
		}
		if (attempt == 29) {
			fprintf(stderr, "sd-userfs-start: failed to synchronize LIFS_DA\n");
			return 1;
		}
		sleep(1);
	}

	for (unsigned attempt = 0; attempt < 30; attempt++) {
		char state[256];
		size_t size = sizeof(state);

		if (sysctlbyname("kern.redwood.lifs", state, &size, NULL, 0) != 0 ||
		    strncmp(state, "loaded:", 7) == 0) {
			break;
		}
		sleep(1);
	}

	execv(program, argv);
	perror(program);
	return 1;
}

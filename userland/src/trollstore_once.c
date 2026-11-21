/*
 * trollstore_once: TrollStore on a fresh data volume (A, 2026-11-20).
 *
 * The image carries the official TrollStore release (opa334, MIT; /System/Library/Redwood/TrollStore/TrollStore.tar,
 * userland/build.sh). Once per data volume (marker /var/root/.trollstore-v1) this extracts it to
 * /var/root/trollstore-bootstrap and runs TrollStore's own `trollstorehelper install-trollstore`, as it was installed
 * by hand on 2026-11-11 (tools/SIDELOADING.md): the helper registers TrollStore as a system app with a data container.
 * A user who deleted TrollStore keeps it deleted (the marker stays). Boot job: com.redwood.trollstore.
 */
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TAR "/System/Library/Redwood/TrollStore/TrollStore.tar"
#define DIR_ "/var/root/trollstore-bootstrap"
#define MARKER "/var/root/.trollstore-v1"

static int
run(char *const argv[])
{
	pid_t pid;
	int st = 0;

	if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0) return -1;
	if (waitpid(pid, &st, 0) < 0) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int
main(void)
{
	struct stat st;

	if (stat(MARKER, &st) == 0 || stat(TAR, &st) != 0) return 0;
	/* SpringBoard and the installer services first: the helper registers the app with LaunchServices */
	sleep(20);
	mkdir(DIR_, 0700);
	/* iOS has no tar of its own: the package prefix's (/var/jb, grafted by init before launchd) */
	char *tar = access("/var/jb/usr/bin/tar", X_OK) == 0 ? "/var/jb/usr/bin/tar" : "/usr/bin/tar";
	char *untar[] = { tar, "-xf", TAR, "-C", DIR_, NULL };
	if (run(untar) != 0) {
		printf("trollstore_once: extracting %s failed\n", TAR);
		return 1;
	}
	char *install[] = { DIR_ "/TrollStore.app/trollstorehelper", "install-trollstore", TAR, NULL };
	int r = run(install);
	printf("trollstore_once: trollstorehelper install-trollstore: exit %d\n", r);
	if (r == 0) close(open(MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0644));
	return r == 0 ? 0 : 1;
}

/*
 * Forward Exynos TMU pressure to the Darwin notification that SpringBoard
 * and ProcessInfo already observe. SpringBoard owns the temperature screen.
 */
#include <errno.h>
#include <notify.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define PRESSURE_NAME "com.apple.system.thermalpressurelevel"
#define PRESSURE_SYSCTL "kern.redwood.thermal_pressure"

static int
kernel_level(int *level)
{
	size_t len = sizeof(*level);
	return sysctlbyname(PRESSURE_SYSCTL, level, &len, NULL, 0);
}

static const char *
level_name(int level)
{
	switch (level) {
	case 0: return "nominal";
	case 20: return "moderate";
	case 30: return "heavy";
	case 40: return "trapping";
	default: return "no thermal sample";
	}
}

static int
serve(void)
{
	int token, last = -1, level;

	if (kernel_level(&level) != 0 && errno == ENOENT) {
		return 0;
	}
	if (notify_register_check(PRESSURE_NAME, &token) != NOTIFY_STATUS_OK) {
		fprintf(stderr, "thermald: cannot register %s\n", PRESSURE_NAME);
		return 1;
	}
	for (;;) {
		if (kernel_level(&level) == 0 && level >= 0 && level != last &&
		    notify_set_state(token, (uint64_t)level) == NOTIFY_STATUS_OK &&
		    notify_post(PRESSURE_NAME) == NOTIFY_STATUS_OK) {
			last = level;
			printf("thermald: pressure %s (%d)\n", level_name(level), level);
			fflush(stdout);
		}
		/* The TMU is already sampled by the kernel. */
		usleep(250000);
	}
}

int
main(int argc, char **argv)
{
	if (argc > 1 && (strcmp(argv[1], "show") == 0 || strcmp(argv[1], "level") == 0)) {
		int level = -1;

		if (kernel_level(&level) != 0) {
			level = -1;
		}
		printf("%d %s\n", level, level_name(level));
		return level < 0;
	}
	return serve();
}

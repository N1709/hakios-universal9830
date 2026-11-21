/* rwsys NAME [STRING]: read a sysctl as a string, or write STRING to it and print what it reads back (the phone has no sysctl(8)) */
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

int
main(int argc, char **argv)
{
	static char out[65536];
	size_t len = sizeof(out) - 1;

	if (argc < 2) {
		fprintf(stderr, "usage: rwsys NAME [STRING]\n");
		return 2;
	}
	if (sysctlbyname(argv[1], out, &len, argc > 2 ? argv[2] : NULL, argc > 2 ? strlen(argv[2]) : 0) != 0) {
		perror(argv[1]);
		return 1;
	}
	out[len < sizeof(out) ? len : sizeof(out) - 1] = 0;
	fputs(out, stdout);
	return 0;
}

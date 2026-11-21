/* objcmap: every Objective-C method of the named images, as "OFFSET -[Class sel]", OFFSET from the shared cache's base
 * (the same offsets kern.redwood.ksample prints). usage: objcmap IMAGE_PATH... */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>

static sigjmp_buf objcmap_back;
static void objcmap_abort(int sig) { (void)sig; siglongjmp(objcmap_back, 1); }

extern const void *_dyld_get_shared_cache_range(size_t *length);

static void
dump(Class cls, const char *name, char kind, uintptr_t base)
{
	unsigned n = 0;
	Method *m = class_copyMethodList(cls, &n);

	for (unsigned i = 0; i < n; i++) {
		uintptr_t imp = (uintptr_t)method_getImplementation(m[i]);
		if (imp > base) {
			printf("%lx %c[%s %s]\n", (unsigned long)(imp - base), kind, name, sel_getName(method_getName(m[i])));
		}
	}
	free(m);
}

int
main(int argc, char **argv)
{
	size_t len = 0;
	uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&len);

	for (int a = 1; a < argc; a++) {
		unsigned count = 0;
		const char **names;

		signal(SIGABRT, objcmap_abort);
		if (sigsetjmp(objcmap_back, 1) == 0) {
			if (dlopen(argv[a], RTLD_NOW) == NULL) {
				fprintf(stderr, "objcmap: %s: %s\n", argv[a], dlerror());
			}
		} else {
			fprintf(stderr, "objcmap: %s: its initializer aborted; its classes are read anyway\n", argv[a]);
		}
		names = objc_copyClassNamesForImage(argv[a], &count);
		fprintf(stderr, "objcmap: %s: %u classes\n", argv[a], count);
		for (unsigned i = 0; i < count; i++) {
			Class cls = objc_getClass(names[i]);
			if (cls == Nil) {
				continue;
			}
			dump(cls, names[i], '-', base);
			dump(object_getClass((id)cls), names[i], '+', base);
		}
		free(names);
	}
	return 0;
}

/* SpringBoard's frameworks assert that the process is a platform binary in an initializer (SBStaticInitializer): for a
 * map that is all the process does, so the assertion is let go */
extern void __assert_rtn(const char *, const char *, int, const char *);
static void
objcmap_assert(const char *func, const char *file, int line, const char *what)
{
	fprintf(stderr, "objcmap: assertion let go: %s (%s:%d %s)\n", what, file, line, func);
}
__attribute__((used, section("__DATA,__interpose"))) static struct {
	const void *replacement, *replacee;
} objcmap_interpose[] = { { (const void *)objcmap_assert, (const void *)__assert_rtn } };

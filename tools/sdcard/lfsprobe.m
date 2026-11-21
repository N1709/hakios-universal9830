#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
static void dump(const char *name) {
	Class c = objc_getClass(name);
	if (!c) { printf("%s: no class\n", name); return; }
	unsigned n; Method *m;
	m = class_copyMethodList(object_getClass(c), &n);
	for (unsigned i = 0; i < n; i++) printf("%s +%s %s\n", name, sel_getName(method_getName(m[i])), method_getTypeEncoding(m[i]));
	free(m);
	m = class_copyMethodList(c, &n);
	for (unsigned i = 0; i < n; i++) printf("%s -%s\n", name, sel_getName(method_getName(m[i])));
	free(m);
}
int main(int argc, char **argv) {
	@autoreleasepool {
		dlopen("/System/Library/PrivateFrameworks/LiveFS.framework/LiveFS", RTLD_NOW); dlopen("/System/Library/PrivateFrameworks/UserFS.framework/UserFS", RTLD_NOW); dlopen("/System/Library/PrivateFrameworks/LiveFSFPHelper.framework/LiveFSFPHelper", RTLD_NOW);
		for (int i = 1; i < argc; i++) dump(argv[i]);
		Protocol *p = objc_getProtocol(getenv("PROTO") ? getenv("PROTO") : "LiveMountService");
		if (p) { unsigned n; struct objc_method_description *d = protocol_copyMethodDescriptionList(p, YES, YES, &n);
			for (unsigned i = 0; i < n; i++) printf("LiveMountService -%s %s\n", sel_getName(d[i].name), d[i].types); }
	}
	return 0;
}

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
int main(void) {
	@autoreleasepool {
		dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
		Class D = objc_getClass("CADisplay");
		id d = ((id (*)(Class, SEL))objc_msgSend)(D, sel_registerName("mainDisplay"));
		printf("CADisplay mainDisplay: %s\n", d ? "yes" : "no");
		if (d) {
			double rr = ((double (*)(id, SEL))objc_msgSend)(d, sel_registerName("refreshRate"));
			long mx = [d respondsToSelector:sel_registerName("maximumRefreshRate")] ? (long)(((double (*)(id, SEL))objc_msgSend)(d, sel_registerName("maximumRefreshRate"))) : -1;
			id mode = ((id (*)(id, SEL))objc_msgSend)(d, sel_registerName("currentMode"));
			printf("refreshRate %.6f s (%.1f Hz), maximumRefreshRate %ld\ncurrentMode %s\n", rr, rr > 0 ? 1 / rr : 0, mx,
			    [[mode description] UTF8String] ?: "?");
		}
	}
	return 0;
}

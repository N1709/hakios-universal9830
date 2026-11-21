// oslogq SECONDS [PROCESS...]: stream the live system log for SECONDS, optionally only the named processes
// (LoggingSupport's live stream, as Apple's `log stream`; the public OSLogStore has no system scope on iOS).
// Entitlements: oslogq.ent. Build: build.sh here.
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
	@autoreleasepool {
		if (!dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW)) { fprintf(stderr, "oslogq: %s\n", dlerror()); return 1; }
		double secs = argc > 1 ? atof(argv[1]) : 10;
		NSMutableSet *procs = [NSMutableSet set];
		for (int i = 2; i < argc; i++) [procs addObject:@(argv[i])];
		id store = ((id (*)(id, SEL))objc_msgSend)(objc_getClass("OSLogEventLiveStore"), sel_registerName("liveLocalStore"));
		dispatch_semaphore_t ready = dispatch_semaphore_create(0);
		__block id source = nil;
		((void (*)(id, SEL, id))objc_msgSend)(store, sel_registerName("prepareWithCompletionHandler:"), ^(id src, NSError *err) {
			if (!src) fprintf(stderr, "oslogq: prepare: %s\n", err.description.UTF8String);
			source = src; dispatch_semaphore_signal(ready); });
		dispatch_semaphore_wait(ready, DISPATCH_TIME_FOREVER);
		if (!source) return 1;
		id stream = ((id (*)(id, SEL, id))objc_msgSend)([objc_getClass("OSLogEventLiveStream") alloc], sel_registerName("initWithLiveSource:"), source);
		((void (*)(id, SEL, unsigned long))objc_msgSend)(stream, sel_registerName("setFlags:"), 0x1 | 0x2 | 0x4);   /* info, debug, signposts off */
		((void (*)(id, SEL, id))objc_msgSend)(stream, sel_registerName("setEventHandler:"), ^(id ev) {
			NSString *p = [[ev valueForKey:@"process"] description];
			if (procs.count && ![procs containsObject:p]) return;
			printf("%s[%s] %s: %s\n", p.UTF8String, [[ev valueForKey:@"processIdentifier"] description].UTF8String,
			    [[ev valueForKey:@"subsystem"] description].UTF8String ?: "", [[ev valueForKey:@"composedMessage"] description].UTF8String ?: "");
			fflush(stdout); });
		((void (*)(id, SEL))objc_msgSend)(stream, sel_registerName("activate"));
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:secs]];
	}
	return 0;
}

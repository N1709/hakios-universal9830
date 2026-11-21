#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#import <objc/message.h>
@interface NSObject (LFS) - (void)listVolumes:(id)r; - (void)addDisk:(id)d reply:(id)r; - (void)addDisk:(id)d fileSystemType:(id)f reply:(id)r; @end
extern const char *_protocol_getMethodTypeEncoding(Protocol *, SEL, BOOL, BOOL);
int main(int argc, char **argv) {
	@autoreleasepool {
		dlopen("/System/Library/PrivateFrameworks/LiveFS.framework/LiveFS", RTLD_NOW);
		Protocol *p = objc_getProtocol("LiveFSVolumeManager");
		if (!p) { printf("no protocol LiveFSVolumeManager\n"); return 1; }
		const char *sels[] = { "listVolumes:", "addDisk:reply:", "addDisk:fileSystemType:reply:", "ejectDisk:usingFlags:reply:" };
		for (int i = 0; i < 4; i++) printf("%s %s\n", sels[i], _protocol_getMethodTypeEncoding(p, sel_registerName(sels[i]), YES, YES) ?: "?");
		if (argc < 2) return 0;
		NSXPCConnection *c = ((id (*)(id, SEL, id, NSUInteger))objc_msgSend)([NSXPCConnection alloc], sel_registerName("initWithMachServiceName:options:"), @"com.apple.filesystems.userfsd", (NSUInteger)0);
		c.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:p];
		c.interruptionHandler = ^{ printf("interrupted\n"); };
		c.invalidationHandler = ^{ printf("invalidated\n"); };
		[c resume];
		dispatch_semaphore_t sem = dispatch_semaphore_create(0);
		id proxy = [c remoteObjectProxyWithErrorHandler:^(NSError *e) { printf("xpc error: %s\n", e.description.UTF8String); dispatch_semaphore_signal(sem); }];
		NSString *cmd = @(argv[1]);
		if ([cmd isEqual:@"list"]) {
			[proxy listVolumes:^(NSDictionary *vols) { printf("volumes: %s\n", [vols description].UTF8String); dispatch_semaphore_signal(sem); }];
		} else if ([cmd isEqual:@"add"] && argc >= 3) {
			NSString *disk = @(argv[2]);
			if (argc >= 4) [proxy addDisk:disk fileSystemType:@(argv[3]) reply:^(NSError *e, NSArray *vols) { printf("addDisk %s fs: %s; volumes %s\n", disk.UTF8String, e ? e.description.UTF8String : "OK", [vols description].UTF8String); dispatch_semaphore_signal(sem); }];
			else [proxy addDisk:disk reply:^(NSError *e) { printf("addDisk %s: %s\n", disk.UTF8String, e ? e.description.UTF8String : "OK"); dispatch_semaphore_signal(sem); }];
		}
		if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC))) printf("timeout\n");
	}
	return 0;
}

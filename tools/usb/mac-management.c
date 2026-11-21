/* Bounded native management diagnostic. Reports public state/capacity and app
 * counts only. Optional service probes start and close connections; they do
 * not request a backup, restore, account operation or file transfer.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
struct notice {
	void *device;
	unsigned message;
};
static void *library;
static CFStringRef target;
static bool probe_services;
static int outcome = 1;
#define LOAD(name, ret, args) ret(*name) args = dlsym(library, #name)
static void print_value(const char *key, CFTypeRef value) {
	printf("%s: ", key);
	if (!value) {
		puts("unavailable");
		return;
	}
	CFTypeID type = CFGetTypeID(value);
	if (type == CFBooleanGetTypeID())
		puts(CFBooleanGetValue(value) ? "true" : "false");
	else if (type == CFNumberGetTypeID()) {
		int64_t n = 0;
		CFNumberGetValue(value, kCFNumberSInt64Type, &n);
		printf("%lld\n", (long long)n);
	} else if (type == CFStringGetTypeID()) {
		char text[256];
		if (CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8))
			puts(text);
		else
			puts("string exceeds diagnostic limit");
	} else
		puts("non-scalar value withheld");
}
static void callback(struct notice *n, void *unused) {
	(void)unused;
	if (n->message != 1)
		return;
	LOAD(AMDeviceCopyDeviceIdentifier, CFStringRef, (void *));
	if (!AMDeviceCopyDeviceIdentifier)
		return;
	CFStringRef identifier = AMDeviceCopyDeviceIdentifier(n->device);
	bool match = identifier && CFEqual(identifier, target);
	if (identifier)
		CFRelease(identifier);
	if (!match)
		return;
	LOAD(AMDeviceConnect, int, (void *));
	LOAD(AMDeviceDisconnect, int, (void *));
	LOAD(AMDeviceValidatePairing, int, (void *));
	LOAD(AMDeviceStartSession, int, (void *));
	LOAD(AMDeviceStopSession, int, (void *));
	LOAD(AMDeviceCopyValue, CFTypeRef, (void *, CFStringRef, CFStringRef));
	LOAD(AMDeviceCopyValueWithError, CFTypeRef, (void *, CFStringRef, CFStringRef, int *));
	LOAD(AMDeviceLookupApplications, int, (void *, CFDictionaryRef, CFDictionaryRef *));
	LOAD(AMDErrorString, const char *, (int));
	if (!AMDeviceConnect || !AMDeviceDisconnect || !AMDeviceValidatePairing || !AMDeviceStartSession ||
	    !AMDeviceStopSession || !AMDeviceCopyValue || !AMDErrorString)
		goto done;
	int r = AMDeviceConnect(n->device);
	printf("Connect: %s\n", AMDErrorString(r));
	if (r)
		goto done;
	r = AMDeviceValidatePairing(n->device);
	printf("Pairing: %s\n", AMDErrorString(r));
	if (r)
		goto disconnect;
	r = AMDeviceStartSession(n->device);
	printf("Session: %s\n", AMDErrorString(r));
	if (r)
		goto disconnect;
	const char *names[] = {"DeviceName", "ActivationState", "BrickState", "PasswordProtected"};
	for (unsigned i = 0; i < 4; i++) {
		CFStringRef key = CFStringCreateWithCString(NULL, names[i], kCFStringEncodingUTF8);
		CFTypeRef value = AMDeviceCopyValue(n->device, NULL, key);
		CFRelease(key);
		print_value(names[i], value);
		if (value)
			CFRelease(value);
	}
	CFTypeRef disk = AMDeviceCopyValue(n->device, CFSTR("com.apple.disk_usage"), NULL);
	if (disk && CFGetTypeID(disk) == CFDictionaryGetTypeID()) {
		const char *keys[] = {"TotalDiskCapacity", "TotalDataCapacity", "TotalDataAvailable",
				      "AmountDataAvailable", "TotalSystemCapacity"};
		for (unsigned i = 0; i < 5; i++) {
			CFStringRef key = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
			print_value(keys[i], CFDictionaryGetValue(disk, key));
			CFRelease(key);
		}
	} else
		puts("Disk usage: unavailable");
	if (disk)
		CFRelease(disk);
	int encryption_error = 0;
	CFTypeRef encryption = AMDeviceCopyValueWithError ?
	    AMDeviceCopyValueWithError(n->device, CFSTR("com.apple.mobile.backup"), CFSTR("WillEncrypt"), &encryption_error) :
	    AMDeviceCopyValue(n->device, CFSTR("com.apple.mobile.backup"), CFSTR("WillEncrypt"));
	print_value("Backup WillEncrypt", encryption);
	if (encryption_error) printf("Backup setting query: %s\n", AMDErrorString(encryption_error));
	if (encryption)
		CFRelease(encryption);
	if (AMDeviceLookupApplications) {
		const void *attrs[] = {CFSTR("CFBundleIdentifier"), CFSTR("UIFileSharingEnabled")};
		CFArrayRef fields = CFArrayCreate(NULL, attrs, 2, &kCFTypeArrayCallBacks);
		const void *keys[] = {CFSTR("ReturnAttributes")}, *values[] = {fields};
		CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
							     &kCFTypeDictionaryValueCallBacks);
		CFDictionaryRef apps = NULL;
		r = AMDeviceLookupApplications(n->device, options, &apps);
		CFRelease(options);
		CFRelease(fields);
		printf("Application list: %s; count=%ld\n", AMDErrorString(r), apps ? CFDictionaryGetCount(apps) : 0L);
		if (apps)
			CFRelease(apps);
	}
	if (probe_services) {
		LOAD(AMDeviceSecureStartService, int, (void *, CFStringRef, CFDictionaryRef, void **));
		LOAD(AMDServiceConnectionInvalidate, void, (void *));
		if (AMDeviceSecureStartService && AMDServiceConnectionInvalidate) {
			const char *services[] = {"com.apple.mobilebackup2", "com.apple.mobile.installation_proxy",
						  "com.apple.mobile.house_arrest",
						  "com.apple.mobile.notification_proxy", "com.apple.mobilesync"};
			for (unsigned i = 0; i < 5; i++) {
				CFStringRef service =
				    CFStringCreateWithCString(NULL, services[i], kCFStringEncodingUTF8);
				void *connection = NULL;
				r = AMDeviceSecureStartService(n->device, service, NULL, &connection);
				CFRelease(service);
				printf("%s: %s\n", services[i], AMDErrorString(r));
				fflush(stdout);
				if (connection) {
					AMDServiceConnectionInvalidate(connection);
					CFRelease(connection);
				}
			}
		}
	}
	outcome = 0;
	AMDeviceStopSession(n->device);
disconnect:
	AMDeviceDisconnect(n->device);
done:
	fflush(stdout);
	CFRunLoopStop(CFRunLoopGetCurrent());
}
int main(int argc, char **argv) {
	if ((argc != 2 && argc != 3) || (argc == 3 && strcmp(argv[2], "--probe-services"))) {
		fprintf(stderr, "usage: mac-management DEVICE_UDID [--probe-services]\n");
		return 2;
	}
	probe_services = argc == 3;
	target = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
	if (!target)
		return 2;
	library =
	    dlopen("/Library/Apple/System/Library/PrivateFrameworks/MobileDevice.framework/MobileDevice", RTLD_NOW);
	if (!library)
		return 2;
	LOAD(AMDeviceNotificationSubscribe, int, (void *, unsigned, unsigned, void *, void **));
	void *sub = NULL;
	if (!AMDeviceNotificationSubscribe || AMDeviceNotificationSubscribe(callback, 0, 0, NULL, &sub))
		return 2;
	alarm(35);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 25, false);
	CFRelease(target);
	return outcome;
}

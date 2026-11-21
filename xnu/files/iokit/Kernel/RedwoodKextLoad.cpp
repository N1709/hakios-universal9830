/*
 * The IOKit side of bsd/dev/redwood_kextload.c (A, 2026-11-20; docs/devices/x1slte/lifs-loader.md): a kext's C++
 * classes registered the way OSKext::load does it (preModLoad, the constructors, postModLoad), owned by the kernel's
 * own OSKext ("__kernel__": postModLoad wants a kext it knows), and its IOKit personality given to the catalogue so
 * that matching starts it.
 */

#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSKextLib.h>

extern "C" {
typedef void (*redwood_ctor_t)(void);

/* run the kext's __mod_init_func list between preModLoad and postModLoad: 0 or an OSReturn */
int redwood_kext_classes(redwood_ctor_t *ctors, unsigned count);
int
redwood_kext_classes(redwood_ctor_t *ctors, unsigned count)
{
	void *handle = OSMetaClass::preModLoad(kOSKextKernelIdentifier);

	if (handle == NULL) {
		return kOSReturnError;
	}
	for (unsigned i = 0; i < count; i++) {
		ctors[i]();
	}
	return OSMetaClass::postModLoad(handle);
}

/* lifs.kext's personality (its Info.plist, IOKitPersonalities) to the catalogue: true if taken */
bool redwood_kext_lifs_personality(void);
bool
redwood_kext_lifs_personality(void)
{
	OSDictionary *p = OSDictionary::withCapacity(8);
	OSArray *a;
	bool ok;

	if (p == NULL) {
		return false;
	}
	/* the owner IOKit looks up before starting the class: the kernel itself, which owns lifs's classes (postModLoad
	 * above); "com.apple.filesystems.lifs" made it ask user space to load that kext (disabled here) */
	p->setObject("CFBundleIdentifier", OSString::withCString(kOSKextKernelIdentifier));
	p->setObject("IOClass", OSString::withCString("com_apple_filesystems_lifs"));
	p->setObject("IOMatchCategory", OSString::withCString("com_apple_filesystems_lifs"));
	p->setObject("IOProviderClass", OSString::withCString("IOResources"));
	p->setObject("IOResourceMatch", OSString::withCString("IOBSD"));
	p->setObject("IOUserClientClass", OSString::withCString("AppleLIFSUserClient"));
	a = OSArray::withObjects((const OSObject **)&p, 1);
	if (a == NULL) {
		p->release();
		return false;
	}
	ok = gIOCatalogue->addDrivers(a, true);
	a->release();
	p->release();
	return ok;
}
}

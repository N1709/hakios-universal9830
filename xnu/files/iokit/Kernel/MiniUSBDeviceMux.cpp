/* Native lockdown cable notifications for the real Redwood USB mux transport.
 * Only published when usb_mux is enabled. IsActive follows USB configuration;
 * no pairing, activation or account state is invented here.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>

class AppleUSBDeviceMux : public IOService {
	OSDeclareDefaultStructors(AppleUSBDeviceMux);
};
OSDefineMetaClassAndStructors(AppleUSBDeviceMux, IOService);
class IOUSBDeviceInterface : public IOService {
	OSDeclareDefaultStructors(IOUSBDeviceInterface);
};
OSDefineMetaClassAndStructors(IOUSBDeviceInterface, IOService);

static AppleUSBDeviceMux *muxService;
static IOUSBDeviceInterface *muxInterface;

extern "C" void redwood_usbmux_notify(bool up);
void redwood_usbmux_notify(bool up) {
	if (!muxService && up) {
		IOService *provider = IOService::getResourceService();
		if (!provider)
			return;
		muxService = OSTypeAlloc(AppleUSBDeviceMux);
		if (!muxService || !muxService->init() || !muxService->attach(provider) ||
		    !muxService->start(provider)) {
			OSSafeReleaseNULL(muxService);
			return;
		}
		muxService->setName("AppleUSBDeviceMux");
		muxService->registerService();
		muxInterface = OSTypeAlloc(IOUSBDeviceInterface);
		if (!muxInterface || !muxInterface->init() || !muxInterface->attach(provider) ||
		    !muxInterface->start(provider)) {
			OSSafeReleaseNULL(muxInterface);
			return;
		}
		muxInterface->setName("RedwoodUSBMuxInterface");
		muxInterface->setProperty("USBDeviceFunction", "AppleUSBMux");
		muxInterface->setProperty("IsActive", up);
		muxInterface->registerService();
	}
	if (muxInterface)
		muxInterface->setProperty("IsActive", up);
	if (muxService)
		muxService->messageClients(0xe3ff8201, (void *)(uintptr_t)(up ? 1 : 0));
	IOLog("usbmux: native cable state %s\n", up ? "active" : "inactive");
}

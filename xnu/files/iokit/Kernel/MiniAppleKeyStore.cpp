/*
 * MiniAppleKeyStore: a stand-in for Apple's AppleKeyStore (the kernel side of the keybag,
 * which on real hardware talks to the Secure Enclave), as seen by MobileKeyBag.framework
 * and libaks.
 *
 * It describes one fixed situation: a device that has no passcode -- never locked, unlocked
 * since boot. No keys exist here. The keychain (securityd) gives every item a key of its own and
 * has it wrapped by the key of the item's protection class (kAKSWrapKey) before it stores it, and
 * unwrapped (kAKSUnwrapKey) to read the item: that is answered, so that the keychain works -- but
 * with nothing secret to wrap with, the "wrapping" is a reversible transformation and protects
 * nothing. The keychain can persist on the data volume; that does not make this wrapping secure.
 * By default requests needing real keys or bags are refused and logged once.
 * softpasscode=1 enables an experimental software verifier and software-bag ABI,
 * never genuine SEP operations, hardware identity or encryption of existing class keys.
 *
 * The protocol was worked out from the user-space side of iOS 16.5: selectors, and the
 * DER-encoded answers ("SET of SEQUENCE { UTF8String key, INTEGER value }").
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <libkern/crypto/crypto_internal.h>
#include "SoftwareKeybag.h"

extern "C" void read_random(void *, u_int);
extern "C" {
#include <sys/errno.h>
}

#include "MiniDrivers.h"

enum {
	kAKSOpenCheck           = 0x00,
	kAKSCreateBag           = 0x02,
	kAKSSaveBag             = 0x03,
	kAKSUnloadBag           = 0x04,
	kAKSSetSystem           = 0x05,
	kAKSLoadBag             = 0x06,
	kAKSChangeSecret        = 0x0f,
	kAKSChangeSecretEpilogue = 0x69,
	kAKSGetLockState        = 0x07,         /* in: handle; out: state flags */
	kAKSLockDevice          = 0x08,
	kAKSUnlockDevice        = 0x09,
	kAKSWrapKey             = 0x0a,         /* in: handle, class; a key; out: the class, the key wrapped */
	kAKSUnwrapKey           = 0x0b,         /* in: handle, class; a wrapped key; out: the key */
	kAKSUnlockBag           = 0x0c,
	kAKSGetSystem           = 0x0e,         /* out: the handle of the system keybag */
	kAKSGetDeviceState      = 0x11,         /* in: handle; out: DER */
	kAKSGetBagUUID          = 0x17,         /* out: 16 bytes */
	kAKSAssertHold          = 0x1a,
	kAKSAssertDrop          = 0x1b,
	kAKSGetConfiguration    = 0x1f,         /* in: handle; out: DER (a big buffer, so out of line) */
	kAKSLockDeviceNow       = 0x20,
	kAKSGetExtendedState    = 0x23,         /* as kAKSGetDeviceState */
	kAKSGeneration          = 0x28,
	kAKSAssertPromote       = 0x64,
	kAKSAssertConsume       = 0x65,
	kAKSNotifyStateSet      = 0x66,         /* "the first-unlock notification has been posted" */
	kAKSNotifyStateIsSet    = 0x67,
	kAKSPrewarm             = 0x73,
};

/* state flags: bit 1 no passcode, bit 2 unlocked since boot */
#define kAKSStateFlags          0x6
/* lock state: 0 unlocked, 1 locked, 2 locking, 3 disabled (no passcode) */
#define kAKSLockStateDisabled   3

static const uint8_t bagUUID[16] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64, 0x00, 0x6b, 0x65, 0x79, 0x62, 0x61, 0x67, 0x00, 0x01 };

static uint64_t gAKSLogged[4];          /* selectors already reported */
static bool     gAKSNotifyState;

/* Experimental: never enabled by default, and never imports real SEP keybags. */
static int gSoftPasscode;
static IOLock *gSoftLock;
static struct swkb_state gSoftBags[16];
static bool gSoftUsed[16];
static IOService *gSoftService;

/* Touch ID unlock rules (as iOS; after redfin 1198/1202, MIT): a passcode since boot, at most 48 h ago,
 * fewer than 5 failed matches, no passcode backoff running. Used by RedwoodMesa.cpp. */
#define BIO_TTL_S       (48 * 3600)
#define BIO_MAX_FAILS   5
static uint64_t gBioPasscodeAt;
static unsigned gBioFails, gBioUnlocks, gBioRefused;

/* iOS 16.5 MKBEventsRegister observes AppleKeyStore's IOGeneralInterest.
 * aksNotificationCB maps d00d to state-changed and d00f to first-unlock.
 * A user-client notification port is a separate keybagd backup interface. */
static const UInt32 kSoftStateChanged = 0xe3ffd00d;
static const UInt32 kSoftFirstUnlock = 0xe3ffd00f;

static uint64_t
softNow(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_SEC;
}

static void
softRandom(void *out, size_t size)
{
	read_random(out, (u_int)size);
}

static void
softHmac(const void *key, size_t keySize, const void *data, size_t dataSize, uint8_t out[32])
{
	g_crypto_funcs->cchmac_fn(g_crypto_funcs->ccsha256_di, keySize, key, dataSize, data, out);
}

static IOReturn
softResult(int result)
{
	return result == SWKB_OK ? kIOReturnSuccess : result == SWKB_BACKOFF ? kIOReturnNotReady :
	    result == SWKB_BAD_RECORD ? kIOReturnBadArgument : kIOReturnNotPermitted;
}

/* User pointers are scalars in this version of libaks, not kernel pointers. */
static bool
softCopySecret(uint64_t address, uint64_t size, uint8_t out[SWKB_MAX_SECRET])
{
	return size <= SWKB_MAX_SECRET && (!size || (address && copyin((user_addr_t)address, out, (size_t)size) == 0));
}


/* Append SEQUENCE { UTF8String key, INTEGER value } for a value below 128; returns the new length. */
static size_t
derPut(uint8_t * buf, size_t length, const char * key, uint8_t value)
{
	size_t keyLength = strlen(key);

	buf[length++] = 0x30;
	buf[length++] = (uint8_t) (keyLength + 5);
	buf[length++] = 0x0c;
	buf[length++] = (uint8_t) keyLength;
	memcpy(buf + length, key, keyLength);
	length += keyLength;
	buf[length++] = 0x02;
	buf[length++] = 0x01;
	buf[length++] = value;
	return length;
}

/* The answer to "device state": every field zero, except the two that say "no passcode, unlocked". */
static size_t
deviceState(uint8_t * buf, const struct swkb_state *state = NULL)
{
	size_t length = 2;
	uint64_t now = state ? softNow() : 0;
	uint8_t backoff = state && state->retry_after > now ? (uint8_t)(state->retry_after - now) : 0;

	length = derPut(buf, length, "sb", backoff);
	length = derPut(buf, length, "sr", 0);
	length = derPut(buf, length, "ss", state ? (uint8_t)swkb_flags(state) : kAKSStateFlags);
	length = derPut(buf, length, "sas", 0);         /* assertion set */
	length = derPut(buf, length, "sfa", state ? (uint8_t)(state->failures > 127 ? 127 : state->failures) : 0);
	length = derPut(buf, length, "sgs", 0);         /* generation state */
	length = derPut(buf, length, "sls", state ? (state->record.has_secret ? (state->locked ? 1 : 0) : 3) : kAKSLockStateDisabled);
	length = derPut(buf, length, "sgpe", 0);        /* grace period enabled */
	length = derPut(buf, length, "srcd", 0);        /* recovery countdown */
	buf[0] = 0x31;
	buf[1] = (uint8_t) (length - 2);
	return length;
}

static size_t
configuration(uint8_t * buf)
{
	size_t length = 2;

	length = derPut(buf, length, "bd", 0);          /* backoff delay */
	length = derPut(buf, length, "gp", 0);          /* grace period */
	length = derPut(buf, length, "mua", 10);        /* maximum unlock attempts */
	buf[0] = 0x31;
	buf[1] = (uint8_t) (length - 2);
	return length;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class AppleKeyStore : public IOService
{
	OSDeclareDefaultStructors(AppleKeyStore);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStore, IOService);

bool
AppleKeyStore::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleKeyStoreUserClient");
	setProperty("RedwoodSoftwarePasscode", gSoftPasscode != 0);
	setName("AppleKeyStore");
	gSoftService = this;
	registerService();
	return true;
}

class AppleKeyStoreUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleKeyStoreUserClient);

	static IOReturn answer(IOExternalMethodArguments * args, const uint8_t * bytes, size_t length);
	static IOReturn softwareMethod(uint32_t selector, IOExternalMethodArguments *args);

public:
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient);

IOReturn
AppleKeyStoreUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

/* keybagd asks to be told about keys to back up and bags to save; there never are any. */
IOReturn
AppleKeyStoreUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	releaseNotificationPort(port);
	return kIOReturnSuccess;
}

/*
 * kAKSWrapKey and kAKSUnwrapKey. A wrapped key is 8 bytes longer than the key, as with AES key
 * wrap (RFC 3394), which is what callers size their buffers for: a mark, the class, and the key
 * mixed with a fixed pattern. Not encryption: there is no secret here to encrypt with.
 */
static const uint8_t kWrapMark[4] = { 'H', 'K', 'w', 'r' };

static void
wrapMix(uint8_t * bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		bytes[i] ^= (uint8_t)(0x5a + 37 * i);
	}
}

/* A structure answer; a big buffer on the other side arrives as a memory descriptor. */
IOReturn
AppleKeyStoreUserClient::answer(IOExternalMethodArguments * args, const uint8_t * bytes, size_t length)
{
	if (args->structureOutputDescriptor != NULL) {
		IOMemoryDescriptor * out = args->structureOutputDescriptor;
		IOReturn ret;

		if (out->getLength() < length) {
			return kIOReturnNoSpace;
		}
		ret = out->prepare();
		if (ret == kIOReturnSuccess) {
			out->writeBytes(0, bytes, length);
			out->complete();
			args->structureOutputDescriptorSize = (uint32_t) length;
		}
		return ret;
	}
	if (args->structureOutput == NULL || args->structureOutputSize < length) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, bytes, length);
	args->structureOutputSize = (uint32_t) length;
	return kIOReturnSuccess;
}

/* Shape-checked adapters for the observed iOS 16.5 libaks ABI. No secret bytes,
 * verifiers, salts or user pointers are ever printed. Native MobileKeyBag owns
 * serialized bag persistence; this driver does not write files itself. */
IOReturn
AppleKeyStoreUserClient::softwareMethod(uint32_t selector, IOExternalMethodArguments *args)
{
	/* Native keybagd/securityd own persistence and changes. Ordinary clients
	 * may query state or verify a code, but cannot export verifier records,
	 * replace the system bag or set a new passcode through this raw interface. */
	if ((selector == kAKSCreateBag || selector == kAKSSaveBag || selector == kAKSLoadBag ||
	    selector == kAKSUnloadBag || selector == kAKSSetSystem || selector == kAKSChangeSecret ||
	    selector == kAKSChangeSecretEpilogue) &&
	    !kauth_cred_issuser(kauth_cred_get())) return kIOReturnNotPrivileged;
	if (!g_crypto_funcs || !g_crypto_funcs->ccsha256_di || !g_crypto_funcs->cchmac_fn) {
		return kIOReturnNotReady;
	}
	if (!gSoftUsed[0]) {
		swkb_init(&gSoftBags[0], softRandom);
		/* Preserve the existing no-passcode bag identity until a persisted
		 * software bag is explicitly installed. Existing keychain metadata
		 * must not see a new identity just because this backend is enabled. */
		memcpy(gSoftBags[0].record.uuid, bagUUID, sizeof(bagUUID));
		gSoftUsed[0] = true;
	}
	const uint64_t *in = args->scalarInput;
	if (args->scalarInputCount && !in) return kIOReturnBadArgument;
	uint8_t first[SWKB_MAX_SECRET] = {}, second[SWKB_MAX_SECRET] = {};
	struct SecretWipe {
		uint8_t *a, *b;
		~SecretWipe() { swkb_clear(a, SWKB_MAX_SECRET); swkb_clear(b, SWKB_MAX_SECRET); }
	} wipe = { first, second };
	if (selector == kAKSCreateBag || selector == kAKSLoadBag) {
		if (!args->scalarOutput || args->scalarOutputCount < 1) return kIOReturnBadArgument;
		unsigned slot;
		for (slot = 1; slot < 16 && gSoftUsed[slot]; slot++) {}
		if (slot == 16) return kIOReturnNoResources;
		if (selector == kAKSCreateBag) {
			if (args->scalarInputCount != 6 || in[4] || in[5] || !softCopySecret(in[2], in[3], first)) return kIOReturnBadArgument;
			swkb_init(&gSoftBags[slot], softRandom);
			int result = swkb_change(&gSoftBags[slot], softHmac, softRandom, NULL, 0, first, (size_t)in[3], softNow());
			if (result != SWKB_OK) return softResult(result);
		} else {
			struct swkb_record record;
			if (args->structureInputDescriptor) {
				if (args->structureInputDescriptor->getLength() != sizeof(record) ||
				    args->structureInputDescriptor->readBytes(0, &record, sizeof(record)) != sizeof(record)) return kIOReturnBadArgument;
			} else {
				if (!args->structureInput || args->structureInputSize != sizeof(record)) return kIOReturnBadArgument;
				memcpy(&record, args->structureInput, sizeof(record));
			}
			int result = swkb_load(&gSoftBags[slot], &record, sizeof(record));
			swkb_clear(&record, sizeof(record));
			if (result != SWKB_OK) return softResult(result);
		}
		gSoftUsed[slot] = true;
		args->scalarOutput[0] = slot;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;
	}
	if (selector == kAKSGetSystem) {
		if (!args->scalarOutput || args->scalarOutputCount < 1) return kIOReturnBadArgument;
		args->scalarOutput[0] = 0;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;
	}
	if (!in || args->scalarInputCount < 1 || in[0] >= 16 || !gSoftUsed[in[0]]) return kIOReturnBadArgument;
	unsigned slot = (unsigned)in[0];
	struct swkb_state *bag = &gSoftBags[slot];
	switch (selector) {
	case kAKSSaveBag:
		return answer(args, (const uint8_t *)&bag->record, sizeof(bag->record));
	case kAKSUnloadBag:
		if (!slot) return kIOReturnNotPermitted;
		swkb_clear(bag, sizeof(*bag));
		gSoftUsed[slot] = false;
		return kIOReturnSuccess;
	case kAKSSetSystem:
		if (!kauth_cred_issuser(kauth_cred_get())) return kIOReturnNotPrivileged;
		if (args->scalarInputCount != 4 || in[1] != 0) return kIOReturnBadArgument;
		if (in[3]) {
			if (!softCopySecret(in[2], in[3], first)) return kIOReturnBadArgument;
			int result = swkb_unlock(bag, softHmac, first, (size_t)in[3], softNow());
			if (result != SWKB_OK) return softResult(result);
		}
		gSoftBags[0] = *bag;
		return kIOReturnSuccess;
	case kAKSGetLockState:
		if (!args->scalarOutput || args->scalarOutputCount < 1) return kIOReturnBadArgument;
		args->scalarOutput[0] = swkb_flags(bag);
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;
	case kAKSGetDeviceState:
	case kAKSGetExtendedState: {
		uint8_t der[128];
		return answer(args, der, deviceState(der, bag));
	}
	case kAKSGetBagUUID:
		return answer(args, bag->record.uuid, sizeof(bag->record.uuid));
	case kAKSLockDevice:
	case kAKSLockDeviceNow:
		swkb_lock(bag);
		return kIOReturnSuccess;
	case kAKSUnlockDevice:
	case kAKSUnlockBag:
		if (args->scalarInputCount != 3 || !softCopySecret(in[1], in[2], first)) return kIOReturnBadArgument;
		return softResult(swkb_unlock(bag, softHmac, first, (size_t)in[2], softNow()));
	case kAKSChangeSecretEpilogue:
		/* Native MobileKeyBag calls this only after saving and installing
		 * the candidate. Verify its new secret against the installed bag. */
		if (args->scalarInputCount != 3 || !softCopySecret(in[1], in[2], first)) return kIOReturnBadArgument;
		return softResult(swkb_check(bag, softHmac, first, (size_t)in[2], softNow()));
	case kAKSChangeSecret: {
		/* scalars: handle, flags, se-type, old ptr/len, new ptr/len,
		 * optional SE secrets ptr/len and mode. SE-derived secrets are unsupported. */
		if (args->scalarInputCount != 11 || in[7] || in[8] || in[9] || in[10] ||
		    !softCopySecret(in[3], in[4], first) || !softCopySecret(in[5], in[6], second)) return kIOReturnBadArgument;
		if ((in[1] & ~4ULL) || (in[2] != 255 && in[2] != 0)) return kIOReturnUnsupported;
		if (args->scalarOutputCount < 1 || !args->scalarOutput) return kIOReturnBadArgument;
		/* Flag 4 asks for a separate candidate bag: MobileKeyBag persists it
		 * before installing it as the system bag. Never mutate the current
		 * system verifier before that commit. */
		unsigned destination = slot;
		if (in[1] & 4) {
			for (destination = 1; destination < 16 && gSoftUsed[destination]; destination++) {}
			if (destination == 16) return kIOReturnNoResources;
		}
		struct swkb_state candidate = *bag;
		int result = swkb_change(&candidate, softHmac, softRandom, first, (size_t)in[4], second, (size_t)in[6], softNow());
		if (result != SWKB_OK) {
			bag->failures = candidate.failures;
			bag->retry_after = candidate.retry_after;
			swkb_clear(&candidate, sizeof(candidate));
			return softResult(result);
		}
		gSoftBags[destination] = candidate;
		gSoftUsed[destination] = true;
		swkb_clear(&candidate, sizeof(candidate));
		args->scalarOutput[0] = destination;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;
	}
	default:
		return kIOReturnUnsupported;
	}
}

IOReturn
AppleKeyStoreUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	uint8_t der[128];
	uint64_t * out = args->scalarOutput;

	if (gSoftPasscode && (selector == kAKSCreateBag || selector == kAKSSaveBag || selector == kAKSLoadBag ||
	    selector == kAKSUnloadBag || selector == kAKSSetSystem || selector == kAKSGetSystem ||
	    selector == kAKSChangeSecret || selector == kAKSChangeSecretEpilogue || selector == kAKSGetLockState || selector == kAKSGetDeviceState ||
	    selector == kAKSGetExtendedState || selector == kAKSGetBagUUID || selector == kAKSLockDevice ||
	    selector == kAKSLockDeviceNow || selector == kAKSUnlockDevice || selector == kAKSUnlockBag)) {
		IOLockLock(gSoftLock);
		unsigned previousFlags = gSoftUsed[0] ? swkb_flags(&gSoftBags[0]) : kAKSStateFlags;
		unsigned previousFailures = gSoftBags[0].failures;
		uint64_t previousRetry = gSoftBags[0].retry_after;
		IOReturn result = softwareMethod(selector, args);
		unsigned currentFlags = gSoftUsed[0] ? swkb_flags(&gSoftBags[0]) : kAKSStateFlags;
		bool changed = previousFlags != currentFlags || previousFailures != gSoftBags[0].failures ||
		    previousRetry != gSoftBags[0].retry_after;
		bool firstUnlock = !(previousFlags & 4) && (currentFlags & 4);
		/* Touch ID (RedwoodMesa): a passcode that unlocked the system bag starts the 48 h biometric window */
		if (result == kIOReturnSuccess && (previousFlags & 1) && !(currentFlags & 1) &&
		    (selector == kAKSUnlockDevice || selector == kAKSUnlockBag || selector == kAKSSetSystem)) {
			gBioPasscodeAt = softNow();
			gBioFails = 0;
		}
		IOLockUnlock(gSoftLock);
		/* Never call observers while holding the bag lock: consumers query
		 * the state again. No timer or periodic polling is required. */
		if (gSoftService && changed) gSoftService->messageClients(kSoftStateChanged);
		if (gSoftService && firstUnlock) gSoftService->messageClients(kSoftFirstUnlock);
		return result;
	}

	switch (selector) {
	case kAKSOpenCheck:
	case kAKSLockDevice:
	case kAKSLockDeviceNow:
	case kAKSUnlockDevice:
	case kAKSUnlockBag:
	case kAKSAssertHold:
	case kAKSAssertDrop:
	case kAKSAssertPromote:
	case kAKSAssertConsume:
	case kAKSPrewarm:
		return kIOReturnSuccess;

	case kAKSGetLockState:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kAKSStateFlags;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kAKSGetSystem:
	case kAKSGeneration:
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		return kIOReturnSuccess;

	case kAKSGetDeviceState:
	case kAKSGetExtendedState:
		return answer(args, der, deviceState(der));

	case kAKSGetConfiguration:
		return answer(args, der, configuration(der));

	case kAKSGetBagUUID:
		return answer(args, bagUUID, sizeof(bagUUID));

	case kAKSNotifyStateSet:
		gAKSNotifyState = true;
		return kIOReturnSuccess;

	case kAKSWrapKey: {
		uint8_t wrapped[8 + 64];
		size_t length = args->structureInputSize;

		if (args->structureInput == NULL || length == 0 || length > 64 || args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		memcpy(wrapped, kWrapMark, sizeof(kWrapMark));
		wrapped[4] = (uint8_t)args->scalarInput[1];
		wrapped[5] = wrapped[6] = wrapped[7] = 0;
		memcpy(wrapped + 8, args->structureInput, length);
		wrapMix(wrapped + 8, length);
		if (args->scalarOutputCount >= 1) {
			out[0] = args->scalarInput[1];
			args->scalarOutputCount = 1;
		}
		return answer(args, wrapped, 8 + length);
	}

	case kAKSUnwrapKey: {
		uint8_t key[64];
		const uint8_t * wrapped = (const uint8_t *)args->structureInput;
		size_t length = args->structureInputSize;

		if (wrapped == NULL || length <= 8 || length > 8 + sizeof(key) || memcmp(wrapped, kWrapMark, sizeof(kWrapMark)) != 0) {
			return kIOReturnBadArgument;
		}
		memcpy(key, wrapped + 8, length - 8);
		wrapMix(key, length - 8);
		return answer(args, key, length - 8);
	}

	case kAKSNotifyStateIsSet:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = gAKSNotifyState;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 256 && !(gAKSLogged[selector / 64] & (1ULL << (selector % 64)))) {
		char procname[32] = "?";

		gAKSLogged[selector / 64] |= 1ULL << (selector % 64);
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniAppleKeyStore: %s[%d] selector 0x%x not implemented (scalars %u in, %u out; structure %u in, %u out)\n",
		    procname, proc_selfpid(), selector, args->scalarInputCount, args->scalarOutputCount, args->structureInputSize,
		    args->structureOutputSize);
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void
MiniAppleKeyStoreStart(void)
{
	PE_parse_boot_argn("softpasscode", &gSoftPasscode, sizeof(gSoftPasscode));
	if (gSoftPasscode) {
		gSoftLock = IOLockAlloc();
		if (!gSoftLock) {
			IOLog("MiniAppleKeyStore: software passcode initialization failed\n");
			return;
		}
		/* MobileKeyBag's native software-storage path checks for this
		 * capability in IODeviceTree:/defaults. The port has no Effaceable
		 * Storage, so publish its absence instead of fabricating wipe IDs. */
		IORegistryEntry *defaults = IORegistryEntry::fromPath("/defaults", gIODTPlane);
		if (defaults) {
			defaults->setProperty("no-effaceable-storage", kOSBooleanTrue);
			defaults->release();
		}
		IOLog("MiniAppleKeyStore: experimental software authentication enabled; no SEP/data encryption\n");
	}
	/* user space looks for it by class, and by the path IOService:/IOResources/AppleKeyStore */
	IOService * resources = IOService::getResourceService();
	AppleKeyStore * store = OSTypeAlloc(AppleKeyStore);

	if (resources == NULL || store == NULL || !store->init() || !store->attach(resources)) {
		OSSafeReleaseNULL(store);
		IOLog("MiniAppleKeyStore: cannot create the service\n");
		return;
	}
	if (!store->start(resources)) {
		store->detach(resources);
	}
	store->release();
}

/* ---- Touch ID hooks for RedwoodMesa.cpp ---- */
static const char *
bioRefusal(uint64_t now)
{
	if (!gBioPasscodeAt) return "passcode required after restart";
	if (now - gBioPasscodeAt > BIO_TTL_S) return "passcode required (48 h since the last passcode)";
	if (gBioFails >= BIO_MAX_FAILS) return "passcode required (5 failed Touch ID attempts)";
	if (gSoftBags[0].retry_after > now) return "passcode backoff";
	return NULL;
}

/* 1 = a match may unlock now; else 0 and *why. No passcode set: allowed, nothing to unlock. */
extern "C" int
redwood_bio_allowed(const char **why)
{
	*why = "";
	if (!gSoftPasscode || !gSoftLock) return 1;
	IOLockLock(gSoftLock);
	const char *r = gSoftUsed[0] && gSoftBags[0].record.has_secret ? bioRefusal(softNow()) : NULL;
	IOLockUnlock(gSoftLock);
	if (r) *why = r;
	return r == NULL;
}

/* A verified match: unlock the system bag under the rules. 0 = allowed (*did_unlock 1 if it was locked), EPERM. */
extern "C" int
redwood_bio_unlock(int *did_unlock, const char **why)
{
	*did_unlock = 0;
	*why = "";
	if (!gSoftPasscode || !gSoftLock) return 0;
	IOLockLock(gSoftLock);
	const char *r = NULL;
	bool changed = false;
	if (gSoftUsed[0] && gSoftBags[0].record.has_secret) {
		r = bioRefusal(softNow());
		if (r) {
			gBioRefused++;
		} else {
			gBioFails = 0;
			if (gSoftBags[0].locked) {
				gSoftBags[0].locked = 0;
				changed = true;
				gBioUnlocks++;
			}
		}
	}
	IOLockUnlock(gSoftLock);
	if (r) {
		*why = r;
		IOLog("AppleKeyStore: Touch ID unlock refused: %s\n", r);
		return EPERM;
	}
	if (changed) {
		*did_unlock = 1;
		IOLog("AppleKeyStore: Touch ID unlocked the keybag\n");
		if (gSoftService) gSoftService->messageClients(kSoftStateChanged);
	}
	return 0;
}

extern "C" void
redwood_bio_failed(void)
{
	if (!gSoftPasscode || !gSoftLock) return;
	IOLockLock(gSoftLock);
	unsigned f = ++gBioFails;
	IOLockUnlock(gSoftLock);
	IOLog("AppleKeyStore: Touch ID no match %u/%d%s\n", f, BIO_MAX_FAILS, f >= BIO_MAX_FAILS ? ": passcode required" : "");
}

/* LocalAuthentication's "biometry matched" credential (redfin's SweetACM): nothing to arm here yet. */
extern "C" void
redwood_bio_note_verified_match(void)
{
}

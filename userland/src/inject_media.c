/*
 * libredwood-media.dylib: code of ours inside mediaserverd (see inject.c for how it gets there).
 *
 * mediaserverd is the audio server, and also the camera's: the capture stack of CMCapture lives
 * in the same process. Soon after it starts, that stack "prewarms" the Core Image filters of the
 * portrait mode, for which Core Image wants an OpenGL ES context; there is no GPU, the context
 * has no functions behind it, and the daemon jumps to address 0 -- every time launchd restarts
 * it. Nobody is going to take a portrait here: the prewarming is skipped.
 *
 * What the daemon is wanted for meanwhile is neither sound nor the camera: it is where
 * VideoToolbox decodes for everybody else (the service com.apple.coremedia.decompressionsession),
 * and a HEIC picture is HEVC inside. Without the daemon ImageIO has "no way to decode a HEIC",
 * and the stock wallpaper of iOS 16, whose layers are four such pictures, is its gradient alone.
 * There is no hardware decoder here; VideoToolbox takes its software one (VCPHEVC.videocodec).
 *
 * The flashlight (only where the kernel has one: torch=3, /dev/redwood-torch): Control Center's
 * button and AVFlashlight reach FigFlashlight in here, which asks the capture stack's device vendor
 * (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the camera device and its back
 * camera's stream, and sets the stream's "TorchLevel" (0 to 1). Without a camera there is no
 * vendor (it is nil), every message to it does nothing, and FigFlashlight says it has set the level
 * when nothing has been. So where there is no vendor, FigFlashlight -- and only FigFlashlight,
 * by where it calls from -- is given one of ours, with a device and a stream whose torch level goes
 * to the kernel (see flashlight_*() below). Everybody else still gets none, as before.
 *
 * The camera (only where the kernel has one: cam=5, /dev/redwood-camera): the vendor is made for
 * everybody, with the create function of our capture device (camera_device.c) in place of the
 * one H10ISP.mediacapture would give for the iPhone's ISP; the capture graph is built around it and
 * its frames reach Camera.app's preview (see camera_device.c). Its stream takes the torch level too,
 * so FigFlashlight would need nothing of its own then (not tried yet).
 *
 * What the capture stack asks of hardware this phone has none of is refused here: there is no ISP
 * processing session, so there can be no video stabilization (see video_stabilization_method below).
 *
 * CMCapture is not loaded yet when this library is: the methods are replaced when its image
 * arrives.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- the flashlight ------------------------------------------------------------------------------- */

static int torch = -1;                  /* /dev/redwood-torch */
static bool camera;                     /* /dev/redwood-camera is there */
extern int redwood_camera_device_create(const void *allocator, const void *options, void **out);
static Class vendor_class, device_class, stream_class;
static IMP original_shared_vendor;
static unsigned torch_said;

/*
 * FigFlashlight's functions that ask for the vendor, unslid (20F66): flashlight_IsAvailable,
 * _NotifyForCurrentState and _setupDevice lie together, _teardownDevice apart.
 */
static const struct { uintptr_t from, to; } flashlight_code[] = {
	{ 0x1a1fb0830UL, 0x1a1fb0c5cUL }, { 0x1a1eee5c0UL, 0x1a1eee6e8UL },
};

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor] */
static id
shared_vendor(id self, SEL selector)
{
	uintptr_t caller = (uintptr_t)__builtin_return_address(0) - slide;
	id vendor = ((id (*)(id, SEL))original_shared_vendor)(self, selector);
	static id ours, camera_vendor;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (vendor != NULL) {
		return vendor;
	}
	if (!camera && access("/dev/redwood-camera", F_OK) == 0) {
		camera = true;          /* (should the kernel have made it after mediaserverd started) */
		say("redwood: mediaserverd: the kernel has a camera (/dev/redwood-camera)\n");
	}
	if (camera) {
		pthread_mutex_lock(&lock);
		if (camera_vendor == NULL) {
			id made = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));

			camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made, sel_registerName("initWithDeviceCreateFunction:"),
			    (void *)redwood_camera_device_create);
			say("redwood: mediaserverd: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
			    "CANNOT BE MADE");
		}
		pthread_mutex_unlock(&lock);
		return camera_vendor;
	}
	for (size_t i = 0; torch >= 0 && i < sizeof(flashlight_code) / sizeof(flashlight_code[0]); i++) {
		if (caller >= flashlight_code[i].from && caller < flashlight_code[i].to) {
			if (ours == NULL) {
				ours = class_createInstance(vendor_class, 0);
				say("redwood: mediaserverd: no camera, so no device vendor: FigFlashlight gets the flashlight's own\n");
			}
			return ours;
		}
	}
	return NULL;
}

static void
flashlight_set(uint32_t level)
{
	ssize_t written = write(torch, &level, sizeof(level));

	if (written != (ssize_t)sizeof(level) || torch_said < 40) {
		torch_said++;
		say("redwood: mediaserverd: the flashlight at %u of 1000%s\n", level, written == (ssize_t)sizeof(level) ? "" :
		    " (NOT TAKEN by /dev/redwood-torch)");
	}
}

/* the vendor's: one client, one device, always available to it */
static int
flashlight_register(__unused id self, __unused SEL selector, __unused int pid, __unused id description, __unused int priority,
    __unused BOOL steal, __unused BOOL share, __unused id handler)
{
	return 1;
}

static id
flashlight_copy_device(__unused id self, __unused SEL selector, __unused int client, __unused BOOL inform, int *error)
{
	if (error != NULL) {
		*error = 0;
	}
	return class_createInstance(device_class, 0);
}

static id
flashlight_copy_stream(__unused id self, __unused SEL selector, __unused int position, __unused int type, id device)
{
	return device != NULL && object_getClass(device) == device_class ? class_createInstance(stream_class, 0) : NULL;
}

static void
flashlight_take_back(__unused id self, __unused SEL selector, __unused id device, __unused int client, __unused BOOL inform)
{
	flashlight_set(0);
}

static BOOL
flashlight_active_equals(__unused id self, __unused SEL selector, id device)
{
	return device == NULL || object_getClass(device) == device_class;
}

/* the stream's -supportedProperties: none that FigFlashlight looks for ("HighCurrentTorchEnabled") */
static id
flashlight_stream_properties(__unused id self, __unused SEL selector)
{
	static CFDictionaryRef none;

	if (none == NULL) {
		none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return (id)none;
}

/* the stream's -setProperty:value: -- "TorchLevel", 0 to 1, as thousandths to the kernel */
static int
flashlight_stream_set(__unused id self, __unused SEL selector, id key, id value)
{
	float level = 0;

	if (key != NULL && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
		if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
		}
		level = level < 0 ? 0 : level > 1 ? 1 : level;
		flashlight_set((uint32_t)(level * 1000 + 0.5f));
	}
	return 0;
}

/*
 * Video stabilization, of which this hardware has none: the capture graph's VIS node
 * (-[FigCaptureVISPipeline _newVISNodeWithUpstreamOutput:...]) is made with the ISP's processing
 * session, and this device has no such session, so the node comes back nil, the VIS pipeline with
 * it, the movie file sink's tail pipeline with that, and captureSession_buildGraphWithConfiguration
 * tears the whole graph down -- the preview too, for all that the preview has nothing to do with
 * stabilizing a recording. The connection's stabilization method is where the whole stack reads the
 * wish from (+[FigVideoCaptureConnectionConfiguration videoStabilizationMethods:includeIris:] ->
 * cspc_getVideoStabilizationType -> the connections' FigCaptureVideoStabilizationType), and 0 there
 * means none: the graph is then built without a VIS pipeline anywhere
 * (+[FigCaptureVISPipeline visPipelineIsRequiredForFigCaptureVideoStabilizationType:] says no to
 * every type but 2 and 3), and recordings are as steady as the hand that holds the phone.
 */
static int
video_stabilization_method(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;
}

/*
 * The type the stack acts on. The connection configuration arrives from the application's
 * AVFoundation with the type in it, which is read straight out of the object and not computed from
 * the method above (-[FigCaptureSessionParsedConfiguration _computeVideoStabilizationTypes] only
 * fills in what the client left out), so the wish has to be refused here as well.
 */
static int
video_stabilization_type(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;                       /* kFigCaptureVideoStabilizationType_None */
}

/*
 * The microphone, which the capture stack has none of here. A capture session's sound comes through
 * CoreMedia's own capture audio session -- BWAudioSourceNode's start sets its properties and reads
 * its pull format, all of it through a brokered plug-in of the audio HAL -- and that plug-in is not
 * on this phone: the node's start comes back with 'nope', the HAL's word for "no such thing".
 *
 * Camera.app's session has an audio input in it, because it is a session for photographs and for
 * video both. The graph starts its source nodes late (startDeferredSourceNodesIfNeeded) and takes
 * the first error it gets for the whole graph's, so the session reported AVCaptureSessionRuntimeError
 * to the application, which took the session down and built it again, two seconds later, for ever:
 * the camera's preview came in bursts of five frames with the sensor powering down in between.
 *
 * The node is told that it started. It then produces no sound -- which is all a phone with no
 * capture audio has to offer -- and the rest of the graph, the preview and the photograph, runs.
 * Only that one error is swallowed: a microphone that is there and fails for another reason still
 * takes the session down, as it should. (The kernel's own microphone, adsp=1 mic=4, is a different
 * path: AVAudioRecorder and Voice Memos record through it, and nothing here touches it.)
 */
#define ERROR_NO_SUCH_THING     1852797029      /* 'nope' */
static void audio_watch_start(id o);
extern void objc_release(id object);

static IMP original_audio_source_start;

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	BOOL started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
	id made = error != NULL ? *error : NULL;
	static bool said;

	say("redwood: audio source: start -> %d (error %ld)\n", (int)started,
	    made != NULL ? ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) : 0L);
	/*
	 * The node fills a gap in the microphone's times with silence when the I/O's host time jumps past the one
	 * it expected by more than _ioprocTimeStampDeltaLimit. Here the times of the HAL plug-in's device (sample
	 * time from its start, host time the system's) made it see a gap of 430 million frames on every buffer: it
	 * zeroed 1.7 GB for each, never passed one sound buffer on, the recording's movie writer waited for sound
	 * for ever, no video was ever finished (2026-11-12). No gap is filled: the limit is out of reach.
	 */
	{
		Ivar v = class_getInstanceVariable(object_getClass(self), "_ioprocTimeStampDeltaLimit");

		if (v != NULL) {
			CMTime *limit = (CMTime *)((uint8_t *)self + ivar_getOffset(v));

			say("redwood: audio source: gap limit %.3f s -> none\n", CMTIME_IS_VALID(*limit) ? CMTimeGetSeconds(*limit) : -1.0);
			*limit = CMTimeMake(INT64_MAX / 2, 1);
		}
	}
	audio_watch_start(self);

	if (started || made == NULL || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	*error = NULL;                  /* (autoreleased by the capture stack: not ours to give back) */
	if (!said) {
		said = true;
		say("redwood: mediaserverd: there is no capture audio on this phone: the session's audio source gives nothing, "
		    "and the camera runs without it\n");
	}
	return true;
}

/*
 * A photograph's quality prioritization, "speed" at the most. Asked for "balanced" (Camera.app's) or
 * "quality", the capture device takes a scene for a low-light one whenever a frame was exposed at
 * least as long as the sensor's still image stabilization threshold
 * (-[BWFigVideoCaptureDevice _isLowLightSceneUsingFrameStatisticsByPortType:]) -- a threshold from
 * the sensor's tuning in CameraSetup.plist, 0 for a sensor it has no tuning for, as ours -- and
 * then asks the stream for a bracket of four frames for SIS fusion (captureType 4, BWSISNode), which
 * this port has nothing to fuse with: the photograph never came. With "speed" it asks for one frame
 * (-_resolveStillImageCaptureTypeFromStatisticsByPortType:..., captureType 1), which the stream
 * gives (camera_device.c, StillImageCaptureNow).
 */
static IMP original_quality_prioritization;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
	static bool said;

	if (asked > 1 && !said) {
		said = true;
		say("redwood: mediaserverd: a photograph's quality prioritization %d taken as 1 (speed): one frame, no SIS bracket\n",
		    asked);
	}
	return asked > 1 ? 1 : asked;
}

/*
 * A photograph's noise reduction (BWGNRNode, between the still image coordinator and the photo
 * encoder) is a FigSampleBufferProcessor of the iPhone's, which is handed the picture and never
 * hands one back here: the photograph stopped there, with no error, and the request with it (the
 * shutter's second press asked for nothing more). The picture goes past it to the node's output as
 * it came, as with no noise reduction to do.
 */
static void
gnr_render(id self, SEL selector, void *sample_buffer, id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
	static bool said;

	(void)selector, (void)input;
	if (!said) {
		said = true;
		say("redwood: mediaserverd: a photograph goes past the noise reduction, which has nothing to do it with here\n");
	}
	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

static bool
is_node(id node, const char *name)
{
	return node != nil && strcmp(object_getClassName(node), name) == 0;
}

/*
 * How long a photograph's encoding took, from the picture handed to the photo encoder node to the one
 * it hands on (HEVC in software, see register_hevc_encoder below: the slowest step of a photograph).
 */
static void
time_photo_encoder(id output, CMSampleBufferRef sample_buffer)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
	static uint64_t began;
	static mach_timebase_info_data_t timebase;
	id connection, input;

	if (image == NULL || CVPixelBufferGetWidth(image) < 2000) {
		return;                 /* not a photograph */
	}
	if (is_node(((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node")), "BWPhotoEncoderNode")) {
		if (began != 0) {
			if (timebase.denom == 0) {
				mach_timebase_info(&timebase);
			}
			say("redwood: mediaserverd: a photograph of %zux%zu encoded in %.0f ms\n", CVPixelBufferGetWidth(image),
			    CVPixelBufferGetHeight(image), (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1e6);
		}
		began = 0;
		return;
	}
	connection = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("connection"));
	input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
	if (input != nil && is_node(((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")), "BWPhotoEncoderNode")) {
		began = mach_absolute_time();
	}
}

/*
 * A node's output given no sample buffer at all, which no node takes: the session's audio source
 * (-[BWAudioSourceNode _generateSamples]), whose audio unit never started here (see audio_source_start
 * above), fills the silence by a timer with buffers it fails to make and emits them all the same --
 * NULL. Live Photo's staging node added that to an array (-[BWIrisStagingNode renderSampleBuffer:forInput:])
 * and mediaserverd died of the NSInvalidArgumentException, every half a minute with Live Photo on, the
 * preview black for a second each time. A NULL is not passed on.
 */
static IMP original_emit_sample_buffer;
static const char *node_of(id port);

static void
emit_sample_buffer(id self, SEL selector, CMSampleBufferRef sample_buffer)
{
	static bool said;

	if (sample_buffer == NULL) {
		if (!said) {
			said = true;
			say("redwood: mediaserverd: a node's output was given no sample buffer (the silent audio source's): not passed on\n");
		}
		return;
	}
	time_photo_encoder(self, sample_buffer);
	{
		static uint64_t audio_emits;
		id node = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("node"));

		if (node != nil && strcmp(object_getClassName(node), "BWIrisStagingNode") == 0) {
			static uint64_t iris_emits;

			if (iris_emits++ % 30 == 0) {
				say("redwood: live photo: the staging node emits %llu (pts %.3f)\n", iris_emits,
				    CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample_buffer)));
			}
		}
		if (node != nil && strcmp(object_getClassName(node), "BWAudioSourceNode") == 0 && audio_emits++ % 100 == 0) {
			CMTime t = CMSampleBufferGetPresentationTimeStamp(sample_buffer);
			id conn = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("connection"));
			id fmt = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("liveFormat"));
			id to = conn != nil ? ((id (*)(id, SEL))objc_msgSend)(conn, sel_registerName("input")) : nil;

			say("redwood: audio source: emit %llu: pts %.3f, %ld samples, live format %s, to %s\n", audio_emits - 1,
			    CMTimeGetSeconds(t), (long)CMSampleBufferGetNumSamples(sample_buffer), fmt != nil ? "yes" : "NO",
			    to != nil ? node_of(to) : "nothing");
		}
	}
	((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, sample_buffer);
}

/*
 * The photograph's encoder. Camera.app asks for HEIC -- HEVC inside a HEIF file, which CMPhoto makes
 * through VideoToolbox -- and VideoToolbox here has no HEVC encoder but the iPhone's hardware ones
 * (com.apple.videotoolbox.videoencoder.hevc and .hevc.gva, behind its AVE): the photo encoder node
 * failed with kVTCouldNotFindVideoEncoderErr (-12908). Apple's software HEVC codec is in the system
 * all the same, VCPHEVC.videocodec, whose decoder VideoToolbox takes here for every HEIC picture (see
 * the top), and there is an encoder in it, HEVCVideoEncoder_CreateInstance, that iOS never registers:
 * the codec's VCPHEVCRegisterEncoder is an empty function. It is registered here, as the codec
 * registers its decoder (VTRegisterVideoDecoderWithInfo, not hardware accelerated).
 */
static void
register_hevc_encoder(void)
{
	void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
	int (*register_encoder)(uint32_t, CFDictionaryRef, void *) = vt != NULL ? dlsym(vt, "VTRegisterVideoEncoderWithInfo") : NULL;
	CFStringRef *id_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderID") : NULL;
	CFStringRef *name_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderName") : NULL;
	CFStringRef *hardware_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated") : NULL;
	int status;

	if (create == NULL || register_encoder == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		say("redwood: mediaserverd: no software HEVC encoder to register (%s)\n", codec == NULL ? "no VCPHEVC.videocodec" :
		    create == NULL ? "no HEVCVideoEncoder_CreateInstance" : "VideoToolbox without VTRegisterVideoEncoderWithInfo");
		return;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
		kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);

	status = register_encoder('hvc1', info, create);
	CFRelease(info);
	say("redwood: mediaserverd: the software HEVC encoder of VCPHEVC.videocodec registered for 'hvc1' (%d)\n", status);
}

struct method_spec {
	const char *selector, *types;
	IMP imp;
};

static Class
make_class(const char *name, const struct method_spec *methods, size_t count)
{
	Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);

	for (size_t i = 0; i < count; i++) {
		class_addMethod(cls, sel_registerName(methods[i].selector), methods[i].imp, methods[i].types);
	}
	objc_registerClassPair(cls);
	return cls;
}

static void
flashlight_classes(void)
{
	static const struct method_spec vendor[] = {
		{ "registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
		  "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:", "i@:i@iBB@", (IMP)flashlight_register },
		{ "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:", "@@:iB^i", (IMP)flashlight_copy_device },
		{ "copyStreamForFlashlightWithPosition:deviceType:forDevice:", "@@:ii@", (IMP)flashlight_copy_stream },
		{ "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:", "v@:@iB", (IMP)flashlight_take_back },
		{ "activeDeviceEquals:", "B@:@", (IMP)flashlight_active_equals },
	}, stream[] = {
		{ "supportedProperties", "@@:", (IMP)flashlight_stream_properties },
		{ "setProperty:value:", "i@:@@", (IMP)flashlight_stream_set },
	};

	vendor_class = make_class("RedwoodFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
	device_class = make_class("RedwoodFlashlightDevice", NULL, 0);
	stream_class = make_class("RedwoodFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in iOS 16.5 (20F66): a method that is not what it was is left alone. `original`, where
 * there is one, keeps what was there (the vendor's: given as it is wherever there is one, and where
 * there is none, ours only with the kernel's camera or flashlight).
 */
/*
 * Rear recordings in H.264, as the front's. Camera.app's movie settings ask the back camera for HEVC (hvc1),
 * and the one HEVC encoder here is Apple's software one (VCPHEVC, registered for photographs below), far
 * too slow for video: the first compressed frame came 14-28 s late, the Stop never reached the file, rear
 * videos were never finished or came out at ~8 frames a second (2026-11-12). The front's are H.264 (avc1),
 * which libredwood-media's OpenH264 encoder keeps up with. So an HEVC request is answered as H.264.
 */
static IMP original_movie_video_settings;

static CFDictionaryRef
movie_video_settings(id self, SEL selector)
{
	CFDictionaryRef d = ((CFDictionaryRef (*)(id, SEL))original_movie_video_settings)(self, selector);
	CFStringRef codec;

	if (d == NULL || CFGetTypeID(d) != CFDictionaryGetTypeID() ||
	    (codec = CFDictionaryGetValue(d, CFSTR("AVVideoCodecKey"))) == NULL || CFGetTypeID(codec) != CFStringGetTypeID() ||
	    (CFStringCompare(codec, CFSTR("hvc1"), 0) != kCFCompareEqualTo && CFStringCompare(codec, CFSTR("hev1"), 0) != kCFCompareEqualTo)) {
		return d;
	}
	CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(NULL, 0, d);
	CFDictionaryRef props = CFDictionaryGetValue(d, CFSTR("AVVideoCompressionPropertiesKey"));

	CFDictionarySetValue(m, CFSTR("AVVideoCodecKey"), CFSTR("avc1"));
	if (props != NULL && CFGetTypeID(props) == CFDictionaryGetTypeID()) {
		CFMutableDictionaryRef mp = CFDictionaryCreateMutableCopy(NULL, 0, props);

		CFDictionaryRemoveValue(mp, CFSTR("ProfileLevel"));      /* (HEVC_Main_AutoLevel: not an H.264 profile) */
		CFDictionarySetValue(m, CFSTR("AVVideoCompressionPropertiesKey"), mp);
		CFRelease(mp);
	}
	static int said;
	if (said++ < 4) say("redwood: movie: HEVC asked for, H.264 given (the software HEVC encoder cannot keep up)\n");
	return (CFDictionaryRef)CFAutorelease(m);
}

static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement, *original;
	bool instance;                  /* an instance method, not a class method */
	bool done;
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2115c0cUL,
	  (IMP)prewarm_portrait_filter, NULL, false, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0x1a1ef0d04UL,
	  (IMP)shared_vendor, &original_shared_vendor, false, false },
	{ "BWAudioSourceNode", "start:", 0x1a1eefa18UL, (IMP)audio_source_start, &original_audio_source_start, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", 0x1a1e99ef8UL,
	  (IMP)video_stabilization_method, NULL, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", 0x1a20d8f48UL,
	  (IMP)video_stabilization_type, NULL, true, false },
	{ "FigCaptureStillImageSettings", "qualityPrioritization", 0x1a1f533dcUL,
	  (IMP)quality_prioritization, &original_quality_prioritization, true, false },
	{ "BWGNRNode", "renderSampleBuffer:forInput:", 0x1a1ec83b8UL, (IMP)gnr_render, NULL, true, false },
	{ "BWNodeOutput", "emitSampleBuffer:", 0x1a1ea37e0UL, (IMP)emit_sample_buffer, &original_emit_sample_buffer, true, false },
	/* (expected 1: any address -- the getter is only wrapped, and says in the log when it changes something) */
	{ "FigCaptureMovieFileRecordingSettings", "videoSettings", 1, (IMP)movie_video_settings, &original_movie_video_settings,
	  true, false },
};

/* ---- the Metal device leaves this process before the capture stack asks for it -------------------- */

/*
 * With the Metal properties on the accelerator service (boot-arg metal=1) MTLCreateSystemDefaultDevice()
 * answers in every process, and in mediaserverd the first to ask is the capture stack: CMCapture's
 * FigPreloadShadersInternal reaches -[_MTLDevice newLibraryWithURL:error:] through three functions of
 * AppleCVAPhoto, asking for System/Library/PrivateFrameworks/AppleCVAPhoto.framework/Resources/
 * cvadepth-iOS.metallib. That file is not in this image (ios165/select-system.py drops .metallib under
 * the framework directories), so the URL is nil and Metal aborts on `url must not be nil' -- twenty-eight
 * times in one boot, launchd restarting the daemon after each (logs/135751-run.log, docs/metal.md 10.38).
 * AppleCVAPhoto itself survives a library it cannot make: it checks the result and logs "Error creating
 * metal library". What kills the process is Metal's own assertion, and that is reached only because there
 * IS a device to send the message to.
 *
 * So the device is taken out of this process, exactly as inject.c does for the window server: Metal keeps
 * every device it has made in one static NSMutableArray (0x1e1efe9a0, __DATA_DIRTY.__bss, so one per
 * process), and MTLCreateSystemDefaultDevice() is no more than "if that array holds exactly one object,
 * return it" -- an empty array answers nil without a single instruction of Metal's being touched. The
 * array is built on first use, so it has to be asked for first; that makes the device object, which is
 * what the capture stack's next instruction would have done anyway, so the process pays nothing new.
 * The device is retained and never released, for inject.c's reason: what our -dealloc would do to a GPU
 * is not measured.
 *
 * WHEN, and why not where inject.c does it. There the hook is -[CAWindowServer _detectDisplays], the last
 * Objective-C method on the way in and an address that can be checked. Here the way in is three C
 * functions of AppleCVAPhoto with no method of ours to stand on, so this runs from the add-image notifier
 * this library already registers, the first time Metal is in the process at all -- which is before any
 * code of the image that brought it has run. Being early cannot cost correctness: if the device can be
 * made, it is made and removed; if the accelerator service is not matched yet and the array is built
 * empty, nil is what the capture stack gets, and nil is the whole point.
 *
 * Boot-arg mediamtl=1 leaves the device where it is. It is a switch of its own and not inject.c's camtl=1
 * because the two processes do not risk the same thing: a window server that aborts is a crash loop and a
 * black screen, while mediaserverd aborting costs HEIC decoding, the camera and the sound. A boot may
 * reasonably want Metal in one and not the other. Either way the metallib has to be in the image first,
 * or the same assertion comes straight back.
 */
#define MTL_DEVICE_ARRAY        0x1e1efe9a0UL   /* Metal's static NSMutableArray *_deviceArray */

extern id objc_retain(id object);

static bool metal_left_alone;                   /* boot-arg mediamtl=1 */

static bool
boot_arg(const char *what)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	return sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    strstr(arguments, what) != NULL;
}

static void
keep_metal_from_the_capture_stack(void)
{
	static bool done;
	unsigned long (*ask)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	void (*tell)(id, SEL) = (void (*)(id, SEL))objc_msgSend;
	id *slot = (id *)(MTL_DEVICE_ARRAY + slide);
	id (*create)(void);
	id device, array;
	unsigned long held;

	if (done || metal_left_alone) {
		return;
	}
	/* Metal is not in this process yet: the image that brings it has not arrived, and the notifier
	 * will be back here when it does. Nothing is printed for that -- a line per image would bury
	 * the log, and the constructor has already said that this is armed. */
	if ((create = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice")) == NULL) {
		return;
	}
	done = true;
	if ((device = create()) == NULL) {
		say("redwood: mediaserverd: no Metal device in this process, so the capture stack's shader "
		    "preload has nothing to ask\n");
		return;
	}
	if ((array = *slot) == NULL) {
		say("redwood: mediaserverd: a device, but nothing at %#lx; the device list is NOT emptied\n",
		    MTL_DEVICE_ARRAY);
		return;
	}
	if (!((BOOL (*)(id, SEL, Class))objc_msgSend)(array, sel_registerName("isKindOfClass:"),
	    objc_getClass("NSMutableArray"))) {
		say("redwood: mediaserverd: %#lx holds a %s, not Metal's device array; left alone\n",
		    MTL_DEVICE_ARRAY, object_getClassName(array));
		return;
	}
	held = ask(array, sel_registerName("count"));
	objc_retain(device);
	tell(array, sel_registerName("removeAllObjects"));
	say("redwood: mediaserverd: %s taken out of the capture stack (%lu in the list, %lu now); "
	    "MTLCreateSystemDefaultDevice() = %s\n", object_getClassName(device), held,
	    ask(array, sel_registerName("count")), create() != NULL ? "A DEVICE STILL" : "nil");
}


/* ---- tracing a graph's stop: which node is told its input's data ended (2026-11-12) ----------------
 * BWGraph's stop waits 10 s for every sink node to be idle, idle meaning each of its inputs was told
 * "end of data" (didReachEndOfDataForInput:), which comes down the graph from the sources' outputs
 * (-[BWNodeOutput markEndOfLiveOutput]). A video recording's stop never got there ("Timed out waiting
 * for the capture graph to stop"). Every class that has its own didReachEndOfDataForInput: is traced,
 * and every markEndOfLiveOutput: the node that is missing from the log is the one that waits.
 */
#define EOD_MAX 96
static Class eod_class[EOD_MAX];
static IMP eod_original[EOD_MAX];
static int eod_count;
static IMP original_mark_end;
static __thread Class eod_at;           /* the class whose implementation runs now (for [super ...]) */
static __thread id eod_self;

static const char *
node_of(id port)
{
	id node = port != nil ? ((id (*)(id, SEL))objc_msgSend)(port, sel_registerName("node")) : nil;

	return node != nil ? object_getClassName(node) : "-";
}

static void
eod_trace(id self, SEL selector, id input)
{
	Class c = (eod_self == self && eod_at != Nil) ? class_getSuperclass(eod_at) : object_getClass(self);
	Class saved_at = eod_at;
	id saved_self = eod_self, connection, from;
	int i = -1;

	for (; c != Nil && i < 0; c = i < 0 ? class_getSuperclass(c) : c) {
		for (int k = 0; k < eod_count; k++) {
			if (eod_class[k] == c) {
				i = k;
				break;
			}
		}
		if (i >= 0) {
			break;
		}
	}
	if (i < 0) {
		return;
	}
	if (saved_self != self) {
		connection = input != nil ? ((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("connection")) : nil;
		from = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("output")) : nil;
		say("redwood: stop: end of data to %s %p (from %s)\n", object_getClassName(self), (void *)self, node_of(from));
	}
	eod_at = eod_class[i];
	eod_self = self;
	((void (*)(id, SEL, id))eod_original[i])(self, selector, input);
	eod_at = saved_at;
	eod_self = saved_self;
}

static id marked[64];
static unsigned marked_next;

static bool
was_marked(id output)
{
	for (unsigned i = 0; i < 64; i++) {
		if (marked[i] == output) {
			return true;
		}
	}
	return false;
}

static void
mark_end_trace(id self, SEL selector)
{
	say("redwood: stop: %s %p marks its output's end\n", node_of(self), (void *)self);
	marked[marked_next++ % 64] = self;
	((void (*)(id, SEL))original_mark_end)(self, selector);
}



/* The audio source's own counters, once a second while it runs: does the audio unit call it at all? */
static id audio_watched;
static dispatch_source_t audio_timer;

static long long
ivar_ll(id o, const char *name, int size)
{
	Ivar v = class_getInstanceVariable(object_getClass(o), name);

	if (v == NULL) {
		return -999;
	}
	const char *base = (const char *)o + ivar_getOffset(v);
	return size == 1 ? *(const uint8_t *)base : size == 4 ? *(const int32_t *)base : *(const int64_t *)base;
}

static void
audio_watch_tick(void *context)
{
	id o = context;

	say("redwood: audio source: renders %lld, silence frames %lld, started %lld, interrupted %lld, eod sent %lld, "
	    "silence filling off %lld, unit %#llx (subtype %#llx), device %lld, pull %lld\n",
	    ivar_ll(o, "_auRenderCount", 8), ivar_ll(o, "_silenceFramesGeneratedSinceLastAURenderProc", 8),
	    ivar_ll(o, "_streamStarted", 1), ivar_ll(o, "_streamInterrupted", 1), ivar_ll(o, "_endOfDataMarkerSent", 1),
	    ivar_ll(o, "_silenceFramesFillingDisabled", 1), ivar_ll(o, "_audioUnit", 8), ivar_ll(o, "_auSubType", 4),
	    ivar_ll(o, "_audioDeviceID", 4), ivar_ll(o, "_pullDuration", 4));
	{
		CMSimpleQueueRef q[3] = { (CMSimpleQueueRef)ivar_ll(o, "_renderProcErrorQueue", 8),
			(CMSimpleQueueRef)ivar_ll(o, "_activeBuffersQueue", 8), (CMSimpleQueueRef)ivar_ll(o, "_inactiveBuffersQueue", 8) };
		int32_t n[3];

		for (int i = 0; i < 3; i++) {
			n[i] = (uintptr_t)q[i] > 0x1000 ? CMSimpleQueueGetCount(q[i]) : -1;
		}
		const void *head = n[0] > 0 ? CMSimpleQueueGetHead(q[0]) : NULL;
		say("redwood: audio source: queues: errors %d (head %ld), active %d, inactive %d\n", n[0], (long)(intptr_t)head, n[1], n[2]);
		const uint8_t *b = n[1] > 0 ? CMSimpleQueueGetHead(q[1]) : NULL;
		if (b != NULL) {
			const CMTime *pts = (const CMTime *)(b + 0x18);
			say("redwood: audio source: oldest buffer: frames %u, gap %u, capacity %u, pts %lld/%d; next expected host %llu, "
			    "prev frames %lld\n", *(const uint32_t *)(b + 0x14), *(const uint32_t *)(b + 0x38), *(const uint32_t *)(b + 0x10),
			    pts->value, pts->timescale, (unsigned long long)ivar_ll(o, "_nextExpectedHostTime", 8), ivar_ll(o, "_prevNumFrames", 4));
		}
	}
}

static void
audio_watch_stop(void)
{
	if (audio_timer != NULL) {
		dispatch_source_cancel(audio_timer);
		audio_timer = NULL;
	}
	if (audio_watched != nil) {
		audio_watch_tick(audio_watched);
		objc_release(audio_watched);
		audio_watched = nil;
	}
}

static void
audio_watch_start(id o)
{
	audio_watch_stop();
	audio_watched = objc_retain(o);
	audio_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
	dispatch_set_context(audio_timer, o);
	dispatch_source_set_event_handler_f(audio_timer, audio_watch_tick);
	dispatch_source_set_timer(audio_timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
	dispatch_resume(audio_timer);
}

/*
 * The audio source's stop, finished: -[BWAudioSourceNode stop:] stops the microphone but never marks its
 * outputs' end (the trace above: every other source did, it never), so every node behind it -- Live Photo's
 * BWIrisStagingNode, a recording's movie file sink -- waited for its sound to end, BWGraph's stop waited
 * 10 s for them and mediaserverd died of "Timed out waiting for the capture graph to stop": no video was
 * ever finished (2026-11-12). Half a second after the stop, an output still live is marked here.
 */
static IMP original_audio_stop;

extern id objc_retain(id object);

static void
audio_stop_finish(void *context)
{
	id self = context;
	id outputs = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("outputs"));
	unsigned long n = outputs != nil ? ((unsigned long (*)(id, SEL))objc_msgSend)(outputs, sel_registerName("count")) : 0;

	for (unsigned long i = 0; i < n; i++) {
		id output = ((id (*)(id, SEL, unsigned long))objc_msgSend)(outputs, sel_registerName("objectAtIndex:"), i);

		if (!was_marked(output)) {
			say("redwood: stop: the audio source's output %lu was not ended: ended here\n", i);
			((void (*)(id, SEL))objc_msgSend)(output, sel_registerName("markEndOfLiveOutput"));
		}
	}
	objc_release(self);
}

static BOOL
audio_source_stop(id self, SEL selector, id *error)
{
	BOOL r = ((BOOL (*)(id, SEL, id *))original_audio_stop)(self, selector, error);

	say("redwood: stop: the audio source stopped (%d)\n", (int)r);
	audio_watch_stop();
	objc_retain(self);
	audio_stop_finish(self);        /* at once: half a second later the graph could be gone (SIGSEGV, 2026-11-12) */
	return r;
}

static void
finish_audio_stop(void)
{
	Class c = objc_getClass("BWAudioSourceNode");
	SEL sel = sel_registerName("stop:");
	Method m = c != Nil ? class_getInstanceMethod(c, sel) : NULL;
	IMP before;

	if (m == NULL) {
		say("redwood: mediaserverd: no -[BWAudioSourceNode stop:]\n");
		return;
	}
	before = method_getImplementation(m);
	IMP own = class_replaceMethod(c, sel, (IMP)audio_source_stop, method_getTypeEncoding(m));
	original_audio_stop = own != NULL ? own : before;
	say("redwood: mediaserverd: -[BWAudioSourceNode stop:] finished here (its outputs' end)\n");
}


/*
 * What reaches a recording's movie file sink, per kind of input (sound, picture, metadata): the first buffers,
 * then every 30th, with their times. The movie's writer interleaves them by time; one kind missing or with times
 * far from the others' leaves it waiting with the rest unwritten (2026-11-12: the file stopped at 2.6 MB).
 */
static IMP original_movie_render;

static void
movie_render(id self, SEL selector, CMSampleBufferRef sb, id input)
{
	static uint64_t count[4];
	CMFormatDescriptionRef fd = sb != NULL ? CMSampleBufferGetFormatDescription(sb) : NULL;
	FourCharCode type = fd != NULL ? CMFormatDescriptionGetMediaType(fd) : 0;
	int k = type == kCMMediaType_Audio ? 0 : type == kCMMediaType_Video ? 1 : type == kCMMediaType_Metadata ? 2 : 3;

	if (count[k] < 3 || count[k] % 30 == 0) {
		CMTime t = sb != NULL ? CMSampleBufferGetPresentationTimeStamp(sb) : kCMTimeInvalid;
		CMTime d = sb != NULL ? CMSampleBufferGetDuration(sb) : kCMTimeInvalid;

		say("redwood: movie: %c%c%c%c buffer %llu: pts %.3f s (dur %.4f), %ld samples, input %p\n", type >> 24, type >> 16 & 255,
		    type >> 8 & 255, type & 255, count[k], CMTIME_IS_VALID(t) ? CMTimeGetSeconds(t) : -1.0,
		    CMTIME_IS_VALID(d) ? CMTimeGetSeconds(d) : -1.0, sb != NULL ? (long)CMSampleBufferGetNumSamples(sb) : -1L, (void *)input);
	}
	count[k]++;
	((void (*)(id, SEL, CMSampleBufferRef, id))original_movie_render)(self, selector, sb, input);
}

static void
trace_movie_sink(void)
{
	Class c = objc_getClass("BWQuickTimeMovieFileSinkNode");
	SEL sel = sel_registerName("renderSampleBuffer:forInput:");
	Method m = c != Nil ? class_getInstanceMethod(c, sel) : NULL;

	if (m == NULL) {
		return;
	}
	IMP before = method_getImplementation(m);
	IMP own = class_replaceMethod(c, sel, (IMP)movie_render, method_getTypeEncoding(m));
	original_movie_render = own != NULL ? own : before;
	say("redwood: mediaserverd: the movie file sink's buffers are traced\n");
}


/* Live Photo's movie: what the staging node is asked (2026-11-12: the photographs said LIVE, no movie came) */
static IMP iris_enqueue_orig, iris_open_orig, iris_close_orig, iris_error_orig, iris_drop_orig, iris_render_orig;
static uint64_t iris_renders[4];
static double iris_first_pts[4] = { -1, -1, -1, -1 }, iris_last_pts[4];

static int
iris_enqueue(id self, SEL sel, id request)
{
	int r = ((int (*)(id, SEL, id))iris_enqueue_orig)(self, sel, request);
	say("redwood: live photo: request queued -> %d (%s)\n", r, request ? object_getClassName(request) : "nil");
	return r;
}

static void
iris_render(id self, SEL sel, CMSampleBufferRef sb, id input)
{
	CMFormatDescriptionRef fd = sb ? CMSampleBufferGetFormatDescription(sb) : NULL;
	FourCharCode t = fd ? CMFormatDescriptionGetMediaType(fd) : 0;
	int k = t == kCMMediaType_Video ? 0 : t == kCMMediaType_Audio ? 1 : t == kCMMediaType_Metadata ? 2 : 3;
	double pts = sb ? CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb)) : -1;

	if (iris_first_pts[k] < 0) {
		iris_first_pts[k] = pts;
	}
	iris_last_pts[k] = pts;
	iris_renders[k]++;
	((void (*)(id, SEL, CMSampleBufferRef, id))iris_render_orig)(self, sel, sb, input);
}

static int
iris_open(id self, SEL sel, id request)
{
	if (request != nil) {
		static const char *const names[] = { "stillImageCaptureTime", "movieStartTime", "movieEndTime", "movieTrimStartTime",
			"masterMovieStartTime", "beginTrimMasterPTS" };
		char line[400];
		size_t at = 0;

		for (int i = 0; i < 6; i++) {
			SEL g = sel_registerName(names[i]);
			CMTime t = class_respondsToSelector(object_getClass(request), g) ? ((CMTime (*)(id, SEL))objc_msgSend)(request, g) :
			    kCMTimeInvalid;

			at += (size_t)snprintf(line + at, sizeof(line) - at, " %s %s%.3f", names[i], CMTIME_IS_VALID(t) ? "" : "(invalid) ",
			    CMTIME_IS_VALID(t) ? CMTimeGetSeconds(t) : 0.0);
		}
		say("redwood: live photo: request%s\n", line);
	}
	{
		/*
		 * The staging node's _targetFrameDuration was never set here, and its pre-roll arithmetic with it gave an
		 * invalid time: -16958, "no pre-roll available", no Live Photo movie although ten seconds were staged
		 * (2026-11-12). The camera's frame time, as the iPhone's pipeline gives it.
		 */
		CMTime d = ((CMTime (*)(id, SEL))objc_msgSend)(self, sel_registerName("targetFrameDuration"));

		if (!CMTIME_IS_VALID(d)) {
			((void (*)(id, SEL, CMTime))objc_msgSend)(self, sel_registerName("setTargetFrameDuration:"), CMTimeMake(1, 30));
			say("redwood: live photo: the staging node's frame duration was unset: 1/30 s\n");
		}
		/*
		 * Its movie's begin and end (_beginIrisMovieCaptureTime, _endIrisMovieCaptureTime) were never set either --
		 * the end is what its pre-roll check goes by (-16958). 1.5 s each side of the photograph, as Live Photos are.
		 */
		CMTime begin = ((CMTime (*)(id, SEL))objc_msgSend)(self, sel_registerName("beginIrisMovieCaptureTime"));
		CMTime still = request != nil ? ((CMTime (*)(id, SEL))objc_msgSend)(request, sel_registerName("stillImageCaptureTime")) :
		    kCMTimeInvalid;

		if (!getenv("REDWOOD_IRIS_NOHACK") && !CMTIME_IS_VALID(begin) && CMTIME_IS_VALID(still)) {
			CMTime half = CMTimeMakeWithSeconds(1.5, 1000000000);

			((void (*)(id, SEL, CMTime))objc_msgSend)(self, sel_registerName("setBeginIrisMovieCaptureTime:"), CMTimeSubtract(still, half));
			((void (*)(id, SEL, CMTime))objc_msgSend)(self, sel_registerName("setEndIrisMovieCaptureTime:"), CMTimeAdd(still, half));
			say("redwood: live photo: the movie's begin and end were unset: %.3f to %.3f\n", CMTimeGetSeconds(still) - 1.5,
			    CMTimeGetSeconds(still) + 1.5);
		}
	}
	int r = ((int (*)(id, SEL, id))iris_open_orig)(self, sel, request);
	if (r == 0 && request != nil) {
		/* (and again once open: opening cleared them) */
		CMTime still = ((CMTime (*)(id, SEL))objc_msgSend)(request, sel_registerName("stillImageCaptureTime"));
		CMTime b0 = ((CMTime (*)(id, SEL))objc_msgSend)(self, sel_registerName("beginIrisMovieCaptureTime"));

		if (!getenv("REDWOOD_IRIS_NOHACK") && !CMTIME_IS_VALID(b0) && CMTIME_IS_VALID(still)) {
			CMTime half = CMTimeMakeWithSeconds(1.5, 1000000000);

			((void (*)(id, SEL, CMTime))objc_msgSend)(self, sel_registerName("setBeginIrisMovieCaptureTime:"), CMTimeSubtract(still, half));
			((void (*)(id, SEL, CMTime))objc_msgSend)(self, sel_registerName("setEndIrisMovieCaptureTime:"), CMTimeAdd(still, half));
		}
	}
	CMTime begin = ((CMTime (*)(id, SEL))objc_msgSend)(self, sel_registerName("beginIrisMovieCaptureTime"));
	CMTime buffering = ((CMTime (*)(id, SEL))objc_msgSend)(self, sel_registerName("bufferingTime"));

	say("redwood: live photo: valve opened -> %d; begin %.3f, buffering %.3f; staged video %llu (%.3f to %.3f), audio %llu "
	    "(%.3f to %.3f), metadata %llu\n", r, CMTIME_IS_VALID(begin) ? CMTimeGetSeconds(begin) : -1.0,
	    CMTIME_IS_VALID(buffering) ? CMTimeGetSeconds(buffering) : -1.0, iris_renders[0], iris_first_pts[0], iris_last_pts[0],
	    iris_renders[1], iris_first_pts[1], iris_last_pts[1], iris_renders[2]);
	return r;
}

static void
iris_close(id self, SEL sel)
{
	say("redwood: live photo: valve closed\n");
	((void (*)(id, SEL))iris_close_orig)(self, sel);
}

static void
iris_error(id self, SEL sel, id error, id input)
{
	id d = error ? ((id (*)(id, SEL))objc_msgSend)(error, sel_registerName("description")) : nil;
	say("redwood: live photo: node error %s\n", d ? ((const char *(*)(id, SEL))objc_msgSend)(d, sel_registerName("UTF8String")) : "?");
	((void (*)(id, SEL, id, id))iris_error_orig)(self, sel, error, input);
}

static void
iris_drop(id self, SEL sel, id sample, id input)
{
	static int n;
	if (n++ % 30 == 0) {
		say("redwood: live photo: dropped sample (%d)\n", n);
	}
	((void (*)(id, SEL, id, id))iris_drop_orig)(self, sel, sample, input);
}

static void
hook_one(const char *cls, const char *sel, IMP repl, IMP *orig)
{
	Class c = objc_getClass(cls);
	Method m = c ? class_getInstanceMethod(c, sel_registerName(sel)) : NULL;

	if (m != NULL) {
		IMP before = method_getImplementation(m);
		IMP own = class_replaceMethod(c, sel_registerName(sel), repl, method_getTypeEncoding(m));
		*orig = own ? own : before;
	}
}

static void
trace_live_photo(void)
{
	hook_one("BWIrisStagingNode", "enqueueIrisRequest:", (IMP)iris_enqueue, &iris_enqueue_orig);
	hook_one("BWIrisStagingNode", "renderSampleBuffer:forInput:", (IMP)iris_render, &iris_render_orig);
	hook_one("BWIrisStagingNode", "openValveWithIrisRequest:", (IMP)iris_open, &iris_open_orig);
	hook_one("BWIrisStagingNode", "closeValve", (IMP)iris_close, &iris_close_orig);
	hook_one("BWIrisStagingNode", "handleNodeError:forInput:", (IMP)iris_error, &iris_error_orig);
	hook_one("BWIrisStagingNode", "handleDroppedSample:forInput:", (IMP)iris_drop, &iris_drop_orig);
}

static void
trace_graph_stop(void)
{
	static bool done;
	unsigned int n = 0;
	Class *all;
	SEL eod = sel_registerName("didReachEndOfDataForInput:");
	Class out = objc_getClass("BWNodeOutput"), node = objc_getClass("BWNode");

	if (done || out == Nil || node == Nil) {
		return;
	}
	done = true;
	finish_audio_stop();
	trace_movie_sink();
	trace_live_photo();
	Method m = class_getInstanceMethod(out, sel_registerName("markEndOfLiveOutput"));
	if (m != NULL) {
		original_mark_end = method_setImplementation(m, (IMP)mark_end_trace);
	}
	all = objc_copyClassList(&n);
	for (unsigned int i = 0; all != NULL && i < n && eod_count < EOD_MAX; i++) {
		unsigned int mc = 0;
		Method *ms;
		bool is_node = false;

		for (Class c = all[i]; c != Nil; c = class_getSuperclass(c)) {
			if (c == node) {
				is_node = true;
				break;
			}
		}
		if (!is_node || (ms = class_copyMethodList(all[i], &mc)) == NULL) {
			continue;
		}
		for (unsigned int k = 0; k < mc; k++) {
			if (method_getName(ms[k]) == eod) {
				eod_class[eod_count] = all[i];
				eod_original[eod_count] = method_setImplementation(ms[k], (IMP)eod_trace);
				eod_count++;
			}
		}
		free(ms);
	}
	free(all);
	say("redwood: mediaserverd: a graph's stop is traced (%d node classes, markEndOfLiveOutput %s)\n", eod_count,
	    original_mark_end ? "too" : "not found");
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	/* before anything of the image that was just added runs, and this is the only place in this
	 * library that is told about a new image at all */
	keep_metal_from_the_capture_stack();
	if (objc_getClass("BWGraph") != Nil && objc_getClass("BWQuickTimeMovieFileSinkNode") != Nil) {
		trace_graph_stop();
	}
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
		    class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (original == NULL || (r->expected != 1 && (uintptr_t)original != r->expected + slide)) {
			say("redwood: %c[%s %s]: found %#lx, expected %#lx; left alone\n", r->instance ? '-' : '+', r->class_name,
			    r->selector,
			    original ? (uintptr_t)original - slide : 0, r->expected);
			continue;
		}
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("redwood: mediaserverd: %c[%s %s] replaced\n", r->instance ? '-' : '+', r->class_name, r->selector);
	}
}

__attribute__((constructor))
static void
redwood_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	/*
	 * A development copy: /var/redwood/libredwood-media-dev.dylib, when it is there, is loaded instead of this
	 * one (which then does nothing), so the camera's code changes with `killall mediaserverd' and no reboot.
	 * (Made with -install_name /var/redwood/libredwood-media-dev.dylib and ldid -S; deleted, this one is back.)
	 */
	{
		static const char dev[] = "/var/redwood/libredwood-media-dev.dylib";
		Dl_info me;

		/* (the development copy itself is built with -DREDWOOD_DEV: dyld names it /private/var/..., 2026-11-12) */
#if !defined(REDWOOD_DEV) && !defined(REDWOOD_MEDIA_BUILTIN)
		if (dladdr((const void *)redwood_media_init, &me) != 0 && me.dli_fname != NULL && strstr(me.dli_fname, dev) == NULL &&
		    access(dev, R_OK) == 0) {
#else
		(void)me;
		if (false) {
#endif
			void *h = dlopen(dev, RTLD_NOW | RTLD_GLOBAL);

			say("redwood: mediaserverd: the development copy %s: %s\n", dev, h != NULL ? "loaded instead" : dlerror());
			if (h != NULL) {
				return;
			}
		}
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	/* The S20 has no AVE (the iPhone's video encoder): our software H.264 encoder (src/h264_encoder.c) is
	 * registered with VideoToolbox here, where compression sessions run (iOS runs them in this process).
	 * h264=0 leaves it out. */
	if (!boot_arg("h264=0")) {
		extern bool redwood_h264_register(void);
		(void)redwood_h264_register();
	}
	metal_left_alone = boot_arg("mediamtl=1");
	if (metal_left_alone) {
		say("redwood: mediaserverd: mediamtl=1: the capture stack may have the Metal device (it "
		    "needs AppleCVAPhoto's cvadepth-iOS.metallib in the image)\n");
	} else {
		say("redwood: mediaserverd: the Metal device leaves this process as soon as Metal is in "
		    "it (mediamtl=1 leaves it there)\n");
	}
	torch = open("/dev/redwood-torch", O_WRONLY);
	camera = access("/dev/redwood-camera", F_OK) == 0;
	if (torch >= 0) {
		flashlight_classes();
		say("redwood: mediaserverd: the kernel has a flashlight (/dev/redwood-torch)\n");
	}
	if (camera) {
		say("redwood: mediaserverd: the kernel has a camera (/dev/redwood-camera)\n");
		register_hevc_encoder();
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}

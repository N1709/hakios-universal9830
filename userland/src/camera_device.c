/*
 * The phone's cameras as iOS's capture stack wants them: a FigCaptureDevice with two FigCaptureStreams,
 * the wide camera as the back camera and the front one as the front camera, made for mediaserverd's
 * BWFigCaptureDeviceVendor in place of the device Apple's H10ISP.mediacapture makes of the iPhone's ISP
 * (part of libredwood-media.dylib; inject_media.c gives the vendor our device's create function when
 * the kernel has a camera: cam=5).
 *
 * Both are CoreMedia "derived objects" (CMDerivedObjectCreate): a table of functions for the base
 * class (properties) and one for the device or stream class, as H10ISP's are laid out in iOS 16.5:
 *   device  RequestStreams, RelinquishStreams (and a processing session and Pearl calibration, not here)
 *   stream  Start, Stop, AddAttachmentsToSampleBuffer
 * A stream's frames go to the capture graph through the blocks it is given in its property
 * "VideoOutputHandlers" ({ output: { PixelBufferHandler: ^(CVPixelBufferRef, CMTime) } }), for the
 * outputs "VideoOutputConfigurations" describes (the graph's own buffer pools) and "VideoOutputsEnabled"
 * switches on. A frame: read from the kernel's /dev/redwood-camera (RAW10), made into a picture by
 * camera_isp.c (exposure, white balance kept to the sensor's reference lights, the colour matrix and
 * lens shading of the camera's own tuning, gamma, zoom), one pixel buffer for each output, with the
 * metadata the graph keeps its frame statistics and its zoom by. The kernel gives one sensor's frames
 * at a time: a stream that starts stops the other's frames and has the kernel change sensors.
 *
 * The formats the streams offer are named as the iPhone X's cameras' are in CMCapture's
 * D22/AVCaptureSession.plist (FigCaptureStreamFormats, by the sensor sizes and frame rates the plist
 * matches them by), so that their AVCaptureDevice formats and session presets resolve: the pictures
 * are scaled from the sensor's frames to whatever size is asked for.
 *
 * The shutter: StillImageCaptureNow asks for one frame of the whole sensor on the StillImage output
 * (the time machine's TimeMachineSuspendNow/ResumeNow are taken and ignored). Focus, where there is
 * a lens motor (the wide camera's): by contrast, continuous, and on the window a tap gives (af_frame()).
 *
 * Everything asked of the device and the streams that is not answered here is said in mediaserverd's
 * log ("redwood: camera: ..."), once per property.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <Block.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <notify.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "camera_isp.h"
#include "camera_s20_zoom.h"

static const char *tuning_dir(void);
static const char *rw_path(const char *leaf, char *buf, size_t size);

void redwood_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));

void
redwood_camera_say(const char *format, ...)
{
	char line[400];
	va_list ap;
	int n = snprintf(line, sizeof(line), "redwood: camera: ");

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
}
#define say redwood_camera_say

/* ---- CoreMedia's derived objects, as iOS 16.5 lays them out ---------------------------------------- */

typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef const void *CMBaseClassID;

struct base_class {
	uint64_t        version;
	uint64_t        storage_size;
	Boolean         (*equal)(CMBaseObjectRef, CMBaseObjectRef);
	OSStatus        (*invalidate)(CMBaseObjectRef);
	void            (*finalize)(CMBaseObjectRef);
	CFStringRef     (*copy_debug_description)(CMBaseObjectRef);
	OSStatus        (*copy_property)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
	OSStatus        (*set_property)(CMBaseObjectRef, CFStringRef, CFTypeRef);
	OSStatus        (*notification_barrier)(CMBaseObjectRef);
	const void *    protocols;
	const void *    reserved;
};
struct device_class {
	uint64_t        version;                /* 3 */
	void *          processing_session_create;
	OSStatus        (*request_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
	OSStatus        (*relinquish_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
	void *          pearl_calibration;
};
struct stream_class {
	uint64_t        version;                /* 2 */
	OSStatus        (*start)(CMBaseObjectRef);
	OSStatus        (*stop)(CMBaseObjectRef);
	OSStatus        (*add_attachments)(CMBaseObjectRef, CMSampleBufferRef, CFDictionaryRef);
};
struct vtable {
	const void *    reserved;
	const struct base_class *base;
	const void *    derived;
};

#define ERR_PARAM               (-12780)
#define ERR_NOT_AVAILABLE       (-12783)
#define ERR_UNSUPPORTED         (-12782)

static OSStatus (*derived_create)(CFAllocatorRef, const struct vtable *, CMBaseClassID, CMBaseObjectRef *);
static void *(*derived_storage)(CMBaseObjectRef);
static CMBaseClassID (*device_class_id)(void);
static CMBaseClassID (*stream_class_id)(void);
static CFStringRef port_back;           /* kFigCapturePortType_BackFacingCamera */
static CFStringRef port_front;          /* kFigCapturePortType_FrontFacingCamera */

/*
 * The frames' metadata: what H10ISP's stream attaches to every frame (a dictionary under
 * kFigCaptureSampleBufferAttachmentKey_MetadataDictionary, through the stream's
 * AddAttachmentsToSampleBuffer when the graph defers it), and what the capture device keeps its frame
 * statistics of. -[BWFrameStatisticsByPortType updateWithFrameMetadata:updateFocusDistance:] counts
 * a frame only for a buffer that has it, and -[BWFigVideoCaptureDevice
 * _stillImageCaptureInProgressSubstateForFrameStatisticsByPortType:...] puts a standard photograph
 * off until more than four frames have been counted (then only while AFStatus is 2, a coarse focus
 * scan): without it the shutter's capture was put off on every frame (the retry in
 * _updateStateUsingVideoSampleBuffer:fromCaptureStream:), for ever, and StillImageCaptureNow never
 * came. The dictionary goes on the pixel buffer too, and mutable, as the ISP's does: FigImageControl_Focus
 * (the device's camera controls, on every frame once the sample buffer has metadata) reads it from
 * the pixel buffer with no check that it is there -- CFDictionaryGetValue(NULL, ...): mediaserverd
 * died of SIGSEGV -- and adds its own keys to it. The keys are CoreMedia's; the values come from the
 * frame's header and our ISP's levels, in Apple's units where they are known (the exposure in seconds,
 * gains with 256 for 1x); lux and SNR are rough.
 */
enum {
	MD_PORT, MD_PTS, MD_EXPOSURE, MD_AGC, MD_ISP_DGAIN, MD_SENSOR_DGAIN, MD_ISO_BASE, MD_FRAME_RATE, MD_LUX, MD_SNR,
	MD_AF_STATUS, MD_FOCUSING, MD_AE_STABLE, MD_AE_LOCKED, MD_AWB_STABLE, MD_AWB_LOCKED, MD_AF_LOCKED, MD_ZOOM, MD_KEYS
};
static const char *const md_names[MD_KEYS] = {
	"kFigCaptureStreamMetadata_PortType", "kFigCaptureSampleBufferAttachmentKey_OriginalPresentationTimeStamp",
	"kFigCaptureStreamMetadata_ExposureTime", "kFigCaptureStreamMetadata_AGC", "kFigCaptureStreamMetadata_ispDGain",
	"kFigCaptureStreamMetadata_sensorDGain", "kFigCaptureStreamMetadata_ISOBase", "kFigCaptureStreamMetadata_CurrentFrameRate",
	"kFigCaptureStreamMetadata_LuxLevel", "kFigCaptureStreamMetadata_NormalizedSNR", "kFigCaptureStreamMetadata_AFStatus",
	"kFigCaptureStreamMetadata_FocusingMethod", "kFigCaptureStreamMetadata_AEStable", "kFigCaptureStreamMetadata_AELocked",
	"kFigCaptureStreamMetadata_AWBStable", "kFigCaptureStreamMetadata_AWBLocked", "kFigCaptureStreamMetadata_AFLocked",
	"kFigCaptureStreamMetadata_ZoomFactor",
};
static CFStringRef md_key[MD_KEYS];
static CFStringRef md_attachment;       /* kFigCaptureSampleBufferAttachmentKey_MetadataDictionary */

static bool
find_functions(void)
{
	CFStringRef *port;

	derived_create = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
	derived_storage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
	device_class_id = dlsym(RTLD_DEFAULT, "FigCaptureDeviceGetClassID");
	stream_class_id = dlsym(RTLD_DEFAULT, "FigCaptureStreamGetClassID");
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_BackFacingCamera");
	port_back = port != NULL ? *port : NULL;
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_FrontFacingCamera");
	port_front = port != NULL ? *port : NULL;
	port = dlsym(RTLD_DEFAULT, "kFigCaptureSampleBufferAttachmentKey_MetadataDictionary");
	md_attachment = port != NULL ? *port : NULL;
	for (int i = 0; i < MD_KEYS; i++) {
		port = dlsym(RTLD_DEFAULT, md_names[i]);
		md_key[i] = port != NULL ? *port : NULL;
		if (md_key[i] == NULL) {
			say("%s is not there: the frames' metadata go without it", md_names[i]);
		}
	}
	if (md_attachment == NULL || md_key[MD_PORT] == NULL) {
		say("kFigCaptureSampleBufferAttachmentKey_MetadataDictionary or the port's key is not there: frames without "
		    "metadata, and the shutter will wait for ever");
		md_attachment = NULL;
	}
	if (derived_create == NULL || derived_storage == NULL || device_class_id == NULL || stream_class_id == NULL || port_back == NULL ||
	    port_front == NULL) {
		say("CoreMedia's or CMCapture's functions are not there: no camera");
		return false;
	}
	return true;
}

/* ---- what is asked and not answered: said once per name ------------------------------------------- */

static CFMutableSetRef said_names;
static pthread_mutex_t said_lock = PTHREAD_MUTEX_INITIALIZER;

static void
say_value(const char *what, CFStringRef key, CFTypeRef value)
{
	char name[160] = "?", text[300] = "";

	CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
	if (value != NULL) {
		CFStringRef d = CFCopyDescription(value);

		CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
		CFRelease(d);
		for (char *c = text; *c; c++) {
			*c = *c == '\n' ? ' ' : *c;
		}
	}
	say("%s %s%s%s", what, name, value ? " = " : "", text);
}

static void
say_once(const char *what, CFStringRef key, CFTypeRef value)
{
	CFStringRef tag = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s %@"), what, key);

	pthread_mutex_lock(&said_lock);
	if (said_names == NULL) {
		said_names = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	bool first = !CFSetContainsValue(said_names, tag);

	CFSetAddValue(said_names, tag);
	pthread_mutex_unlock(&said_lock);
	CFRelease(tag);
	if (first) {
		say_value(what, key, value);
	}
}

/* ---- numbers and dictionaries ---------------------------------------------------------------------- */

static CFNumberRef
number_i(int64_t v)
{
	return CFNumberCreate(NULL, kCFNumberSInt64Type, &v);
}

static CFNumberRef
number_f(double v)
{
	return CFNumberCreate(NULL, kCFNumberDoubleType, &v);
}

static void
put(CFMutableDictionaryRef d, const char *key, CFTypeRef value)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);

	CFDictionarySetValue(d, k, value);
	CFRelease(k);
	CFRelease(value);
}

static CFDictionaryRef
new_dict(void)
{
	return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static bool
key_is(CFStringRef key, const char *name)
{
	char buffer[128];

	return CFStringGetCString(key, buffer, sizeof(buffer), kCFStringEncodingUTF8) && strcmp(buffer, name) == 0;
}

/* ---- what the device and the stream say they have ---------------------------------------------------- */

/*
 * The capture stack asks only for the properties an object lists in its "SupportedPropertiesDictionary"
 * ({ name: { PropertyType, ReadWriteStatus } }). Listed here: what is answered, and what is taken
 * (and, for now, mostly not acted on) without failing the graph that sets it.
 */
struct prop {
	const char *name, *type, *access;
};

static const struct prop stream_props[] = {
	{ "PortType", "String", "ReadOnly" }, { "UniqueID", "String", "ReadOnly" }, { "NonLocalizedName", "String", "ReadOnly" },
	{ "SupportedFormatsArray", "Collection", "ReadOnly" }, { "FocalLength", "Number", "ReadOnly" },
	{ "LensFNumber", "Number", "ReadOnly" }, { "PixelSize", "Number", "ReadOnly" },
	{ "MinZoomFactor", "Number", "ReadOnly" }, { "MaxZoomFactor", "Number", "ReadOnly" }, { "SupportedColorSpaces", "Collection", "ReadOnly" },
	{ "HasAutoFocusPositionSensor", "Boolean", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" },
	{ "FormatIndex", "Number", "ReadWrite" }, { "MinimumFrameRate", "Number", "ReadWrite" }, { "MaximumFrameRate", "Number", "ReadWrite" },
	{ "MaximumAllowedFrameRate", "Number", "ReadWrite" }, { "VideoOutputConfigurations", "Collection", "ReadWrite" },
	{ "VideoOutputsEnabled", "Collection", "ReadWrite" }, { "VideoOutputHandlers", "Collection", "WriteOnly" },
	{ "DeferAdditionOfAttachments", "Boolean", "ReadWrite" }, { "TorchLevel", "Number", "ReadWrite" },
	{ "HighCurrentTorchEnabled", "Boolean", "ReadWrite" }, { "ColorSpace", "Number", "ReadWrite" }, { "ZoomFactor", "Number", "ReadWrite" },
	{ "ExposureBias", "Number", "ReadWrite" }, { "DeviceOrientation", "Number", "ReadWrite" }, { "FlashMode", "Number", "ReadWrite" },
	{ "StillImageCaptureNow", "Collection", "WriteOnly" }, { "LockAENow", "Boolean", "WriteOnly" }, { "UnlockAENow", "Boolean", "WriteOnly" },
	{ "LockAWBNow", "Boolean", "WriteOnly" }, { "UnlockAWBNow", "Boolean", "WriteOnly" }, { "MaxIntegrationTime", "Number", "ReadWrite" },
	{ "AEMaxGain", "Number", "ReadWrite" }, { "AEMinGain", "Number", "ReadWrite" }, { "FaceDetectionEnabled", "Boolean", "ReadWrite" },
	{ "VideoRecordingInProgress", "Boolean", "ReadWrite" }, { "MemoryPoolID", "Number", "ReadWrite" },
	{ "ChromaNoiseReductionEnabled", "Boolean", "ReadWrite" }, { "TemporalNoiseReductionEnabled", "Boolean", "ReadWrite" },
	{ "MultipleOutputSupportEnabled", "Boolean", "ReadWrite" }, { "VideoOutputsColorInfo", "Collection", "ReadWrite" },
	{ "RegionOfInterestForCameraControls", "Collection", "ReadWrite" }, { "AEMeteringMode", "Number", "ReadWrite" },
	{ "MaxFrameCoalescingDuration", "Number", "ReadWrite" }, { "LimitAERange", "Boolean", "ReadWrite" },
	{ "TimeMachineSuspendNow", "Boolean", "WriteOnly" }, { "TimeMachineResumeNow", "Boolean", "WriteOnly" },
	/* focus (the wide camera's lens motor; see af_frame()) */
	{ "AutoFocusNow", "Collection", "WriteOnly" }, { "ContinuousAutoFocusNow", "Collection", "WriteOnly" },
	{ "FocusNow", "Collection", "WriteOnly" }, { "PeakTrackingFocusNow", "Collection", "WriteOnly" },
	{ "LockAFNow", "Boolean", "WriteOnly" }, { "ManualFocusNow", "Collection", "WriteOnly" },
	{ "AFWindowParams", "Collection", "ReadWrite" }, { "AutoFocusMode", "Number", "ReadWrite" },
}, device_props[] = {
	{ "StreamArray", "Collection", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" }, { "ClientIsActive", "Boolean", "ReadWrite" },
	{ "SupportedSynchronizedStreamsGroups", "Collection", "ReadOnly" }, { "MemoryPoolID", "Number", "ReadWrite" },
};

static CFDictionaryRef
supported(const struct prop *p, size_t n)
{
	CFMutableDictionaryRef all = (CFMutableDictionaryRef)new_dict();

	for (size_t i = 0; i < n; i++) {
		CFMutableDictionaryRef one = (CFMutableDictionaryRef)new_dict();

		put(one, "PropertyType", CFStringCreateWithCString(NULL, p[i].type, kCFStringEncodingUTF8));
		put(one, "ReadWriteStatus", CFStringCreateWithCString(NULL, p[i].access, kCFStringEncodingUTF8));
		put(all, p[i].name, one);
	}
	return all;
}

/* ---- the formats ----------------------------------------------------------------------------------- */

#define PF_420F         0x34323066      /* '420f' */
#define PF_420V         0x34323076      /* '420v' */
#define PF_BGRA         0x42475241      /* 'BGRA' */

static CFDictionaryRef
output_info(int width, int height)
{
	CFMutableDictionaryRef o = (CFMutableDictionaryRef)new_dict();
	CFNumberRef pf[3] = { number_i(PF_420F), number_i(PF_420V), number_i(PF_BGRA) };
	CFArrayRef pfs = CFArrayCreate(NULL, (const void **)pf, 3, &kCFTypeArrayCallBacks);

	for (int i = 0; i < 3; i++) {
		CFRelease(pf[i]);
	}
	put(o, "SupportedPixelFormats", pfs);
	put(o, "NativeWidth", number_i(width));
	put(o, "NativeHeight", number_i(height));
	put(o, "MaxDownscaleFactor", number_f(32));
	put(o, "MaxScaleFactor", number_f(1));
	put(o, "MinScaleFactor", number_f(1.0 / 32));
	put(o, "OutputWidthAlignmentRequirement", number_i(16));
	return o;
}

static CFDictionaryRef
format(int sensor_width, int sensor_height, int max_fps, bool binned, double fov)
{
	CFMutableDictionaryRef f = (CFMutableDictionaryRef)new_dict(), outputs = (CFMutableDictionaryRef)new_dict();
	CMVideoFormatDescriptionRef fd = NULL;

	CMVideoFormatDescriptionCreate(NULL, PF_420F, sensor_width, sensor_height, NULL, &fd);
	if (fd != NULL) {
		put(f, "FormatDescription", fd);
	}
	put(f, "VideoMinFrameRate", number_f(2));
	put(f, "VideoMaxFrameRate", number_f(max_fps));
	put(f, "VideoIsBinned", CFRetain(binned ? kCFBooleanTrue : kCFBooleanFalse));
	put(f, "VideoBinningFactorHorizontal", number_i(binned ? 2 : 1));
	put(f, "VideoBinningFactorVertical", number_i(binned ? 2 : 1));
	put(f, "VideoFieldOfView", number_f(fov));
	put(f, "VideoMinIntegrationTime", number_i(100));
	put(f, "VideoMaxIntegrationTime", number_i(1000000 / 15));
	put(f, "VideoRawBitDepth", number_i(10));
	put(f, "VideoMaxWidth", number_i(sensor_width));
	put(f, "VideoMaxHeight", number_i(sensor_height));
	put(f, "Experimental", CFRetain(kCFBooleanFalse));
	put(f, "SensorWidth", number_i(sensor_width));
	put(f, "SensorHeight", number_i(sensor_height));
	put(f, "ISOBase", number_f(32));
	/* Live Photo: the movie around the photograph (BWIrisStagingNode) is only made for a format that says so */
	put(f, "IrisSupported", CFRetain(kCFBooleanTrue));
	put(f, "DefaultAEMaxGain", number_f(64));

	/*
	 * The sensor crop geometry the capture-graph builder reads off this format to resolve non-zero
	 * output dimensions (confirmed by a hook: -[FigCaptureSourceVideoFormat sensorCropDimensions] /
	 * postGDCCropDimensions] return these). Everything spans the full sensor: no GDC crop.
	 */
	put(f, "SensorCropWidth", number_i(sensor_width));
	put(f, "SensorCropHeight", number_i(sensor_height));
	put(f, "PreGDCSensorCropWidth", number_i(sensor_width));
	put(f, "PreGDCSensorCropHeight", number_i(sensor_height));
	put(f, "PostGDCCropWidth", number_i(sensor_width));
	put(f, "PostGDCCropHeight", number_i(sensor_height));
	put(f, "GeometricDistortionCorrectedNativeWidth", number_i(sensor_width));
	put(f, "GeometricDistortionCorrectedNativeHeight", number_i(sensor_height));

	put(outputs, "PrimaryScaler", output_info(sensor_width, sensor_height));
	put(outputs, "SecondaryScaler", output_info(sensor_width, sensor_height));
	put(outputs, "StillImage", output_info(sensor_width, sensor_height));
	put(f, "SupportedOutputs", outputs);
	return f;
}

/* ---- the stream ------------------------------------------------------------------------------------- */

#define HELD    3               /* the moving pictures kept from the pool after they are given (see deliver) */

struct output {
	const char *    name;
	CFDictionaryRef config;                 /* from VideoOutputConfigurations */
	bool            enabled;
	void *          handler;                /* a block: ^(CVPixelBufferRef, CMTime) */
	bool            said_shape;
	uint64_t        delivered, refused;
	CVPixelBufferRef held[HELD];            /* the pictures given last, kept from the pool (see deliver) */
	unsigned        held_next;
	bool            said_pool;
};

static void let_go(struct output *out);

#define OUTPUTS 3
static const char *const output_names[OUTPUTS] = { "PrimaryScaler", "SecondaryScaler", "StillImage" };

#define MAX_ZOOM        16              /* (the most the iPhone X's wide camera zooms digitally) */

/*
 * The device's cameras, a stream each, as the iPhone X's are in CMCapture's D22/AVCaptureSession.plist
 * (its devices' uniqueIDs, and the stream formats its formats and presets are matched to by sensor size
 * and frame rate): the wide camera as the back camera, the front one as the front camera. The kernel
 * gives one sensor's frames at a time; a stream's frames start with its own sensor's (see worker()).
 */
static const struct camera {
	uint32_t        sensor;         /* the kernel's (the frames' sensor, REDWOOD_CAMERA_SENSOR) */
	const char      *name, *unique_id;
	bool            turned;         /* its sensor the other way up from the iPhone's: the pictures turned half round */
	double          focal_length, f_number, pixel_size, fov;
	int             formats[2][3];  /* sensor width, height, frames a second of "Photo" and of a video format */
} cameras[2] = {
	{ 1, "Back Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:0", false, 4.0, 1.8, 1.22, 65.0,
	  { { 4032, 3024, 30 }, { 1920, 1080, 30 } } },        /* "Photo", "Video-30": 1080p, as the front (4224x2376 at 60 was
	                                                         * never picked for video: no recording began, 2026-11-12) */
	/* (turned: true on the Galaxy S20 too: its front SAK3J1's frames came upside down and mirrored with false, 2026-11-11;
	 * kern.redwood.camera_turn bit 0 flips it once more, live) */
	{ 2, "Front Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:1", true, 2.87, 2.2, 1.0, 70.0,
	  { { 3088, 2320, 30 }, { 1920, 1080, 30 } } },        /* "Photo", "Video-30" (1080p, as the iPhone X's front camera
	                                                         * records; 4K was a CPU ISP and encoder load the S20 cannot carry) */
};

struct stream {
	CFArrayRef      formats;
	CFDictionaryRef supported;
	CFDictionaryRef colour_info;    /* from VideoOutputsColorInfo */
	CFDictionaryRef metadata;       /* the last frame's (frame_metadata()), for a buffer that has none of its own */
	pthread_mutex_t metadata_lock;  /* (not `lock': AddAttachmentsToSampleBuffer may come while a frame is given out) */
	unsigned        stills_wanted;  /* photographs asked for by StillImageCaptureNow, not yet given */
	bool            flash_on;       /* FlashMode ForceFlashOn: the front camera's photographs with the screen lit */
	uint64_t        flash_ready;    /* the frame from which a lit photograph may be taken (0: no flash going) */
	int             format_index;
	double          min_fps, max_fps;
	float           zoom;           /* ZoomFactor: the picture is the middle 1/zoom of the sensor each way */
	const struct camera *camera;
	uint64_t        render_ticks;   /* the moving pictures' ISP time since the last per-second line */
	uint64_t        read_ticks, stats_ticks; /* reading a frame from the kernel; measuring, AE/AWB/AF */
	CFStringRef     port;
	bool            af_now;         /* a focus asked for (AutoFocusNow and the like): a search at the next frame */
	bool            af_locked;      /* LockAFNow: no search but one asked for */
	float           af_window[4];   /* AFWindowParams: where to focus, as fractions of the frame (x0, y0, x1, y1) */
	float           ev_bias;        /* ExposureBias, in stops (Camera.app's sun slider) */
	bool            ae_locked;      /* LockAENow until UnlockAENow */
	struct output   out[OUTPUTS];
	pthread_mutex_t lock;
	pthread_t       worker;
	volatile bool   running;
	volatile bool   graph_running;  /* the capture graph has started this stream: frames may be given out */
	bool            worker_started;
	volatile bool   worker_busy;    /* the worker thread has not returned yet (it may outlive a stop: stream_stop) */
	int             torch;
	int             last_turn;
	uint64_t        frames, stills_given;
};

static void flash_light(struct stream *s, bool on);

static struct stream *
stream_of(CMBaseObjectRef o)
{
	return derived_storage(o);
}

static int
output_index(CFStringRef name)
{
	for (int i = 0; i < OUTPUTS; i++) {
		if (key_is(name, output_names[i])) {
			return i;
		}
	}
	return -1;
}

static OSStatus
stream_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	struct stream *s = stream_of(o);
	CFTypeRef value = NULL;

	(void)allocator;
	if (key_is(key, "PortType")) {
		value = CFRetain(s->port);
	} else if (key_is(key, "UniqueID") || key_is(key, "ModelID")) {
		value = CFStringCreateWithCString(NULL, s->camera->unique_id, kCFStringEncodingUTF8);
	} else if (key_is(key, "NonLocalizedName") || key_is(key, "LocalizedName")) {
		value = CFStringCreateWithCString(NULL, s->camera->name, kCFStringEncodingUTF8);
	} else if (key_is(key, "Manufacturer")) {
		value = CFRetain(CFSTR("Apple Inc."));
	} else if (key_is(key, "SupportedFormatsArray")) {
		value = CFRetain(s->formats);
	} else if (key_is(key, "FormatIndex")) {
		value = number_i(s->format_index);
	} else if (key_is(key, "MinimumFrameRate")) {
		value = number_f(s->min_fps);
	} else if (key_is(key, "MaximumFrameRate")) {
		value = number_f(s->max_fps);
	} else if (key_is(key, "Clock")) {
		value = CFRetain(CMClockGetHostTimeClock());
	} else if (key_is(key, "TorchLevel") || key_is(key, "ExposureBias")) {
		value = number_f(0);
	} else if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled")) {
		/*
		 * ReadWrite, but the capture-graph builder READS these back while it builds the graph
		 * (BWMultiStreamCameraSourceNode's _updateOutputsStorage does a getProperty: for
		 * VideoOutputConfigurations, not IfSupported:), and a failed read tears the graph down. Return
		 * what each output holds now.
		 */
		bool configs = key_is(key, "VideoOutputConfigurations");
		CFMutableDictionaryRef d = (CFMutableDictionaryRef)new_dict();

		pthread_mutex_lock(&s->lock);
		for (int i = 0; i < OUTPUTS; i++) {
			CFStringRef name = CFStringCreateWithCString(NULL, output_names[i], kCFStringEncodingUTF8);

			if (configs) {
				if (s->out[i].config != NULL) {
					CFDictionarySetValue(d, name, s->out[i].config);
				}
			} else {
				CFDictionarySetValue(d, name, s->out[i].enabled ? kCFBooleanTrue : kCFBooleanFalse);
			}
			CFRelease(name);
		}
		pthread_mutex_unlock(&s->lock);
		value = d;
	} else if (key_is(key, "VideoOutputsColorInfo")) {
		/*
		 * ReadWrite, and the capture graph READS it before it starts the session:
		 * -[BWMultiStreamCameraSourceNode _verifyColorSpacePropertiesForAllOutputs] does a
		 * getProperty:error: for it, and an error there fails the session's transition to running
		 * and tears the whole graph down again. What it holds matters only for an output whose
		 * colour space is not sRGB (_verifyColorSpacePropertiesForOutputID: returns at once when
		 * the output's colour space is 0), and sRGB is the only one this stream offers, so what
		 * the graph set is given back as it is.
		 */
		pthread_mutex_lock(&s->lock);
		value = s->colour_info != NULL ? CFRetain(s->colour_info) : new_dict();
		pthread_mutex_unlock(&s->lock);
	} else if (key_is(key, "SupportedPropertiesDictionary")) {
		value = CFRetain(s->supported);
	} else if (key_is(key, "FocalLength")) {
		value = number_f(s->camera->focal_length);     /* (the iPhone X's cameras', in millimetres) */
	} else if (key_is(key, "LensFNumber")) {
		value = number_f(s->camera->f_number);
	} else if (key_is(key, "PixelSize")) {
		/*
		 * The capture-graph builder REQUIRES this of the stream: -[BWFigVideoCaptureStream
		 * initWithCaptureStream:...] does a getProperty: (not IfSupported:) for it, and without it the
		 * init fails, so FigVideoCaptureSourcesActivateAndCreateDevices errors and the graph is never
		 * built. The sensor's pixel pitch in micrometres (the iPhone X cameras'); any value in
		 * (0, 100) is accepted.
		 */
		value = number_f(s->camera->pixel_size);
	} else if (key_is(key, "MinZoomFactor")) {
        value=number_f(s->camera->sensor==1&&access(rw_path("redwood-ultrawide-enable",(char[160]){0},160),F_OK)==0?0.5f:1.0f);
	} else if (key_is(key, "MaxZoomFactor")) {
		value = number_f(MAX_ZOOM);
	} else if (key_is(key, "ZoomFactor")) {
		value = number_f(s->zoom);
	} else if (key_is(key, "SupportedColorSpaces")) {
		CFNumberRef srgb = number_i(0);

		value = CFArrayCreate(NULL, (const void **)&srgb, 1, &kCFTypeArrayCallBacks);
		CFRelease(srgb);
	} else if (key_is(key, "ColorSpace") || key_is(key, "DeviceOrientation") || key_is(key, "FlashMode")) {
		value = number_i(0);
	} else if (key_is(key, "HasAutoFocusPositionSensor") || key_is(key, "HighCurrentTorchEnabled") ||
	    key_is(key, "DeferAdditionOfAttachments")) {
		value = CFRetain(kCFBooleanFalse);
	} else {
		say_once("stream: no answer for", key, NULL);
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static void torch_set(struct stream *s, CFTypeRef value);

static double
dict_number(CFDictionaryRef d, const char *name, double otherwise)
{
	CFStringRef k = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFTypeRef v = CFDictionaryGetValue(d, k);
	double n = otherwise;

	CFRelease(k);
	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
		CFNumberGetValue(v, kCFNumberDoubleType, &n);
	}
	return n;
}

/* AFWindowParams: the first window, as fractions of the frame; none (or not one) is the middle */
static void
af_window(struct stream *s, CFTypeRef value)
{
	CFDictionaryRef w = NULL;
	double x0, y0, width, height;

	if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() && CFArrayGetCount(value) > 0) {
		w = CFArrayGetValueAtIndex(value, 0);
	}
	if (w == NULL || CFGetTypeID(w) != CFDictionaryGetTypeID() || (width = dict_number(w, "width", 0)) <= 0 ||
	    (height = dict_number(w, "height", 0)) <= 0) {
		s->af_window[0] = s->af_window[1] = 0.3f;
		s->af_window[2] = s->af_window[3] = 0.7f;
		return;
	}
	x0 = dict_number(w, "hStart", 0) / 1000;
	y0 = dict_number(w, "vStart", 0) / 1000;
	width /= 1000;
	height /= 1000;
	/* (at least a tenth of the frame each way, about the middle of what was asked) */
	if (width < 0.1) {
		x0 -= (0.1 - width) / 2;
		width = 0.1;
	}
	if (height < 0.1) {
		y0 -= (0.1 - height) / 2;
		height = 0.1;
	}
	x0 = x0 < 0 ? 0 : x0 + width > 1 ? 1 - width : x0;
	y0 = y0 < 0 ? 0 : y0 + height > 1 ? 1 - height : y0;
	s->af_window[0] = (float)x0;
	s->af_window[1] = (float)y0;
	s->af_window[2] = (float)(x0 + width);
	s->af_window[3] = (float)(y0 + height);
}

static OSStatus
stream_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	struct stream *s = stream_of(o);

	if (key_is(key, "FormatIndex")) {
		int64_t i = 0;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberSInt64Type, &i);
		}
		if (i < 0 || i >= CFArrayGetCount(s->formats)) {
			return ERR_PARAM;
		}
		s->format_index = (int)i;
		say("stream: format %lld", (long long)i);
		return 0;
	}
	if (key_is(key, "ZoomFactor")) {
		/*
		 * The zoom: the camera's pictures -- moving and still alike -- made of the middle of the sensor
		 * (src/camera_isp.c, crop()), as the iPhone's ISP crops for it. Digital: no more detail than
		 * that part of the sensor has.
		 */
		double zoom = 1;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberDoubleType, &zoom);
		}
		zoom=s20_zoom_clamp((float)zoom,s->camera->sensor==1,access(rw_path("redwood-ultrawide-enable",(char[160]){0},160),F_OK)==0);
		if (fabs(zoom - s->zoom) >= 0.25 || (zoom == 1) != (s->zoom == 1)) {
			say("stream: zoom %.2fx", zoom);
		}
		s->zoom = (float)zoom;
		return 0;
	}
	if (key_is(key, "MinimumFrameRate") || key_is(key, "MaximumFrameRate")) {
		double fps = 0;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberDoubleType, &fps);
		}
		*(key_is(key, "MinimumFrameRate") ? &s->min_fps : &s->max_fps) = fps;
		say_once("stream: set", key, value);
		return 0;
	}
	if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled") || key_is(key, "VideoOutputHandlers")) {
		bool configs = key_is(key, "VideoOutputConfigurations"), enabled = key_is(key, "VideoOutputsEnabled");

		say_once("stream: set", key, value);
		if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
			return 0;
		}
		pthread_mutex_lock(&s->lock);
		CFIndex n = CFDictionaryGetCount(value);
		const void **keys = calloc((size_t)n + 1, sizeof(void *)), **values = calloc((size_t)n + 1, sizeof(void *));

		CFDictionaryGetKeysAndValues(value, keys, values);
		for (CFIndex i = 0; i < n; i++) {
			int k = CFGetTypeID(keys[i]) == CFStringGetTypeID() ? output_index(keys[i]) : -1;
			struct output *out;

			if (k < 0) {
				say_once("stream: an output not here:", keys[i], NULL);
				continue;
			}
			out = &s->out[k];
			if (configs) {
				if (out->config != NULL) {
					CFRelease(out->config);
				}
				let_go(out);
				out->config = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ? CFRetain(values[i]) : NULL;
			} else if (enabled) {
				int64_t on = 1;

				if (CFGetTypeID(values[i]) == CFBooleanGetTypeID()) {
					on = CFBooleanGetValue(values[i]);
				} else if (CFGetTypeID(values[i]) == CFNumberGetTypeID()) {
					CFNumberGetValue(values[i], kCFNumberSInt64Type, &on);
				}
				out->enabled = on != 0;
			} else {
				CFTypeRef h = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ?
				    CFDictionaryGetValue(values[i], CFSTR("PixelBufferHandler")) : NULL;

				/* Only a block is a handler: the graph also sends other objects here (kCFNull when it takes one
				 * away), and one of those kept and later called as a block was mediaserverd's SIGBUS; copying it
				 * as a block was the next one's SIGSEGV (2026-11-12). */
				const char *cls = h != NULL ? object_getClassName((id)h) : NULL;
				bool is_block = cls != NULL && strstr(cls, "Block") != NULL;

				if (h != NULL && !is_block) {
					say("stream: %s's PixelBufferHandler is a %s, not a block: no handler", out->name, cls ? cls : "?");
				}
				if (out->handler != NULL) {
					_Block_release(out->handler);
				}
				out->handler = is_block ? (void *)_Block_copy(h) : NULL;
			}
		}
		free(keys);
		free(values);
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (key_is(key, "StillImageCaptureNow")) {
		/*
		 * The shutter: the capture graph's still image coordinator asks for a photograph by writing
		 * this, with what it wants of it (how many pictures, the flash, the settings' number by which
		 * it will know the frames again). One frame of the sensor goes to the StillImage output for
		 * each ask.
		 */
		pthread_mutex_lock(&s->lock);
		s->stills_wanted++;
		if (s->flash_on && s->flash_ready == 0) {
			/* the screen lit (SpringBoard, inject_sb.c), and the photograph some frames later, as the exposure
			 * comes down to the lit face */
			s->flash_ready = s->frames + 10;
			flash_light(s, true);
			say("stream: the screen lit for the photograph (from frame %llu)", (unsigned long long)s->flash_ready);
		}
		pthread_mutex_unlock(&s->lock);
		say_value("stream: a photograph asked for", key, value);
		return 0;
	}
	if (key_is(key, "VideoOutputsColorInfo")) {
		pthread_mutex_lock(&s->lock);
		if (s->colour_info != NULL) {
			CFRelease(s->colour_info);
		}
		s->colour_info = value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID() ? CFRetain(value) : NULL;
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (key_is(key, "TorchLevel")) {
		torch_set(s, value);
		return 0;
	}
	if (key_is(key, "AutoFocusNow") || key_is(key, "ContinuousAutoFocusNow") || key_is(key, "FocusNow") ||
	    key_is(key, "PeakTrackingFocusNow") || key_is(key, "LockAFNow") || key_is(key, "ManualFocusNow") ||
	    key_is(key, "AFWindowParams") || key_is(key, "AutoFocusMode")) {
		/*
		 * Camera.app's tap: AutoFocusMode "AutoFocusModeWeightedSum", AFWindowParams an array of
		 * { hStart, vStart, width, height, weight } in thousandths of the frame (the first window is
		 * the one taken here; an empty value is the middle), then AutoFocusNow (true). LockAFNow
		 * keeps the lens where it is until focus is asked for again.
		 */
		if (!key_is(key, "AFWindowParams") || (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() &&
		    CFArrayGetCount(value) > 0)) {
			say_value("stream: focus:", key, value);        /* (an empty window, the middle: often, unsaid) */
		}
		if (key_is(key, "AFWindowParams")) {
			af_window(s, value);
		} else if (key_is(key, "LockAFNow")) {
			s->af_locked = true;
		} else if (key_is(key, "AutoFocusNow") || key_is(key, "ContinuousAutoFocusNow") || key_is(key, "FocusNow") ||
		    key_is(key, "PeakTrackingFocusNow")) {
			s->af_locked = false;
			s->af_now = true;
		}
		return 0;
	}
	if (key_is(key, "ExposureBias")) {
		/* a number, or { ExposureBiasValue, ExposureBiasCommandID }: 1/256 stop steps around 256? said, to learn */
		double v = 0;
		CFTypeRef n = value;

		if (value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
			n = CFDictionaryGetValue(value, CFSTR("ExposureBiasValue"));
		}
		if (n != NULL && CFGetTypeID(n) == CFNumberGetTypeID()) {
			CFNumberGetValue(n, kCFNumberDoubleType, &v);
			/* a multiplier, 256 being 1 (Camera.app's slider: 35 to 2715, about -2.9 to +3.4 stops) */
			s->ev_bias = v > 0 ? (float)log2(v / 256.0) : 0;
			s->ev_bias = s->ev_bias < -3.5f ? -3.5f : s->ev_bias > 3.5f ? 3.5f : s->ev_bias;
		}
		say_value("stream: exposure:", key, value);
		return 0;
	}
	if (key_is(key, "LockAENow") || key_is(key, "UnlockAENow")) {
		s->ae_locked = key_is(key, "LockAENow");
		say_value("stream: exposure:", key, value);
		return 0;
	}
	if (key_is(key, "FlashMode")) {
		s->flash_on = value != NULL && CFGetTypeID(value) == CFStringGetTypeID() && CFEqual(value, CFSTR("ForceFlashOn"));
		say_value("stream: flash:", key, value);
		return 0;
	}
	say_once("stream: set (not used)", key, value);
	return 0;
}

/* the flashlight: TorchLevel (0 to 1) as thousandths to the kernel's /dev/redwood-torch (torch=3) */
static void
torch_set(struct stream *s, CFTypeRef value)
{
	float level = 0;
	uint32_t thousandths;

	if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue(value, kCFNumberFloatType, &level);
	}
	level = level < 0 ? 0 : level > 1 ? 1 : level;
	thousandths = (uint32_t)(level * 1000 + 0.5f);
	if (s->torch < 0) {
		s->torch = open("/dev/redwood-torch", O_WRONLY);
	}
	if (s->torch < 0 || write(s->torch, &thousandths, sizeof(thousandths)) != (ssize_t)sizeof(thousandths)) {
		say("the flashlight at %u of 1000 NOT TAKEN (no /dev/redwood-torch)", thousandths);
	} else {
		say("the flashlight at %u of 1000", thousandths);
	}
}


/*
 * The flash for a photograph: the front camera's is the screen (SpringBoard, inject_sb.c), the rear's the LED beside
 * it -- the S2MPB02's, which the kernel has as the flashlight (/dev/redwood-torch), at its most (2026-11-12).
 */
static void
flash_light(struct stream *s, bool on)
{
	if (s->camera->sensor == 2) {
		notify_post(on ? "com.redwood.screenflash.on" : "com.redwood.screenflash.off");
		return;
	}
	uint32_t thousandths = on ? 1000 : 0;

	if (s->torch < 0) {
		s->torch = open("/dev/redwood-torch", O_WRONLY);
	}
	if (s->torch < 0 || write(s->torch, &thousandths, sizeof(thousandths)) != (ssize_t)sizeof(thousandths)) {
		say("the rear flash %s NOT TAKEN (no /dev/redwood-torch)", on ? "on" : "off");
	} else {
		say("the rear flash %s", on ? "on" : "off");
	}
}

/* ---- the frames ------------------------------------------------------------------------------------- */

struct render_job {
	const struct isp *isp;
	const struct redwood_camera_frame *f;
	const uint8_t *raw;
	CVPixelBufferRef pb;
	uint32_t format, width, height, pieces;
	bool still;                     /* the photograph: every pixel of the sensor (isp_render_still_nv12) */
};

static void
render_piece(void *context, size_t i)
{
	struct render_job *j = context;
	uint32_t rows = (j->height + j->pieces - 1) / j->pieces, first = (uint32_t)i * rows, end = first + rows;

	first &= ~1u;
	end = i + 1 == j->pieces ? j->height : end & ~1u;
	if (j->format == PF_BGRA) {
		isp_render_bgra(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddress(j->pb), CVPixelBufferGetBytesPerRow(j->pb), j->width,
		    j->height, first, end);
	} else if (j->still) {
		isp_render_still_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0),
		    CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0), CVPixelBufferGetBaseAddressOfPlane(j->pb, 1),
		    CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end, j->format == PF_420V);
	} else {
		isp_render_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0),
		    CVPixelBufferGetBaseAddressOfPlane(j->pb, 1), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end,
		    j->format == PF_420V);
	}
}

static CVPixelBufferRef
output_buffer(struct output *out)
{
	CVPixelBufferPoolRef pool = NULL;
	CVPixelBufferRef pb = NULL;
	CFTypeRef value = out->config != NULL ? CFDictionaryGetValue(out->config, CFSTR("BufferPool")) : NULL;

	/*
	 * The graph makes the pool itself and hands it over in the output's configuration
	 * (kFigCaptureStreamVideoOutputConfigurationKey_BufferPool), sized and laid out as the node
	 * behind the output wants it (an IOSurface each, extra rows at the bottom where a scaler wants
	 * them, as few buffers as the node holds at once) -- there is nothing for this side to choose,
	 * and a buffer of our own making is not one the graph would take. The configuration carries no
	 * width or height: the pool's buffers have them.
	 */
	if (value != NULL && CFGetTypeID(value) == CVPixelBufferPoolGetTypeID()) {
		pool = (CVPixelBufferPoolRef)value;
	}
	if (pool == NULL) {
		if (!out->said_shape) {
			out->said_shape = true;
			say("%s: no pool of the graph's in the configuration: no pictures", out->name);
		}
		return NULL;
	}
	for (unsigned i = 0; CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) != kCVReturnSuccess && i < HELD; i++) {
		/* a pool that will not grow that far: the oldest picture kept is the one to have */
		unsigned oldest = (out->held_next + i) % HELD;

		if (out->held[oldest] != NULL) {
			CVPixelBufferRelease(out->held[oldest]);
			out->held[oldest] = NULL;
		}
		if (!out->said_pool) {
			out->said_pool = true;
			say("%s: the graph's pool will not grow by %u: fewer pictures kept", out->name, HELD);
		}
	}
	if (pb == NULL) {
		/*
		 * The preview pool holds one buffer (MinimumBufferCount 1): while the graph still has it,
		 * there is none to be had, and this frame is not given out. Said at most every 300th frame.
		 */
		if (out->refused++ % 300 == 0) {
			say("%s: the graph's pool has no buffer to spare (%llu frames so)", out->name,
			    (unsigned long long)out->refused);
		}
		return NULL;
	}
	return pb;
}

typedef void (^pixel_buffer_handler)(CVPixelBufferRef, CMTime);

static void
let_go(struct output *out)
{
	for (unsigned i = 0; i < HELD; i++) {
		if (out->held[i] != NULL) {
			CVPixelBufferRelease(out->held[i]);
			out->held[i] = NULL;
		}
	}
}

static void
md_put(CFMutableDictionaryRef m, int key, CFTypeRef value)
{
	if (md_key[key] != NULL && value != NULL) {
		CFDictionarySetValue(m, md_key[key], value);
	}
	if (value != NULL) {
		CFRelease(value);
	}
}

/* the frame's metadata, as H10ISP's stream gives them (see md_names) */
static CFDictionaryRef
frame_metadata(const struct isp *isp, const struct redwood_camera_frame *f, const struct isp_stats *st, CFStringRef port)
{
	CFMutableDictionaryRef m;
	double exposure = f->exposure_lines * (double)f->line_ns / 1e9, analog = f->gain_unity ? (double)f->gain / f->gain_unity : 1;
	double total = analog * isp->digital, fps = f->frame_lines && f->line_ns ? 1e9 / ((double)f->frame_lines * f->line_ns) : 30;
	/* (green's level for a second at 1x, a quarter of it: a room's 70 or so, daylight's tens of thousands) */
	double lux = exposure > 0 && total > 0 ? st->mean[1] / (exposure * total) / 4 : 0;

	if (md_attachment == NULL) {
		return NULL;
	}
	m = (CFMutableDictionaryRef)new_dict();
	md_put(m, MD_PORT, CFRetain(port));
	md_put(m, MD_PTS, CMTimeCopyAsDictionary(CMClockMakeHostTimeFromSystemUnits(f->time), NULL));
	md_put(m, MD_EXPOSURE, number_f(exposure));
	md_put(m, MD_AGC, number_i((int64_t)(analog * 256 + 0.5)));
	md_put(m, MD_ISP_DGAIN, number_i((int64_t)(isp->digital * 256 + 0.5)));
	md_put(m, MD_SENSOR_DGAIN, number_i(256));
	md_put(m, MD_ISO_BASE, number_f(32));
	md_put(m, MD_FRAME_RATE, number_f(fps));
	md_put(m, MD_LUX, number_i((int64_t)(lux + 0.5)));
	md_put(m, MD_SNR, number_f(40 - 10 * log10(total < 1 ? 1 : total)));
	md_put(m, MD_AF_STATUS, number_i(0));
	md_put(m, MD_FOCUSING, number_i(0));
	md_put(m, MD_AE_STABLE, CFRetain(kCFBooleanTrue));
	md_put(m, MD_AE_LOCKED, CFRetain(kCFBooleanFalse));
	md_put(m, MD_AWB_STABLE, CFRetain(kCFBooleanTrue));
	md_put(m, MD_AWB_LOCKED, CFRetain(kCFBooleanFalse));
	md_put(m, MD_AF_LOCKED, CFRetain(kCFBooleanFalse));
	/*
	 * The zoom the picture was made with: the source node takes it for what the ISP applied
	 * (-[BWMultiStreamCameraSourceNode _ispAppliedZoomFactorFromSampleBufferMetadataDictionary:outputIndex:])
	 * and would make up the rest of what was asked for itself; without it, a zoomed preview stood still.
	 */
	/* Overlay lens selection leaves the graph request at1x. Report the
     * crop applied within the selected physical lens, not its nominal UI label. */
	md_put(m, MD_ZOOM, number_f(isp->zoom));
	return m;
}

static void
deliver(struct stream *s, struct output *out, const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw,
    CFDictionaryRef metadata)
{
	CVPixelBufferRef pb = output_buffer(out);
	uint32_t pf, width, height;

	if (pb == NULL) {
		return;
	}
	pf = CVPixelBufferGetPixelFormatType(pb);
	width = (uint32_t)CVPixelBufferGetWidth(pb);
	height = (uint32_t)CVPixelBufferGetHeight(pb);
	if (width == 0 || height == 0 || (pf != PF_420F && pf != PF_420V && pf != PF_BGRA)) {
		if (out->delivered == 0 && out->said_shape == false) {
			out->said_shape = true;
			say("%s: %ux%u %c%c%c%c buffers are not for this ISP", out->name, width, height, pf >> 24, pf >> 16 & 255,
			    pf >> 8 & 255, pf & 255);
		}
		CVPixelBufferRelease(pb);
		return;
	}
	CVPixelBufferLockBaseAddress(pb, 0);

	/* (a binned frame's pixels are each worth one of the picture's: demosaiced, not taken a 2x2 cell a pixel) */
	static int demosaic_binned = -1;
	if (demosaic_binned < 0) {
		demosaic_binned = access(rw_path("redwood-binned-demosaic", (char[160]){0}, 160), F_OK) == 0;
	}
	struct render_job job = { isp, f, raw, pb, pf, width, height, 48,
		(strcmp(out->name, "StillImage") == 0 || (f->width < 2000 && demosaic_binned)) && pf != PF_BGRA };
	uint64_t began = mach_absolute_time();

	/*
	 * The Back Camera is black for now (2026-11-12, the user: only the front is being tested, and both showing the
	 * same sensor made it unclear which is which). `touch /var/root/redwood-rear-live' gives it pictures again.
	 */
	if (s->camera->sensor == 1 && !s20_sensor_is_rear(f->sensor) && access(rw_path("redwood-rear-live", (char[160]){0}, 160), F_OK) != 0) {
		if (pf == PF_BGRA) {
			for (uint32_t r = 0; r < height; r++) {
				memset((uint8_t *)CVPixelBufferGetBaseAddress(pb) + r * CVPixelBufferGetBytesPerRow(pb), 0, (size_t)width * 4);
			}
		} else {
			for (uint32_t r = 0; r < height; r++) {
				memset((uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0) + r * CVPixelBufferGetBytesPerRowOfPlane(pb, 0),
				    pf == PF_420V ? 16 : 0, width);
			}
			for (uint32_t r = 0; r < height / 2; r++) {
				memset((uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 1) + r * CVPixelBufferGetBytesPerRowOfPlane(pb, 1), 128, width);
			}
		}
	} else {
		/*
		 * The rear's pictures below the video encoder's priority: its preview costs twice the front's (cells enlarged
		 * to the picture), and at 95 C, throttled, at USER_INTERACTIVE it starved VideoToolbox's encoder -- the first
		 * compressed frame came 18-28 s after a recording began, its Stop never reached the file, and no rear video
		 * was ever finished (2026-11-12). The front keeps the higher class.
		 */
		bool rear_frame = s20_sensor_is_rear(f->sensor);

		dispatch_apply_f(job.pieces, dispatch_get_global_queue(rear_frame ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INTERACTIVE, 0),
		    &job, render_piece);
	}
	bool photograph = strcmp(out->name, "StillImage") == 0;

	if (!photograph) {
		s->render_ticks += mach_absolute_time() - began;
	}
	/* redwood-still-dump in the tuning directory: the photo's luma plane as this ISP made it (before iOS's own stages),
	 * once, as redwood-still.y (width x height bytes, rows packed) */
	if (photograph && pf != PF_BGRA) {
		char flag[160];

		if (access(rw_path("redwood-still-dump", flag, sizeof(flag)), F_OK) == 0) {
			FILE *yf = fopen(rw_path("redwood-still.y", (char[160]){0}, 160), "w");

			unlink(flag);
			if (yf != NULL) {
				const uint8_t *yp = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
				size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);

				for (uint32_t r = 0; r < height; r++) fwrite(yp + r * ys, 1, width, yf);
				fclose(yf);
				say("still luma written (%ux%u)", width, height);
			}
		}
	}
	if (photograph) {
		static mach_timebase_info_data_t tb;

		if (tb.denom == 0) {
			mach_timebase_info(&tb);
		}
		say("%s: %ux%u made of every pixel in %.0f ms", out->name, width, height,
		    (double)(mach_absolute_time() - began) * tb.numer / tb.denom / 1e6);
	}
	CVPixelBufferUnlockBaseAddress(pb, 0);
	CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
	    kCVAttachmentMode_ShouldPropagate);

	if (metadata != NULL) {
		CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(NULL, 0, metadata);

		CVBufferSetAttachment(pb, md_attachment, m, kCVAttachmentMode_ShouldPropagate);
		CFRelease(m);
	}

	CMTime pts = CMClockMakeHostTimeFromSystemUnits(f->time);

	/*
	 * A photograph's time a little before its frame's: Live Photo's staging node builds the movie around the still's
	 * time and wants it inside the video it has already staged (an iPhone's still comes from its time machine);
	 * ours was the newest frame, at the very edge -- "still image host time is before earliest allowed host PTS,
	 * but it's later than last staged video buffer PTS", no movie (2026-11-12).
	 */
	if (strcmp(out->name, "StillImage") == 0) {
		pts = CMTimeSubtract(pts, CMTimeMake(100, 1000));
	}

	((pixel_buffer_handler)out->handler)(pb, pts);
	/*
	 * The moving picture's buffers are kept for HELD frames after they are given. The preview's pool
	 * would otherwise hand a buffer straight back -- the graph lets go of it as soon as it has passed
	 * it on -- while it is still on its way to the screen (the preview sink, Camera.app's image queue,
	 * the window server copying it), and the next frame drawn into it showed as bands of two frames,
	 * torn across. Kept, they make the pool take others. (One kept was not enough.)
	 */
	if (strcmp(out->name, "StillImage") != 0) {
		if (out->held[out->held_next] != NULL) {
			CVPixelBufferRelease(out->held[out->held_next]);
		}
		out->held[out->held_next] = pb;
		out->held_next = (out->held_next + 1) % HELD;
	} else {
		CVPixelBufferRelease(pb);
	}
	if (out->delivered++ == 0) {
		say("%s: the first picture given (%ux%u %c%c%c%c)", out->name, width, height, pf >> 24, pf >> 16 & 255,
		    pf >> 8 & 255, pf & 255);
	}
}

/*
 * The scene's light as the cameras' tuning measures it (Qualcomm's lux index, higher is darker): about
 * 46 a doubling of exposure times gain, 197 at a 33 ms exposure and 1x -- the black level's ranges in
 * the tuning of the wide camera pair lux indexes with gains, 195-200 with 1-1.1x and 380-385 with
 * 15.9-16x, at the longest exposure a frame at 30 a second allows.
 */
static float
lux_index(const struct isp *isp, const struct redwood_camera_frame *f)
{
	double exposure = f->exposure_lines * (double)f->line_ns / 1e9 * f->gain / f->gain_unity * isp->digital;

	return (float)(197 + 46 * log2(exposure > 1e-6 ? exposure / 0.0333 : 1e-6 / 0.0333));
}

/*
 * The light's colour temperature: where the white balance is on the line of the tuning's reference
 * lights; without them, guessed from how much more the blue is raised than the red, by what is typical of a phone's sensor --
 * blue's gain over red's about 0.8 in daylight (6500 K), 1.3 under a fluorescent tube (4000 K), 2 under
 * a filament lamp (2850 K) -- between those on the logarithm of the ratio, and no further.
 */
static float
colour_temperature(const struct isp *isp)
{
	static const float ratio[3] = { 0.8f, 1.3f, 2.0f }, kelvin[3] = { 6500, 4000, 2850 };
	float r = isp->wb[0] > 0 ? isp->wb[2] / isp->wb[0] : 1, cct = isp_cct(isp);

	if (cct > 0) {
		return cct;     /* (by the tuning's reference lights) */
	}

	if (r <= ratio[0]) {
		return kelvin[0];
	}
	for (int i = 1; i < 3; i++) {
		if (r <= ratio[i]) {
			float t = logf(r / ratio[i - 1]) / logf(ratio[i] / ratio[i - 1]);

			return kelvin[i - 1] + t * (kelvin[i] - kelvin[i - 1]);
		}
	}
	return kelvin[2];
}

/*
 * Focus, where the camera has a lens motor (the frames' focus is a position, not REDWOOD_CAMERA_NO_FOCUS:
 * the wide camera's): by contrast, as there is nothing else to go by -- the sensor's phase-detection
 * pixels are not read. A search climbs from where the lens is: a step of AF_STEP one way, on that way
 * while the middle of the picture gets sharper, back the other way if the first step was not, until
 * two steps in a row are less sharp than the sharpest (or the travel, AF_NEAR to AF_FAR, ends); then
 * around the sharpest in steps of AF_FINE, and there it stays. Each place is measured two frames after
 * the lens was told to go there (it settles in less than one). The lens is only ever moved a step at a
 * time: thrown across its travel it knocked, audibly, against its stop at 0. In focus, it stays until
 * the middle has been a good deal less sharp or quite differently lit for a second, and then until
 * the picture has kept still for a third of one: no searching while the phone is being moved.
 * A focus asked for (a tap: AutoFocusNow, on the window AFWindowParams gave) sweeps instead: the
 * lens is walked to the nearer end of its travel, then measured towards the other in steps of
 * AF_STEP until it is past the sharpest (two steps in a row well under it), then around the sharpest
 * -- it finds what was tapped however far out of focus it was, where the climb gave up on the flat
 * of the curve.
 */
enum { AF_WATCH, AF_STEADY, AF_CLIMB, AF_SWEEP, AF_FINE_SEARCH, AF_SETTLE };
#define AF_NEAR         16              /* (off the stop at 0) */
#define AF_FAR          768             /* (its module file's actuator region goes to 700; near 1023 the PM8008i's LDO5 said over-current) */
#define AF_STEP         48
#define AF_FINE         12

struct af {
	int             state;
	uint32_t        at, target;     /* where the lens was told to go; where it is going a step at a time */
	unsigned        wait;           /* frames before the next measurement */
	int             way;            /* +1 nearer, -1 further */
	bool            turned;
	unsigned        worse;          /* steps in a row less sharp than the sharpest */
	uint32_t        start, best, fine_to;
	float           best_sharpness;
	float           sharpness, level;       /* in focus: the middle's, to watch */
	float           last_sharpness, last_level;
	unsigned        unlike, steady, steps;
	bool            now;            /* asked for: no waiting for the picture to keep still */
	bool            sweep;          /* ... and the whole travel measured */
	float           sum_sharpness, sum_level;
	unsigned        measured;
};

static void
af_go(int device, struct af *af, uint32_t target)
{
	struct redwood_camera_command c = { .command = REDWOOD_CAMERA_FOCUS };

	af->target = target < AF_NEAR ? AF_NEAR : target > AF_FAR ? AF_FAR : target;
	c.a = af->at < af->target ? (af->target - af->at > AF_STEP ? af->at + AF_STEP : af->target) :
	    (af->at - af->target > AF_STEP ? af->at - AF_STEP : af->target);
	af->at = c.a;
	/*
	 * The lens settles in a few milliseconds; what is spoilt is the frame being exposed while it moved
	 * (written at a frame's end, the next one's exposure has begun): one frame let go by before a
	 * measurement, none on the way.
	 */
	af->wait = af->at == af->target ? 1 : 0;
	af->steps++;
	af->sum_sharpness = af->sum_level = 0;
	af->measured = 0;
	(void)write(device, &c, sizeof(c));
}

static void
af_frame(int device, struct af *af, const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, const float window[4], bool locked)
{
	float level, sharpness;
	unsigned needed = f->gain >= 16 * f->gain_unity ? 2 : 1;      /* (in the dark, two frames to a measurement) */

	if (f->focus == REDWOOD_CAMERA_NO_FOCUS) {
		return;
	}
	if (af->at == 0 && af->target == 0) {
		af->at = af->target = f->focus;         /* (where the kernel put it) */
	}
	if (af->wait > 0) {
		af->wait--;
		return;
	}
	if (af->at != af->target) {
		af_go(device, af, af->target);          /* (the next step of a longer way) */
		return;
	}
	sharpness = isp_sharpness(isp, f, raw, window[0], window[1], window[2], window[3], &level);
	af->sum_sharpness += sharpness;
	af->sum_level += level;
	if (++af->measured < needed) {
		return;
	}
	sharpness = af->sum_sharpness / af->measured;
	level = af->sum_level / af->measured;
	af->sum_sharpness = af->sum_level = 0;
	af->measured = 0;
	switch (af->state) {
	case AF_WATCH:
		if (locked || (af->sharpness > 0 && sharpness > af->sharpness * 0.6f && level > af->level * 0.7f &&
		    level < af->level * 1.4f)) {
			af->unlike = 0;
			return;
		}
		if (af->sharpness > 0 && ++af->unlike < 30) {
			return;
		}
		af->state = AF_STEADY;
		af->steady = 0;
		af->last_sharpness = sharpness;
		af->last_level = level;
		return;
	case AF_STEADY:
		if (fabsf(sharpness - af->last_sharpness) < 0.15f * af->last_sharpness && fabsf(level - af->last_level) < 0.1f * af->last_level) {
			af->steady++;
		} else {
			af->steady = 0;
		}
		af->last_sharpness = sharpness;
		af->last_level = level;
		if (af->steady < 10 && !af->now) {
			return;
		}
		af->now = false;
		if (af->sweep) {
			af->sweep = false;
			af->state = AF_SWEEP;
			af->best_sharpness = -1;
			af->worse = 0;
			af->steps = 0;
			af->way = af->at - AF_NEAR < AF_FAR - af->at ? 1 : -1;
			af_go(device, af, af->way > 0 ? AF_NEAR : AF_FAR);     /* (walked there a step at a time) */
			return;
		}
		af->state = AF_CLIMB;
		af->best = af->start = af->at;
		af->best_sharpness = sharpness;
		af->way = af->at + AF_STEP > AF_FAR ? -1 : 1;
		af->turned = false;
		af->worse = 0;
		af->steps = 0;
		af_go(device, af, (uint32_t)((int)af->at + af->way * AF_STEP));
		return;
	case AF_CLIMB: {
		int next = (int)af->at + af->way * AF_STEP;
		bool end = next < AF_NEAR || next > AF_FAR;

		if (sharpness > af->best_sharpness * 1.02f) {
			af->best = af->at;
			af->best_sharpness = sharpness;
			af->worse = 0;
		} else {
			af->worse++;
		}
		if (af->worse < 2 && !end) {
			af_go(device, af, (uint32_t)next);
			return;
		}
		next = (int)af->best - af->way * AF_STEP;
		if (!af->turned && af->best == af->start && next >= AF_NEAR && next <= AF_FAR) {
			/* not sharper that way at all: the other way, from where it started */
			af->turned = true;
			af->way = -af->way;
			af->worse = 0;
			af_go(device, af, (uint32_t)next);
			return;
		}
		af->state = AF_FINE_SEARCH;
		af->fine_to = af->best + 2 * AF_FINE > AF_FAR ? AF_FAR : af->best + 2 * AF_FINE;
		af_go(device, af, af->best > 2 * AF_FINE + AF_NEAR ? af->best - 2 * AF_FINE : AF_NEAR);
		return;
	}
	case AF_SWEEP:
		say("focus: sweep: %u %.4f", af->at, sharpness);
		if (sharpness > af->best_sharpness) {
			af->best = af->at;
			af->best_sharpness = sharpness;
			af->worse = 0;
		} else if (sharpness < af->best_sharpness * 0.85f) {
			af->worse++;
		}
		if (af->worse < 2 && (int)af->at + af->way * AF_STEP >= AF_NEAR && (int)af->at + af->way * AF_STEP <= AF_FAR) {
			af_go(device, af, (uint32_t)((int)af->at + af->way * AF_STEP));
			return;
		}
		af->state = AF_FINE_SEARCH;
		af->fine_to = af->best + 2 * AF_FINE > AF_FAR ? AF_FAR : af->best + 2 * AF_FINE;
		af_go(device, af, af->best > 2 * AF_FINE + AF_NEAR ? af->best - 2 * AF_FINE : AF_NEAR);
		return;
	case AF_FINE_SEARCH:
		if (sharpness > af->best_sharpness) {
			af->best = af->at;
			af->best_sharpness = sharpness;
		}
		if (af->at + AF_FINE <= af->fine_to) {
			af_go(device, af, af->at + AF_FINE);
			return;
		}
		af->state = AF_SETTLE;
		af_go(device, af, af->best);
		return;
	case AF_SETTLE:
		af->state = AF_WATCH;
		af->sharpness = sharpness;
		af->level = level;
		af->unlike = 0;
		say("focus: the lens at %u after %u moves (sharpness %.3f)", af->at, af->steps, sharpness);
		return;
	}
}

/* the sensor's colour tuning, when the image has one (fw/camera-<sensor>.isp, tools/camtuning.py) */
static void
load_tuning(struct isp *isp, uint32_t sensor)
{
	static const char *const names[] = { "ov02b10", "s5khm2sd", "ov16a1q", "s5k2la" };
	char path[96];

	if (sensor >= sizeof(names) / sizeof(names[0])) {
		return;
	}
	/* the phone's own (made on it, see tune_on_phone()) before the image's */
	snprintf(path, sizeof(path), "%s/camera-%s.isp", tuning_dir(), names[sensor]);
	if (access(path, R_OK) != 0) {
		snprintf(path, sizeof(path), "/redwood/fw/camera-%s.isp", names[sensor]);
	}
	if (isp_load_tuning(isp, path)) {
		say("the %s's colour tuning from %s: %u colour matrices' lights, %u lens shadings'", names[sensor], path,
		    isp->tuning->ccm.lights, isp->tuning->lsc.lights);
	} else {
		say("no colour tuning for the %s (%s): no colour matrix, no lens shading", names[sensor], path);
		isp_set_ccm(isp, (const float[9]){ 1, 0, 0, 0, 1, 0, 0, 0, 1 });
		isp->lsc.on = false;
	}
}


/*
 * Colour tuning made on the phone (2026-11-12; the S20's front SAK3J1 had none: no colour matrix, no lens shading).
 *   touch /var/root/redwood-lsc-capture   the next frame is a flat field (a white sheet over the lens, evenly
 *                                         lit): the lens shading is measured from it
 *   /var/root/redwood-ccm                 the colour matrix, nine numbers row by row (camera RGB, white balanced,
 *                                         to linear sRGB; each row summing to 1); a generic phone sensor's if absent
 *   touch /var/root/redwood-tuning-reload the matrix file read again (the lens shading kept)
 * Both are written into /var/root/camera-<sensor>.isp (RWCISP02, as tools/camtuning.py makes them), which
 * load_tuning() takes before the image's.
 */

/* a directory mediaserverd's sandbox lets it read and write (its /var/root is closed to it) */
static const char *
tuning_dir(void)
{
	static const char *dir;
	static const char *const dirs[] = { "/var/mobile/Library/Caches/com.apple.mediaserverd", "/var/mobile/Library/Caches",
		"/var/tmp", "/tmp", "/var/mobile/Media", "/var/root" };

	if (dir != NULL) {
		return dir;
	}
	for (int i = 0; i < 6; i++) {
		char t[160];
		FILE *x;

		snprintf(t, sizeof(t), "%s/redwood-write-test", dirs[i]);
		x = fopen(t, "w");
		say("tuning: %s: %s", dirs[i], x ? "writable" : strerror(errno));
		if (x != NULL) {
			fclose(x);
			unlink(t);
			dir = dirs[i];
			return dir;
		}
	}
	dir = "/var/root";
	return dir;
}


static const char *
rw_path(const char *leaf, char *buf, size_t size)
{
	snprintf(buf, size, "%s/%s", tuning_dir(), leaf);
	return buf;
}

#define LSC_GRID (LSC_W * LSC_H)
static float phone_lsc[3][LSC_H][LSC_W];
static bool phone_lsc_have;
/*
 * Whose lens shading phone_lsc is. It was one map for every sensor: once the FRONT lens was measured (white paper),
 * the rear's tuning file was written with the front's shading too -- byte for byte the same 3.5 KB (2026-11-12), a
 * fixed colour and brightness gradient over every rear picture that dark scenes made plain. A map now belongs to
 * the sensor it was measured on, and another sensor's file gets gains of 1 until its own lens is measured.
 */
static int phone_lsc_owner = -1;
#define REAR_FULL_BAYER 0      /* the rear main's 4032x3024 frames (see redwood-rear-bayer) */

static void
read_ccm(float m[9], const char *name)
{
	static const float generic[9] = { 1.72f, -0.56f, -0.16f, -0.28f, 1.58f, -0.30f, 0.02f, -0.66f, 1.64f };
	/* the sensor's own (redwood-ccm-<sensor>: the front SAK3J1 and the rear S5K2LD see colours differently -- the
	 * front's matrix made the rear green, 2026-11-12), else the shared redwood-ccm */
	char own[96];
	FILE *fp;

	snprintf(own, sizeof(own), "redwood-ccm-%s", name);
	fp = fopen(rw_path(own, (char[160]){0}, 160), "r");
	if (fp == NULL) {
		fp = fopen(rw_path("redwood-ccm", (char[160]){0}, 160), "r");
	}
	int n = 0;

	memcpy(m, generic, sizeof(generic));
	if (fp != NULL) {
		float v[9];

		for (n = 0; n < 9 && fscanf(fp, " %f ,", &v[n]) == 1; n++) {
		}
		fclose(fp);
		if (n == 9) {
			memcpy(m, v, sizeof(v));
		}
	}
}

static bool
write_tuning(const char *name, const float ccm[9])
{
	char path[96];
	FILE *fp;
	uint32_t one = 1, zero = 0;
	float range[2] = { -1e9f, 1e9f };

	snprintf(path, sizeof(path), "%s/camera-%s.isp", tuning_dir(), name);
	if ((fp = fopen(path, "wb")) == NULL) {
		static const char *const dirs[] = { "/var/tmp", "/tmp", "/var/mobile/Library/Caches", "/var/mobile/Media" };

		say("tuning: %s: %s", path, strerror(errno));
		for (int i = 0; i < 4; i++) {
			char t[128];
			FILE *x;

			snprintf(t, sizeof(t), "%s/redwood-write-test", dirs[i]);
			x = fopen(t, "w");
			say("tuning: can write %s: %s", dirs[i], x ? "yes" : strerror(errno));
			if (x) {
				fclose(x);
				unlink(t);
			}
		}
		return false;
	}
	fwrite("RWCISP02", 1, 8, fp);
	/* the matrix: one light range, one temperature range */
	fwrite(&one, 4, 1, fp);
	fwrite(range, 4, 2, fp);
	fwrite(&one, 4, 1, fp);
	fwrite(range, 4, 2, fp);
	fwrite(ccm, 4, 9, fp);
	/* the lens shading: R, Gr, Gb, B (G twice), or none (gains of 1) */
	fwrite(&one, 4, 1, fp);
	fwrite(range, 4, 2, fp);
	fwrite(&one, 4, 1, fp);
	fwrite(range, 4, 2, fp);
	for (int c = 0; c < 4; c++) {
		int k = c == 0 ? 0 : c == 3 ? 2 : 1;

		for (int gy = 0; gy < LSC_H; gy++) {
			for (int gx = 0; gx < LSC_W; gx++) {
				float g = phone_lsc_have ? phone_lsc[k][gy][gx] : 1.0f;

				fwrite(&g, 4, 1, fp);
			}
		}
	}
	fwrite(&zero, 4, 1, fp);        /* no reference lights */
	fclose(fp);
	return true;
}

static void
tune_on_phone(struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw)
{
	static const char *const names[] = { "ov02b10", "s5khm2sd", "ov16a1q", "s5k2la" };
	const char *name = f->sensor < 4 ? names[f->sensor] : NULL;
	char cap_p[160], rel_p[160];
	bool capture = access(rw_path("redwood-lsc-capture", cap_p, sizeof(cap_p)), F_OK) == 0,
	    reload = access(rw_path("redwood-tuning-reload", rel_p, sizeof(rel_p)), F_OK) == 0;
	float ccm[9];

	if (isp->trim[1] == 0 && access(rw_path("redwood-ccm", (char[160]){0}, 160), R_OK) == 0) {
		reload = true;          /* (a new stream: the matrix and the trim taken again) */
	}
	if (name == NULL || (!capture && !reload)) {
		return;
	}
	if (phone_lsc_owner != (int)f->sensor) {
		phone_lsc_have = false;
		phone_lsc_owner = (int)f->sensor;
	}
	unlink(cap_p);
	unlink(rel_p);
	if (!phone_lsc_have && isp->tuning != NULL && isp->tuning->lsc.lights > 0) {
		/* (the lens shading of the file already there, kept when only the matrix changes) */
		memcpy(phone_lsc, isp->lsc.gain, sizeof(phone_lsc));
		phone_lsc_have = true;
	}
	if (capture) {
		struct isp tmp = *isp;
		float darkest = 0;

		if (!isp_lsc_from_flat(&tmp, f, raw, &darkest)) {
			say("tuning: that frame is no flat field (too dark, or clipped): nothing measured");
			return;
		}
		memcpy(phone_lsc, tmp.lsc.gain, sizeof(phone_lsc));
		phone_lsc_have = true;
		say("tuning: the lens shading measured (the corners at %.0f %% of the centre)", darkest * 100);
	}
	read_ccm(ccm, name);
	{
		/* redwood-wb-trim: "R B", the white balance's red and blue made that much stronger (a cast the
		 * grey-world estimate leaves: green, 2026-11-12) */
		FILE *tf = fopen(rw_path("redwood-wb-trim", (char[160]){0}, 160), "r");
		float tr = 1, tb = 1;

		if (tf != NULL) {
			if (fscanf(tf, "%f %f", &tr, &tb) != 2) {
				tr = tb = 1;
			}
			fclose(tf);
		}
		isp->trim[0] = tr;
		isp->trim[1] = 1;
		isp->trim[2] = tb;
		say("tuning: white balance trim R %.3f B %.3f", tr, tb);
	}
	if (write_tuning(name, ccm)) {
		load_tuning(isp, f->sensor);
		say("tuning: camera-%s.isp written; matrix %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f", name, ccm[0],
		    ccm[1], ccm[2], ccm[3], ccm[4], ccm[5], ccm[6], ccm[7], ccm[8]);
	}
}

static int one_sensor = -1;             /* the S20 (kern.redwood.camera_turn there): one sensor for both cameras */
static void *worker_body(void *context);

/* the worker, and a flag when it is over (stream_stop does not wait for ever for it) */
static void *
worker(void *context)
{
	struct stream *s = stream_of(context);
	void *r = worker_body(context);

	s->worker_busy = false;
	return r;
}

/*
 * The frames' drawing on a thread of its own (2026-11-12): the worker reads frame n+1 from the kernel and measures
 * it while frame n is drawn into the graph's buffers and given -- one after the other the two were 36 ms a frame,
 * under the sensor's 30 a second.
 */
struct render_handoff {
	pthread_mutex_t lock;
	pthread_cond_t  cond;
	bool            has_job, quit;
	struct stream   *s;
	struct isp      isp;            /* the ISP's state as the frame was measured */
	const uint8_t   *buffer;        /* the frame (header first) */
	CFDictionaryRef metadata;
};

static void
give_frame(struct stream *s, const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw,
    CFDictionaryRef metadata)
{
	pthread_mutex_lock(&s->lock);
	for (int i = 0; s->graph_running && i < OUTPUTS; i++) {
		struct output *out = &s->out[i];

		/*
		 * The scalers' outputs are the moving picture and are given every frame. The still image
		 * output is the whole sensor (4032x3024) and is the photograph: a frame goes to it only
		 * when the graph has asked for one, which is not something to make thirty times a second.
		 */
		bool still = strcmp(out->name, "StillImage") == 0;

		if (out->handler == NULL || out->config == NULL || !(out->enabled || i == 0)) {
			continue;
		}
		if (still && s->stills_wanted == 0) {
			continue;
		}
		if (still && s->flash_ready != 0 && s->frames < s->flash_ready) {
			continue;               /* (the screen's light not in the exposure yet) */
		}
		deliver(s, out, isp, f, raw, metadata);
		if (still) {
			s->stills_wanted--;
			say("the photograph is given (%llu so far)", (unsigned long long)++s->stills_given);
			if (s->stills_wanted == 0 && s->flash_ready != 0) {
				s->flash_ready = 0;
				flash_light(s, false);
			}
		}
	}
	pthread_mutex_unlock(&s->lock);
}

static void *
render_body(void *context)
{
	struct render_handoff *h = context;

	pthread_setname_np("redwood-camera-draw");
	pthread_mutex_lock(&h->lock);
	for (;;) {
		while (!h->has_job && !h->quit) {
			pthread_cond_wait(&h->cond, &h->lock);
		}
		if (!h->has_job && h->quit) {
			break;
		}
		pthread_mutex_unlock(&h->lock);
		const struct redwood_camera_frame *f = (const void *)h->buffer;

		give_frame(h->s, &h->isp, f, h->buffer + f->header_bytes, h->metadata);
		if (h->metadata != NULL) {
			CFRelease(h->metadata);
		}
		pthread_mutex_lock(&h->lock);
		h->metadata = NULL;
		h->has_job = false;
		pthread_cond_broadcast(&h->cond);
	}
	pthread_mutex_unlock(&h->lock);
	return NULL;
}

/* until the drawing thread has given the frame before (its buffer free again) */
static void
render_wait(struct render_handoff *h)
{
	pthread_mutex_lock(&h->lock);
	while (h->has_job) {
		pthread_cond_wait(&h->cond, &h->lock);
	}
	pthread_mutex_unlock(&h->lock);
}

static void *
worker_body(void *context)
{
	CMBaseObjectRef o = context;
	struct stream *s = stream_of(o);
	struct redwood_camera_frame head;
	struct isp isp;
	struct isp_black_calibration rear_dark;
	struct isp_stats st;
	uint8_t *buffer = NULL, *buffers[2] = { NULL, NULL };
	size_t bytes = 0;
	int next_buffer = 0;
	struct render_handoff *h = calloc(1, sizeof(*h));
	pthread_t drawer;
	bool drawer_up = false, asked_binned = false, asked_sensor = false;
	unsigned skipped_other = 0;
	uint32_t tuned_for = ~0u;
	bool switched = false;
    bool aux_enabled=access(rw_path("redwood-ultrawide-enable",(char[160]){0},160),F_OK)==0;
    bool tele_enabled=access(rw_path("redwood-gw2-enable",(char[160]){0},160),F_OK)==0;
    unsigned rear_mode=~0U;
	float lux = -1, cct = -1;
	struct af af = { 0 };
	int device = open(REDWOOD_CAMERA_DEVICE, O_RDWR), failures = 0;

	pthread_setname_np("redwood-camera");
	if (device < 0) {
		say("no %s (%s): no frames", REDWOOD_CAMERA_DEVICE, strerror(errno));
		return NULL;
	}
	/*
	 * The S20: say which sensor this camera is BEFORE the first read. The kernel takes a sensor change between
	 * sessions at once, and the first read starts a session with whatever sensor was last -- so opening the
	 * Front Camera after the Back one started the rear (its ~3 s of tables) only to stop it again for the
	 * front (2026-11-12). Asked here, the first session is already this camera's; the header check below
	 * stays as the safety net.
	 */
	if (one_sensor < 0) {
		int t;
		size_t tl = sizeof(t);
		one_sensor = sysctlbyname("kern.redwood.camera_turn", &t, &tl, NULL, 0) == 0;
	}
	if (one_sensor > 0 && access(rw_path("redwood-rear-off", (char[160]){0}, 160), F_OK) != 0) {
		bool want_rear = s->camera->sensor == 1;
		bool binned = access(rw_path("redwood-binned-stream", (char[160]){0}, 160), F_OK) == 0;
		struct redwood_camera_command rc = { .command = REDWOOD_CAMERA_SENSOR, .a = want_rear ? 1 : 2,
			.b = want_rear ? s20_zoom_mode_tele(s->zoom,aux_enabled,tele_enabled) : binned ? 1 : 0 };

		(void)write(device, &rc, sizeof(rc));
		say("the %s asked for before the first frame", want_rear ? "rear main" : "front sensor");
	}
	isp_init(&isp);
	memset(&rear_dark, 0, sizeof(rear_dark));
	if (s->camera->sensor == 1 && isp_load_black_calibration(&rear_dark,
	    rw_path("redwood-rear-dark-grid", (char[160]){0}, 160))) {
		say("rear dark-grid loaded: %u gains, exact exposure %u lines", rear_dark.profiles, rear_dark.exposure_lines);
	}
	{
		/* the tone curve's contrast (redwood-contrast in the tuning directory, 0 to 1; 0.35 if absent) */
		FILE *cf = fopen(rw_path("redwood-contrast", (char[160]){0}, 160), "r");
		float contrast = 0.35f;

		if (cf != NULL) {
			if (fscanf(cf, "%f", &contrast) != 1) contrast = 0.35f;
			fclose(cf);
		}
		isp_set_tone(&isp, contrast);
		say("tone: contrast %.2f", contrast);
	}
	/*
	 * The white balance where the last stream left it, and a quick one for the first frames: from gains of 1 every
	 * start was green for a second or two (the raw frame's green is the strongest; 2026-11-12).
	 */
	static float last_wb[3];
	if (last_wb[0] > 0) {
		memcpy(isp.wb, last_wb, sizeof(last_wb));
		isp.awb_fast = 4;
	} else {
		isp.awb_fast = 12;
	}
	while (s->running && failures < 5) {
        float requested_zoom=s->zoom;
        if(aux_enabled&&s->camera->sensor==1){
            /* Development override exercises the SAME route as ZoomFactor until
             * the stock Camera UI advertises fractional zoom on its D22 profile. */
            FILE *lf=fopen(rw_path("redwood-rear-zoom",(char[160]){0},160),"r");
            if(lf){float z;if(fscanf(lf,"%f",&z)==1)requested_zoom=s20_zoom_clamp(z,true,true);fclose(lf);}
            unsigned next=s20_zoom_mode_tele(requested_zoom,true,tele_enabled);
            if(rear_mode!=next){
                if(drawer_up)render_wait(h);
                free(buffers[0]);free(buffers[1]);buffers[0]=buffers[1]=buffer=NULL;bytes=0;next_buffer=0;
                struct redwood_camera_command lc={.command=REDWOOD_CAMERA_SENSOR,.a=next==5?3:1,.b=next};
                if(write(device,&lc,sizeof(lc))!=sizeof(lc)){say("rear lens request failed: %s",strerror(errno));break;}
                rear_mode=next;asked_sensor=false;skipped_other=0;tuned_for=~0U;lux=cct=-1;
                isp.awb_fast=12;isp.wb[0]=isp.wb[1]=isp.wb[2]=1;
                say("rear lens requested: %s",next==6?"GW2 tele crop":next==5?"2LA ultra-wide":"2LD main");
            }
        }
        ssize_t n;

		if (buffer == NULL) {
			n = read(device, &head, sizeof(head));
			if (n != (ssize_t)sizeof(head)) {
				failures += n < 0 && errno == EAGAIN ? 0 : 1;
				if (n < 0 && errno == EIO) {
					say("the kernel's camera has had a fault: no frames");
					break;
				}
				continue;
			}
			/*
			 * The Galaxy S20 (kern.redwood.camera_turn is its): both of iOS's cameras are the one front SAK3J1 so
			 * far, so the kernel's sensor is taken as it is -- the switch restarted the sensor (about 0.7 s) at
			 * every camera start and every flip (2026-11-12). The back camera's pictures are black anyway.
			 */
			if (one_sensor < 0) {
				int t;
				size_t tl = sizeof(t);
				one_sensor = sysctlbyname("kern.redwood.camera_turn", &t, &tl, NULL, 0) == 0;
			}
			/*
			 * The S20 with its rear main up (2026-11-12, kernel REDWOOD_CAMERA_SENSOR mode 2): the Back Camera's frames
			 * are the S5K2LD's 2016x1512, the Front Camera's the SAK3J1's (3648 or 1824 wide). A stream whose frames are
			 * the other camera's asks the kernel once (its sensor restarts: ~5 s for the rear's tables so far).
			 * redwood-rear-off in the tuning directory keeps the old way (both cameras the front sensor).
			 */
			if (one_sensor > 0 && !asked_binned && access(rw_path("redwood-rear-off", (char[160]){0}, 160), F_OK) != 0) {
				bool rear_frames = s20_sensor_is_rear(head.sensor), want_rear = s->camera->sensor == 1;
                if(aux_enabled&&want_rear&&(head.sensor!=(rear_mode==6?4U:rear_mode==5?3U:1U)))continue;

				if (rear_frames != want_rear) {
					struct redwood_camera_command rc = { .command = REDWOOD_CAMERA_SENSOR, .a = want_rear ? 1 : 2,
						.b = want_rear ? 2 : 0 };

					/*
					 * Asked ONCE, and then the other sensor's frames are passed over until this one's come: the
					 * switch takes the kernel's next session, and the header right after the request was still
					 * the old sensor's. Taken as this camera's, it sized the buffers for 3648x2736, every rear
					 * frame (2016x1512) was then a short read, and five of those ended the stream with no picture
					 * ("failed to switch", 2026-11-12).
					 */
					if (!asked_sensor) {
						asked_sensor = true;
						say("the kernel's frames are the %s's: the %s's asked for", rear_frames ? "rear main" : "front sensor",
						    want_rear ? "rear main" : "front sensor");
						(void)write(device, &rc, sizeof(rc));
					} else if (++skipped_other % 30 == 1) {
						say("still the %s's frames (%u passed over): waiting for the %s", rear_frames ? "rear main" : "front sensor",
						    skipped_other, want_rear ? "rear main" : "front sensor");
					}
					continue;
				}
			}
			if (head.sensor != s->camera->sensor && !one_sensor) {
				/* another camera's frames (the kernel's last sensor): changed, and the next ones are this one's */
				if (!switched) {
					struct redwood_camera_command c = { .command = REDWOOD_CAMERA_SENSOR, .a = s->camera->sensor, .b = ~0u };

					switched = true;
					say("the kernel's frames are camera %u's: to %u, the %s", head.sensor, s->camera->sensor, s->camera->name);
					(void)write(device, &c, sizeof(c));
				}
				continue;
			}
			/*
			 * The S20: the binned 1824x1368 mode for the stream (a quarter of the full mode's 12 MB a frame: with
			 * every frame demosaiced on the CPU the phone sat at 95 C, throttled, the preview at 17 frames a
			 * second; 2026-11-12). redwood-full-stream in the tuning directory keeps the full mode.
			 */
			/* (the user, 2026-11-12: binned looked much worse and stuttered: the full mode, unless redwood-binned-stream) */
			bool want_binned = access(rw_path("redwood-binned-stream", (char[160]){0}, 160), F_OK) == 0;
			if (s->camera->sensor!=1 && one_sensor > 0 && !asked_binned && (head.width > 2000) == want_binned) {
				struct redwood_camera_command mc = { .command = REDWOOD_CAMERA_SENSOR, .a = head.sensor, .b = want_binned ? 1 : 0 };

				asked_binned = true;
				say("the %s mode asked for (the frames were %ux%u)", want_binned ? "binned" : "full", head.width, head.height);
				(void)write(device, &mc, sizeof(mc));
				continue;
			}
			bytes = sizeof(head) + (size_t)head.stride * head.height;
			buffers[0] = malloc(bytes);
			buffers[1] = malloc(bytes);
			buffer = buffers[0];
			if (h != NULL && buffers[1] != NULL && !drawer_up) {
				pthread_mutex_init(&h->lock, NULL);
				pthread_cond_init(&h->cond, NULL);
				h->s = s;
				drawer_up = pthread_create(&drawer, NULL, render_body, h) == 0;
			}
			say("frames of %ux%u from the kernel (the %s)", head.width, head.height, s->camera->name);
			continue;
		}
		uint64_t t_read = mach_absolute_time();
		n = read(device, buffer, bytes);
		s->read_ticks += mach_absolute_time() - t_read;
		if (n != (ssize_t)bytes) {
			if (n < 0 && errno == EIO) {
				say("the kernel's camera has had a fault: no more frames");
				break;
			}
			if (n > (ssize_t)sizeof(head)) {
				/* a whole frame of another size (the sensor changed under us): not a failure; the buffers go and
				 * the next header says the size (as for the frames of another size below) */
				const struct redwood_camera_frame *fh = (const void *)buffer;

				say("a frame of %zd bytes (%ux%u) where %zu were expected: the buffers are made again", n,
				    fh->width, fh->height, bytes);
				if (drawer_up) {
					render_wait(h);
				}
				free(buffers[0]);
				free(buffers[1]);
				buffers[0] = buffers[1] = NULL;
				buffer = NULL;
				next_buffer = 0;
				continue;
			}
			failures += n < 0 && errno == EAGAIN ? 0 : 1;
			continue;
		}
		failures = 0;
		{
			/*
			 * A frame of another size (the kernel went from the rear main to the front or back): its picture did not
			 * fit this buffer, and drawing it by its own header read past the end -- mediaserverd SIGSEGV (2026-11-12).
			 * The buffers go, and the next header says how big they must be.
			 */
			const struct redwood_camera_frame *fh = (const void *)buffer;

			if (sizeof(head) + (size_t)fh->stride * fh->height != bytes || fh->header_bytes > sizeof(head) + 64) {
				say("frames of %ux%u now, the buffers were for another size: made again", fh->width, fh->height);
				if (drawer_up) {
					render_wait(h);
				}
				free(buffers[0 == next_buffer ? 1 : 0]);
				free(buffers[next_buffer]);
				buffers[0] = buffers[1] = NULL;
				buffer = NULL;
				next_buffer = 0;
				continue;
			}
		}

		/*
		 * The rear main's black level is not the front's 64: its darkest values were 10-30 (2026-11-12, a raw frame),
		 * and taking 64 off cut the shadows away channel by channel -- the colours went wrong. redwood-rear-black (the
		 * tuning directory) says it; 16 until it is measured with the lens covered.
		 */
		{
			struct redwood_camera_frame *fw = (void *)buffer;
			int black = 64;

			if (fw->sensor!=3 && (fw->width == 2016 || fw->width == 4032)) {
				static int rear_black = -1;
				static uint64_t looked;

				if (rear_black < 0 || ++looked % 150 == 0) {
					FILE *bf = fopen(rw_path("redwood-rear-black", (char[160]){0}, 160), "r");
					int v = 64;

					if (bf != NULL) {
						if (fscanf(bf, "%d", &v) != 1) v = 64;
						fclose(bf);
					}
					rear_black = v;
				}
				black = rear_black;
			}
			fw->black = (uint32_t)black;
			for (int k = 0; k < 4; k++) {
				isp.black.site[k] = black;
			}
			if (fw->sensor!=3 && fw->width == 2016 && fw->gain_unity > 0) {
				/*
				 * The S5K2LD's black level per site against the analog gain, measured on dark frames (2 lines of
				 * exposure, cam.s20.dma.test 2 N, 2026-11-12): Gr sinks from 63.5 at 1x to 30.4 at 16x while R, B and
				 * Gb stay near 64 -- the shadows went purple and the picture green when one level was taken off all.
				 */
				static const float gains[4] = { 1, 4, 8, 16 };
				/*
				 * Measured face-down in a dark room (2026-11-12, cam.s20.dma.test 2 g*100000+lines): the S5K2LD's black
				 * depends on the gain AND on the exposure -- an exposure inside the frame (8000-20000 lines) sits far
				 * lower than a very short one or one that stretches the frame (2 or 33000 lines). Sites Gr, R, B, Gb.
				 * (16x inside the frame from a fingertip-covered run.)
				 */
				static const float inside[4][4] = { { 57.9f, 40.3f, 28.1f, 18.1f }, { 59.4f, 44.7f, 39.0f, 30.0f },
					{ 56.0f, 39.0f, 30.4f, 25.0f }, { 59.2f, 44.1f, 36.8f, 30.0f } };
				static const float outside[4][4] = { { 63.5f, 56.2f, 48.2f, 29.6f }, { 62.6f, 61.4f, 61.7f, 64.0f },
					{ 65.2f, 64.1f, 65.0f, 69.7f }, { 64.0f, 62.9f, 62.5f, 63.5f } };
				bool stretched = fw->frame_lines_min != 0 && fw->exposure_lines + fw->margin > fw->frame_lines_min;
				const float (*level)[4] = fw->exposure_lines < 64 || stretched ? outside : inside;
				float g = (float)fw->gain / fw->gain_unity;
				int seg = g <= 4 ? 0 : g <= 8 ? 1 : 2;
				float t = (g - gains[seg]) / (gains[seg + 1] - gains[seg]);

				t = t < 0 ? 0 : t > 1 ? 1 : t;
				for (int k = 0; k < 4; k++) {
					isp.black.site[k] = (int)(level[k][seg] + t * (level[k][seg + 1] - level[k][seg]) + 0.5f);
				}
			}
		}
		{
			/*
			 * The rear main's full 4032x3024 mode starts its readout on another colour of the filter than the
			 * binned mode the kernel's descriptor (GRBG) was right for: the picture went green with a visible
			 * pixel grid, white balance red 0.76 (2026-11-12). The order for those frames: redwood-rear-bayer in
			 * the tuning directory (camraw.py's codes: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR), else REAR_FULL_BAYER.
			 */
			struct redwood_camera_frame *fb = (void *)buffer;

			if (fb->sensor!=3 && fb->width == 4032) {
				static int rear_bayer = -1;
				static uint64_t looked_b;

				if (rear_bayer < 0 || ++looked_b % 60 == 0) {
					FILE *bf = fopen(rw_path("redwood-rear-bayer", (char[160]){0}, 160), "r");
					int v = 9;      /* (none: the frame's own order) */

					if (bf != NULL) {
						if (fscanf(bf, "%d", &v) != 1 || v < 0 || v > 3) v = 9;
						fclose(bf);
					}
					rear_bayer = v;
				}
				if (rear_bayer >= 0 && rear_bayer <= 3) fb->bayer = (uint8_t)rear_bayer;   /* (only when the file says so) */
			}
		}
		{
			struct redwood_camera_frame *fb = (void *)buffer;

			if (fb->width == 2016 || fb->width == 4032)
				/* redwood-raw-dump in the tuning directory: this frame as it came (header + RAW10), once, as redwood-raw.bin */
				{
					char flag[160];

					if (access(rw_path("redwood-raw-dump", flag, sizeof(flag)), F_OK) == 0) {
						FILE *rf = fopen(rw_path("redwood-raw.bin", (char[160]){0}, 160), "w");

						unlink(flag);
						if (rf != NULL) {
							fwrite(buffer, 1, bytes, rf);
							fclose(rf);
							say("raw frame written (%zu bytes, %ux%u, stride %u, header %u)", bytes, fb->width, fb->height,
							    fb->stride, fb->header_bytes);
						}
					}
				}
		}
		{
			/*
			 * The rear's 12 MP frames: two blacks, above and below the band line (row ~1914, measured 2026-11-12: below it
			 * the black is lower, a hard line in every dark picture). Both by the analog gain, from redwood-rear-dark in
			 * the tuning directory: lines "gain  Gr R B Gb (above)  Gr R B Gb (below)", in rising gain (the dark
			 * calibration writes it); without it 64 everywhere. redwood-rear-band: the line's row.
			 */
			struct redwood_camera_frame *fd = (void *)buffer;

			isp.black.band_row = 0;
			if (fd->sensor!=3 && fd->width == 4032 && fd->gain_unity > 0) {
				static float dark[12][9];
				static int dark_n = -1, band = 1914;
				static uint64_t dark_looked;

				if (dark_n < 0 || ++dark_looked % 150 == 0) {
					FILE *df = fopen(rw_path("redwood-rear-dark", (char[160]){0}, 160), "r");
					FILE *bf = fopen(rw_path("redwood-rear-band", (char[160]){0}, 160), "r");

					dark_n = 0;
					while (df != NULL && dark_n < 12 && fscanf(df, "%f %f %f %f %f %f %f %f %f", &dark[dark_n][0],
					    &dark[dark_n][1], &dark[dark_n][2], &dark[dark_n][3], &dark[dark_n][4], &dark[dark_n][5],
					    &dark[dark_n][6], &dark[dark_n][7], &dark[dark_n][8]) == 9) {
						dark_n++;
					}
					if (df != NULL) fclose(df);
					if (bf != NULL) {
						if (fscanf(bf, "%d", &band) != 1) band = 1914;
						fclose(bf);
					}
					band &= ~1;
				}
				if (dark_n == 0 && access(rw_path("redwood-rear-fixed-black", (char[160]){0}, 160), F_OK) != 0) {
					/*
					 * No table: the black measured in this very frame. The S5K2LD's black in its 12 MP mode is not 64:
					 * it sinks with gain (37-59 at 4-8x, measured 2026-11-12), so taking 64 off took the picture away --
					 * "green 2 above black", the gain at 8x, white balance at its limits: night vision. Per site, above
					 * and below the band line, the 0.3rd percentile of a sparse sample (every 8th row pair and column
					 * pair), never above 64: in a bright scene the darkest pixels are above black and 64 stays.
					 */
					static uint32_t hist[2][4][1024];
					const uint8_t *raw0 = buffer + fd->header_bytes;

					memset(hist, 0, sizeof(hist));
					for (uint32_t y = 0; y + 1 < fd->height; y += 16) {
						for (int dy = 0; dy < 2; dy++) {
							const uint8_t *line = raw0 + (size_t)(y + dy) * fd->stride;
							int bnd = (int)(y + dy) >= band ? 1 : 0;

							for (uint32_t x = 0; x + 4 <= fd->width; x += 32) {
								const uint8_t *g = line + (x >> 2) * 5;

								for (int i = 0; i < 2; i++) {
									int v = g[i] << 2 | (g[4] >> (i * 2) & 3);

									hist[bnd][dy * 2 + i][v]++;
								}
							}
						}
					}
					for (int bnd = 0; bnd < 2; bnd++) {
						for (int k = 0; k < 4; k++) {
							uint32_t total = 0, acc = 0;
							int v = 0;

							for (int i = 0; i < 1024; i++) total += hist[bnd][k][i];
							for (v = 0; v < 1023 && acc + hist[bnd][k][v] < total * 3 / 1000 + 1; v++) acc += hist[bnd][k][v];
							v = v > 64 ? 64 : v;
							if (bnd == 0) isp.black.site[k] = v; else isp.black.low[k] = v;
						}
					}
					isp.black.band_row = band;
					if (++dark_looked % 60 == 1) {
						say("rear black measured: above %d %d %d %d, below %d %d %d %d", isp.black.site[0], isp.black.site[1],
						    isp.black.site[2], isp.black.site[3], isp.black.low[0], isp.black.low[1], isp.black.low[2],
						    isp.black.low[3]);
					}
				}
				if (dark_n > 0) {
					float g = (float)fd->gain / fd->gain_unity;
					int a = 0;

					while (a + 1 < dark_n && dark[a + 1][0] <= g) a++;
					int b = a + 1 < dark_n ? a + 1 : a;
					float t = b == a || dark[b][0] <= dark[a][0] ? 0 : (g - dark[a][0]) / (dark[b][0] - dark[a][0]);

					t = t < 0 ? 0 : t > 1 ? 1 : t;
					for (int k = 0; k < 4; k++) {
						isp.black.site[k] = (int)(dark[a][1 + k] + t * (dark[b][1 + k] - dark[a][1 + k]) + 0.5f);
						isp.black.low[k] = (int)(dark[a][5 + k] + t * (dark[b][5 + k] - dark[a][5 + k]) + 0.5f);
					}
					isp.black.band_row = band;
				}
			}
		}
		const struct redwood_camera_frame *f = (const void *)buffer;
		const uint8_t *raw = buffer + f->header_bytes;
		bool spatial_black = isp_apply_black_calibration(&isp, &rear_dark, f);
		if (spatial_black && s->frames % 60 == 0) say("rear dark-grid active: gain %u, exposure %u", f->gain, f->exposure_lines);
		struct redwood_camera_command c;

		uint64_t t_stats = mach_absolute_time();
		/*
		 * The rear: stats (exposure, white balance) every other frame. At 95 C, throttled, they took 22-33 ms a frame
		 * and the rear fell to 15 frames a second (2026-11-12); AE and AWB settle in a quarter second either way.
		 * The front keeps every frame.
		 */
		bool stats_frame = !(f->sensor==4 || f->width == 2016 || f->width == 4032) || (s->frames & 1) == 0;

		if (stats_frame) {
			isp_measure(&isp, f, raw, &st);
			isp_awb(&isp, &st);
		}
		memcpy(last_wb, isp.wb, sizeof(last_wb));
		if (s->frames % 15 == 0) {
			tune_on_phone(&isp, f, raw);
		}
		if (f->sensor != tuned_for) {
			tuned_for = f->sensor;
			load_tuning(&isp, f->sensor);
		}
		/*
		 * Slowly, a second or so: the colour temperature is guessed from the white balance, the tuning it
		 * picks changes what the white balance sees, and the colour matrix makes every swing of it plain.
		 */
		lux = lux < 0 ? lux_index(&isp, f) : lux * 0.9f + lux_index(&isp, f) * 0.1f;
		cct = cct < 0 ? colour_temperature(&isp) : cct * 0.97f + colour_temperature(&isp) * 0.03f;
		isp_tune(&isp, lux, cct);
		isp.zoom = s20_zoom_crop_sensor(requested_zoom,f->sensor);
		isp.turned = s->camera->turned;
		{
			/* the kernel's say (kern.redwood.camera_turn, the S20): bit 0 turns the front camera's pictures half round */
			int turn = 0;
			size_t len = sizeof(turn);
			isp.mirror = false;
			/* bit 2 set: the kernel says, for every camera (the S20's frames all come from one sensor so far):
			 * bit 0 turn half round, bit 1 mirror. The raw frame (2026-11-12, phone upright) has the scene's top at
			 * the sensor's right; iOS turns portrait pictures 90 degrees clockwise, so they need the half turn: 5. */
			/* /var/root/redwood-camera-turn, when there, overrides the kernel's (5 since 2026-11-12: the preview a
			 * mirror, photographs and videos as others see you -- iOS turns those itself; the user checked all three) */
			bool from_file = false;
			/* the rear main (its 2016x1512 frames) is mounted the other way round from the front: its own setting,
			 * redwood-rear-turn, 4 (no turn) unless it says otherwise (2026-11-12, user: upside down with the front's) */
			bool rear_frames = f->sensor==4 || f->width == 2016 || f->width == 4032;   /* (4032x3024: the full mode, 2026-11-12) */
			FILE *tf = fopen(rw_path(f->sensor==4?"redwood-gw2-turn":rear_frames ? "redwood-rear-turn" : "redwood-camera-turn", (char[160]){0}, 160), "r");

			if (tf == NULL && rear_frames) {
				turn = 4;
				from_file = true;
			}

			if (tf != NULL) {
				from_file = fscanf(tf, "%d", &turn) == 1;
				fclose(tf);
			}
			if ((from_file || sysctlbyname("kern.redwood.camera_turn", &turn, &len, NULL, 0) == 0) && (turn & 4)) {
				isp.turned = (turn & 1) != 0;
				isp.mirror = (turn & 2) != 0;
				if (turn != s->last_turn) {
					say("orientation: turn %d (turned %d, mirrored %d) for the %s", turn, isp.turned, isp.mirror, s->camera->name);
					s->last_turn = turn;
				}
			}
		}
		{
			/*
			 * Where Camera.app was tapped (the focus window) counts most for the exposure, as on an iPhone; the
			 * sun slider (ExposureBias) moves the target. Both through the green mean that AE goes by.
			 */
			bool tapped = !(s->af_window[0] == 0.3f && s->af_window[1] == 0.3f && s->af_window[2] == 0.7f &&
			    s->af_window[3] == 0.7f);
			uint32_t cw = f->width / 2, chh = f->height / 2, n = 0;
			double sum = 0;

			if (tapped && (f->width == 2016 || f->width == 4032)) {
				double green;
				uint32_t cells = isp_window_green(&isp, f, raw, s->af_window[0], s->af_window[1],
				    s->af_window[2], s->af_window[3], &green);

				if (cells > 16) st.mean[1] = 0.75 * green + 0.25 * st.mean[1];
			} else if (tapped) {
				uint32_t x0 = (uint32_t)(s->af_window[0] * cw), x1 = (uint32_t)(s->af_window[2] * cw);
				uint32_t y0 = (uint32_t)(s->af_window[1] * chh), y1 = (uint32_t)(s->af_window[3] * chh);
				int r_at = (int)(f->bayer & 3), b_at = 3 - (int)(f->bayer & 3);   /* (as camera_isp.c's cell_places) */
				for (uint32_t cy = y0; cy < y1 && cy < chh; cy += 6) {
					for (uint32_t cx = x0; cx < x1 && cx < cw; cx += 6) {
						const uint8_t *g0 = raw + (size_t)cy * 2 * f->stride + (size_t)(cx >> 1) * 5;
						unsigned o = (cx & 1) * 2;
						int v[4] = { (g0[o] << 2) - 64, (g0[o + 1] << 2) - 64, (g0[f->stride + o] << 2) - 64,
							(g0[f->stride + o + 1] << 2) - 64 };

						for (int j = 0; j < 4; j++) {
							sum += j != r_at && j != b_at ? (v[j] > 0 ? v[j] : 0) / 2.0 : 0;
						}
						n++;
					}
				}
				if (n > 16) {
					st.mean[1] = 0.75 * (sum / n) + 0.25 * st.mean[1];
				}
			}
			/* the sun slider: on the picture at once (Camera.app's own feel), not through AE (a second to follow) */
			isp.bias = exp2f(s->ev_bias);
		}
		/*
		 * The longest frame auto-exposure may stretch to: what the app's minimum frame rate allows (Camera.app's Video
		 * asks for 30: a dim room is then more gain at 30 a second, not 20 a second of 50 ms exposures -- the rear's
		 * video lagged that way, 2026-11-12), and never more than twice the mode's frame as before.
		 */
		if (f->frame_lines_min > 0 && f->line_ns > 0) {
			double mode_fps = 1e9 / ((double)f->frame_lines_min * f->line_ns);
			/* (Camera.app asks 15 in every mode: held at the mode's rate, as Samsung's camera does, unless an app asks 10 or less) */
			double floor_fps = s->min_fps > 1 && s->min_fps <= 10 ? s->min_fps : mode_fps;
			double k = mode_fps / floor_fps;

			k = k < 1 ? 1 : k > 2 ? 2 : k;
			isp.max_frame_lines = (uint32_t)(f->frame_lines_min * k);
		}
		/* redwood-fixed-exposure in the tuning directory: "lines gain" (gain in the sensor's code, 32 = 1x) held, no AE --
		 * for the dark calibration */
		{
			static uint32_t fx_lines, fx_gain;
			static uint64_t fx_looked;
			static bool fx_on;

			if (++fx_looked % 15 == 1) {
				FILE *xf = fopen(rw_path("redwood-fixed-exposure", (char[160]){0}, 160), "r");
				uint32_t l = 0, g = 0;

				fx_on = xf != NULL && fscanf(xf, "%u %u", &l, &g) == 2 && l > 0 && g > 0;
				if (xf != NULL) fclose(xf);
				if (fx_on && (l != fx_lines || g != fx_gain)) {
					struct redwood_camera_command xc = { .command = REDWOOD_CAMERA_EXPOSURE, .a = l, .b = g, .c = 0 };

					fx_lines = l;
					fx_gain = g;
					(void)write(device, &xc, sizeof(xc));
					say("fixed exposure: %u lines, gain %u", l, g);
				}
				if (!fx_on) fx_lines = fx_gain = 0;
			}
			if (fx_on) goto ae_done;
		}
		if (stats_frame && !s->ae_locked && isp_ae(&isp, f, &st, &c)) {
			(void)write(device, &c, sizeof(c));
		}
ae_done:;
		if (s->af_now) {
			s->af_now = false;
			/* (whatever it was doing: at once, from the next frame) */
			af.state = AF_STEADY;
			af.now = true;
			af.sweep = true;
		}
		af_frame(device, &af, &isp, f, raw, s->af_window, s->af_locked);
		s->stats_ticks += mach_absolute_time() - t_stats;
		s->frames++;

		CFDictionaryRef metadata = frame_metadata(&isp, f, &st, s->port);

		/*
		 * Only once the graph has started the stream. Its PixelBufferHandler blocks hand the buffer
		 * to multiStreamCameraSourceNode_sendBufferToBufferServicingThread, which reaches into the
		 * source node's innards: called before the graph is started (or after it has been torn down)
		 * it walks a pointer that is not there yet and mediaserverd dies of SIGSEGV. The frames
		 * before that are read and thrown away, which is also what settles the exposure.
		 */
		if (metadata != NULL) {
			pthread_mutex_lock(&s->metadata_lock);
			if (s->metadata != NULL) {
				CFRelease(s->metadata);
			}
			s->metadata = CFRetain(metadata);
			pthread_mutex_unlock(&s->metadata_lock);
		}
		if (drawer_up) {
			/* to the drawing thread; this one reads the next frame into the other buffer meanwhile */
			render_wait(h);
			pthread_mutex_lock(&h->lock);
			h->isp = isp;
			h->buffer = buffer;
			h->metadata = metadata;         /* (released by the drawing thread) */
			h->has_job = true;
			pthread_cond_broadcast(&h->cond);
			pthread_mutex_unlock(&h->lock);
			next_buffer ^= 1;
			buffer = buffers[next_buffer];
		} else {
			give_frame(s, &isp, f, raw, metadata);
			if (metadata != NULL) {
				CFRelease(metadata);
			}
		}
		if (s->graph_running && s->frames % 30 == 0) {
			/* every second the graph has the frames: what each output took (given / no buffer in its pool) */
			char line[400];
			size_t at = 0;

			for (int i = 0; i < OUTPUTS && at < sizeof(line) - 80; i++) {
				struct output *out = &s->out[i];

				if (out->handler != NULL) {
					at += (size_t)snprintf(line + at, sizeof(line) - at, " %s %llu/%llu%s", out->name,
					    (unsigned long long)out->delivered, (unsigned long long)out->refused, out->enabled ? "" : " (off)");
				}
			}
			line[at] = 0;
			{
				static uint64_t t_last;
				uint64_t t_now = mach_absolute_time();
				double fps = t_last ? 30.0 * 24000000.0 / (double)(t_now - t_last) : 0;

				t_last = t_now;
				say("frame %llu (format %d): %.1f fps; a frame: read %.1f ms, stats %.1f, render %.1f; given/refused:%s",
				    s->frames, s->format_index, fps, s->read_ticks / 24000.0 / 30, s->stats_ticks / 24000.0 / 30,
				    s->render_ticks / 24000.0 / 30, line);
				s->render_ticks = s->read_ticks = s->stats_ticks = 0;
			}
		}
		if (s->frames == 1 || s->frames % 30 == 0) {
			say("frame %llu: exposure %.1f ms, gain %.1fx, digital %.2fx, green %.0f above black; white balance R %.2f "
			    "B %.2f, lux index %.0f, about %.0f K", f->sequence, f->exposure_lines * (double)f->line_ns / 1e6,
			    (double)f->gain / f->gain_unity, isp.digital, st.mean[1], isp.wb[0], isp.wb[2], lux, cct);
			if (s->ev_bias != 0 || s->ae_locked) {
				say("exposure bias %+.2f stops%s", s->ev_bias, s->ae_locked ? ", AE locked" : "");
			}
		}
	}
	if (drawer_up) {
		render_wait(h);
		pthread_mutex_lock(&h->lock);
		h->quit = true;
		pthread_cond_broadcast(&h->cond);
		pthread_mutex_unlock(&h->lock);
		pthread_join(drawer, NULL);
	}
	free(h);
	free(buffers[0]);
	free(buffers[1]);
	close(device);
	say("the frames stopped (%llu)", s->frames);
	return NULL;
}

/*
 * Reading frames from the kernel and giving them to the graph are two things: the first begins when
 * the capture graph reserves the stream (`graph` false, from the device's RequestStreams: the sensor
 * takes a moment to settle and the exposure longer), the second only when the graph starts the
 * stream itself (`graph` true, the FigCaptureStream start method).
 */
static CMBaseObjectRef all_streams[2];  /* the device's, back and front */
static OSStatus stream_stop(CMBaseObjectRef o);

static OSStatus
stream_begin(CMBaseObjectRef o, bool graph)
{
	struct stream *s = stream_of(o);

	if (graph) {
		pthread_mutex_lock(&s->lock);
		s->graph_running = true;
		pthread_mutex_unlock(&s->lock);
		say("stream: the graph starts the stream (format %d)", s->format_index);
	}
	if (s->running) {
		return 0;
	}
	/* one camera at a time: the kernel gives one sensor's frames (the other stream's worker stopped first) */
	for (int i = 0; i < 2; i++) {
		if (all_streams[i] != NULL && all_streams[i] != o && stream_of(all_streams[i])->running) {
			say("stream: the %s's frames stop for the %s's", stream_of(all_streams[i])->camera->name, s->camera->name);
			(void)stream_stop(all_streams[i]);
		}
	}
	/* a worker that a stop did not wait for: let it end first (it still has the kernel's camera open) */
	for (int i = 0; i < 2; i++) {
		for (int w = 0; w < 200 && all_streams[i] != NULL && stream_of(all_streams[i])->worker_busy; w++) {
			usleep(10000);
		}
	}
	s->running = true;
	s->frames = 0;
	s->worker_busy = true;
	say("stream: frames begin (format %d)", s->format_index);
	if (pthread_create(&s->worker, NULL, worker, o) != 0) {
		s->running = false;
		s->worker_busy = false;
		return ERR_UNSUPPORTED;
	}
	s->worker_started = true;
	return 0;
}

/*
 * The stream's own notifications, as H10ISP's stream posts them (CoreMedia's notification center, the stream as the
 * object): "StreamStarting"/"StreamStarted" and "StreamStopping"/"StreamStopped". BWFigVideoCaptureDevice waits
 * for StreamStopped (captureStreamWillStopStreaming:, _deviceDidStopStreaming): without it every stop ended in
 * "Timed out waiting for the capture graph to stop" -- mediaserverd's SIGTRAP and a movie never finished
 * (2026-11-12).
 */
static void
post(CMBaseObjectRef o, CFStringRef name)
{
	static void *(*center)(void);
	static OSStatus (*post_notification)(void *, CFStringRef, const void *, CFTypeRef, uint32_t);
	static bool looked;

	if (!looked) {
		looked = true;
		center = dlsym(RTLD_DEFAULT, "CMNotificationCenterGetDefaultLocalCenter");
		post_notification = dlsym(RTLD_DEFAULT, "CMNotificationCenterPostNotification");
	}
	if (center != NULL && post_notification != NULL) {
		(void)post_notification(center(), name, o, NULL, 0);
	}
}

static OSStatus
stream_start(CMBaseObjectRef o)
{
	OSStatus st;

	post(o, CFSTR("StreamStarting"));
	st = stream_begin(o, true);
	if (st == 0) {
		post(o, CFSTR("StreamStarted"));
	}
	return st;
}

static OSStatus
stream_stop(CMBaseObjectRef o)
{
	struct stream *s = stream_of(o);

	say("stream: stop (%llu frames, %llu pictures given)", (unsigned long long)s->frames,
	    (unsigned long long)s->out[0].delivered);
	post(o, CFSTR("StreamStopping"));
	if (s->flash_ready != 0) {
		s->flash_ready = 0;
		flash_light(s, false);
	}
	/* The flags first, without the lock: the worker holds it while it gives a frame to the graph, whose handler
	 * may wait for the graph that is stopping now -- waiting for the lock here was "Timed out waiting for the
	 * capture graph to stop" and mediaserverd's SIGTRAP (2026-11-12). */
	s->graph_running = false;
	s->running = false;
	pthread_mutex_lock(&s->lock);
	s->graph_running = false;
	for (int i = 0; i < OUTPUTS; i++) {
		let_go(&s->out[i]);
	}
	pthread_mutex_unlock(&s->lock);
	s->running = false;
	if (s->worker_started) {
		/*
		 * At most 300 ms for the worker: it can be inside the graph's frame handler, and that can wait for the
		 * graph -- which is waiting for this Stop. Joined when it is done in time, else left to end by itself
		 * (it sees `running' false); the next start waits for it. (The wait for ever was mediaserverd's "Timed
		 * out waiting for the capture graph to stop", 2026-11-12.)
		 */
		uint64_t t0 = mach_absolute_time();
		int w;

		for (w = 0; w < 30 && s->worker_busy; w++) {
			usleep(10000);
		}
		if (!s->worker_busy) {
			pthread_join(s->worker, NULL);
		} else {
			pthread_detach(s->worker);
			say("stream: the worker is still in a frame after 300 ms: left to finish by itself");
		}
		s->worker_started = false;
		say("stream: stopped in %.0f ms", (double)(mach_absolute_time() - t0) / 24000.0);
	}
	post(o, CFSTR("StreamStopped"));
	return 0;
}

static void
add_absent(const void *key, const void *value, void *context)
{
	if (!CFDictionaryContainsKey(context, key)) {
		CFDictionarySetValue(context, key, value);
	}
}

/*
 * The frame's metadata into the sample buffer the graph made of our pixel buffer: the pixel buffer's
 * own dictionary (deliver()), the same object, else the last frame's; whatever the graph has put in a
 * dictionary of the sample buffer's already stays as it is.
 */
static OSStatus
stream_add_attachments(CMBaseObjectRef o, CMSampleBufferRef sbuf, CFDictionaryRef options)
{
	struct stream *s = stream_of(o);
	static bool said;
	CVImageBufferRef image = sbuf != NULL ? CMSampleBufferGetImageBuffer(sbuf) : NULL;
	CFTypeRef ours = image != NULL && md_attachment != NULL ? CVBufferGetAttachment(image, md_attachment, NULL) : NULL, theirs;
	CFMutableDictionaryRef m;

	if (!said) {
		said = true;
		say("stream: AddAttachmentsToSampleBuffer (options %s): the frames' metadata %s", options ? "given" : "none",
		    md_attachment != NULL ? "attached" : "NOT THERE");
	}
	if (md_attachment == NULL || sbuf == NULL) {
		return 0;
	}
	if (ours != NULL && CFGetTypeID(ours) == CFDictionaryGetTypeID()) {
		CFRetain(ours);
	} else {
		pthread_mutex_lock(&s->metadata_lock);
		ours = s->metadata != NULL ? CFDictionaryCreateMutableCopy(NULL, 0, s->metadata) : NULL;
		pthread_mutex_unlock(&s->metadata_lock);
	}
	if (ours == NULL) {
		return 0;
	}
	theirs = CMGetAttachment(sbuf, md_attachment, NULL);
	if (theirs == ours) {
		CFRelease(ours);
		return 0;
	}
	if (theirs != NULL && CFGetTypeID(theirs) == CFDictionaryGetTypeID()) {
		m = CFDictionaryCreateMutableCopy(NULL, 0, theirs);
		CFDictionaryApplyFunction(ours, add_absent, m);
	} else {
		m = (CFMutableDictionaryRef)CFRetain(ours);
	}
	CMSetAttachment(sbuf, md_attachment, m, kCMAttachmentMode_ShouldPropagate);
	CFRelease(m);
	CFRelease(ours);
	return 0;
}

static OSStatus
stream_invalidate(CMBaseObjectRef o)
{
	(void)stream_stop(o);
	return 0;
}

static void
stream_finalize(CMBaseObjectRef o)
{
	struct stream *s = stream_of(o);

	(void)stream_stop(o);
	for (int i = 0; i < OUTPUTS; i++) {
		if (s->out[i].config) {
			CFRelease(s->out[i].config);
		}
		if (s->out[i].handler) {
			_Block_release(s->out[i].handler);
		}
	}
	if (s->formats) {
		CFRelease(s->formats);
	}
	if (s->supported) {
		CFRelease(s->supported);
	}
	if (s->colour_info) {
		CFRelease(s->colour_info);
	}
	if (s->metadata) {
		CFRelease(s->metadata);
	}
	if (s->torch >= 0) {
		close(s->torch);
	}
}

static CFStringRef
stream_description(CMBaseObjectRef o)
{
	(void)o;
	return CFRetain(CFSTR("<RedwoodCaptureStream: the wide camera>"));
}

static const struct base_class stream_base = {
	1, sizeof(struct stream), NULL, stream_invalidate, stream_finalize, stream_description, stream_copy_property, stream_set_property,
	NULL, NULL, NULL
};
static const struct stream_class stream_derived = { 2, stream_start, stream_stop, stream_add_attachments };
static const struct vtable stream_vtable = { NULL, &stream_base, &stream_derived };

static CMBaseObjectRef
stream_create(CFAllocatorRef allocator, const struct camera *camera, CFStringRef port)
{
	CMBaseObjectRef o = NULL;
	struct stream *s;

	if (derived_create(allocator, &stream_vtable, stream_class_id(), &o) != 0 || o == NULL) {
		say("the stream cannot be made");
		return NULL;
	}
	s = stream_of(o);
	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	pthread_mutex_init(&s->metadata_lock, NULL);
	s->torch = -1;
	s->min_fps = 2;
	s->zoom = 1;
	s->af_window[0] = s->af_window[1] = 0.3f;
	s->af_window[2] = s->af_window[3] = 0.7f;
	s->max_fps = 30;
	s->camera = camera;
	s->port = port;
	for (int i = 0; i < OUTPUTS; i++) {
		s->out[i].name = output_names[i];
	}

	/*
	 * The video format first (2026-11-12): with the photo format first, Camera.app's video mode stayed on it and
	 * recorded its 4:3 1504x1128 scaler output, not 1080p. redwood-photo-first (in the tuning directory) puts it
	 * back. (format_index is only said, never used: the ISP draws whatever the graph's buffers are.)
	 */
	int first = access(rw_path("redwood-photo-first", (char[160]){0}, 160), F_OK) == 0 ? 0 : 1;
	CFDictionaryRef f[2] = { format(camera->formats[first][0], camera->formats[first][1], camera->formats[first][2], false, camera->fov),
		format(camera->formats[!first][0], camera->formats[!first][1], camera->formats[!first][2], false, camera->fov) };

	s->formats = CFArrayCreate(NULL, (const void **)f, 2, &kCFTypeArrayCallBacks);
	s->supported = supported(stream_props, sizeof(stream_props) / sizeof(stream_props[0]));
	CFRelease(f[0]);
	CFRelease(f[1]);
	return o;
}

/* ---- the device ------------------------------------------------------------------------------------- */

struct device {
	CFArrayRef      streams;
	CFDictionaryRef supported;
};

static OSStatus
device_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	struct device *d = derived_storage(o);
	CFTypeRef value = NULL;

	(void)allocator;
	if (key_is(key, "StreamArray")) {
		value = CFRetain(d->streams);
	} else if (key_is(key, "Clock")) {
		value = CFRetain(CMClockGetHostTimeClock());
	} else if (key_is(key, "SupportedPropertiesDictionary")) {
		value = CFRetain(d->supported);
	} else if (key_is(key, "SupportedSynchronizedStreamsGroups")) {
		value = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	} else {
		say_once("device: no answer for", key, NULL);
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static OSStatus
device_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	(void)o;
	say_once("device: set (not used)", key, value);
	return 0;
}

static unsigned requests, relinquishes;

static OSStatus
device_request_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
	(void)o;
	if (requests++ < 3) {
		say("device: streams asked for (%ld)%s", streams ? (long)CFArrayGetCount(streams) : 0L, options ? ", with options" : "");
		if (options != NULL) {
			say_once("device: the request's options", CFSTR("RequestStreams"), options);
		}
	}
	/*
	 * Requesting a stream is this device's cue to begin producing: mediaserverd's capture graph
	 * configures the stream (it installs the PrimaryScaler PixelBufferHandler) and then reserves the
	 * streams through here, but it never calls the FigCaptureStream start method, so start the stream's
	 * frame worker from the reservation. stream_start is idempotent (it no-ops when already running),
	 * and relinquish does not stop the worker, so the reservation thrash does not tear it down.
	 */
	if (streams != NULL) {
		CFIndex n = CFArrayGetCount(streams);

		for (CFIndex i = 0; i < n; i++) {
			CMBaseObjectRef st = (CMBaseObjectRef)CFArrayGetValueAtIndex(streams, i);

			/* ... but only a stream the graph has configured: one of its outputs has a PixelBufferHandler.
			 * mediaserverd also reserves both streams as it starts, only to list the cameras
			 * (FigCaptureSourceServerStart -> FigCaptureCreateSourceInfoArrayFromDeviceAndPlist, seen by a
			 * backtrace 2026-11-13), and beginning there powered the rear sensor and then streamed the front
			 * one for seconds at every start of the phone, with nobody looking -- where the kernel's
			 * read/free race stopped the phone. */
			if (st != NULL) {
				struct stream *s = stream_of(st);
				bool wanted = false;

				for (int k = 0; k < OUTPUTS; k++) {
					wanted = wanted || s->out[k].handler != NULL;
				}
				if (wanted) {
					(void)stream_begin(st, false);
				} else if (requests <= 3) {
					say("device: the %s only listed (no output handler): its sensor stays off", s->camera->name);
				}
			}
		}
	}
	return 0;
}

static OSStatus
device_relinquish_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
	(void)o;
	(void)options;
	if (relinquishes++ < 3) {
		say("device: streams given back (%ld)", streams ? (long)CFArrayGetCount(streams) : 0L);
	} else if (relinquishes % 1000 == 0) {
		say("device: streams given back %u times", relinquishes);
	}
	return 0;
}

static OSStatus
device_invalidate(CMBaseObjectRef o)
{
	(void)o;
	return 0;
}

static void
device_finalize(CMBaseObjectRef o)
{
	struct device *d = derived_storage(o);

	if (d->streams) {
		CFRelease(d->streams);
	}
	if (d->supported) {
		CFRelease(d->supported);
	}
}

static CFStringRef
device_description(CMBaseObjectRef o)
{
	(void)o;
	return CFRetain(CFSTR("<RedwoodCaptureDevice: the POCO X5 Pro's wide camera>"));
}

static const struct base_class device_base = {
	1, sizeof(struct device), NULL, device_invalidate, device_finalize, device_description, device_copy_property, device_set_property,
	NULL, NULL, NULL
};
static const struct device_class device_derived = { 3, NULL, device_request_streams, device_relinquish_streams, NULL };
static const struct vtable device_vtable = { NULL, &device_base, &device_derived };

/* The vendor's device create function (as H10ISPCaptureDeviceCreate is called). */
OSStatus redwood_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out);

OSStatus
redwood_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out)
{
	static bool found, usable;
	CMBaseObjectRef device = NULL, stream[2];
	struct device *d;

	(void)options;
	if (!found) {
		found = true;
		usable = find_functions();
	}
	if (!usable || out == NULL) {
		return ERR_UNSUPPORTED;
	}
	if ((stream[0] = stream_create(allocator, &cameras[0], port_back)) == NULL) {
		return ERR_UNSUPPORTED;
	}
	if ((stream[1] = stream_create(allocator, &cameras[1], port_front)) == NULL) {
		CFRelease(stream[0]);
		return ERR_UNSUPPORTED;
	}
	if (derived_create(allocator, &device_vtable, device_class_id(), &device) != 0 || device == NULL) {
		CFRelease(stream[0]);
		CFRelease(stream[1]);
		say("the device cannot be made");
		return ERR_UNSUPPORTED;
	}
	d = derived_storage(device);
	memset(d, 0, sizeof(*d));
	d->streams = CFArrayCreate(NULL, (const void **)stream, 2, &kCFTypeArrayCallBacks);
	all_streams[0] = stream[0];             /* (kept by the device's array) */
	all_streams[1] = stream[1];
	d->supported = supported(device_props, sizeof(device_props) / sizeof(device_props[0]));
	CFRelease(stream[0]);
	CFRelease(stream[1]);
	*out = device;
	say("the device made: two streams, the back camera and the front one");
	return 0;
}

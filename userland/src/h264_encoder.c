/*
 * A software H.264 encoder for VideoToolbox (B, 2026-11-12): the Galaxy S20 has no AVE, the iPhone's video
 * encoder, so Apple's only H.264/HEVC encoders (/System/Library/VideoEncoders/AppleVideoEncoder.bundle) cannot
 * run and every recording came out as an empty file. This one encodes with OpenH264 (Cisco, BSD licence;
 * work/third-party/openh264, built for iOS arm64) and is registered with VideoToolbox in whatever process
 * loads it (libredwood-media: mediaserverd, where the camera records).
 *
 * The encoder is a CoreMedia derived object of VideoToolbox's encoder class, laid out as Apple's own (read from
 * AppleVideoEncoder, whose functions log their names; tools/camera/CHECKLIST.md "Video encoder plan"):
 *   factory   OSStatus create(FourCharCode codec, CFAllocatorRef, CMBaseObjectRef *out)
 *   base      version 1: storage size, invalidate, finalize, copy property, set property
 *   encoder   version 3: [1] StartSession(enc, session, CMVideoDimensions (width low, height high), CFDictionary)
 *                        [2] EncodeFrame(enc, token, pixel buffer, CMTime *pts, CMTime *duration, options, flags *)
 *                        [3] CompleteFrames(enc, CMTime *until)   [4] CopySupportedPropertyDictionary(enc, out)
 *                        [5] SetProperties(enc, dictionary)        [7] PrepareToEncodeFrames(enc)
 * Frames are encoded as they come (no reordering: every frame's decode time is its presentation time) and given
 * back with VTEncoderSessionEmitEncodedFrame; SPS and PPS go into the format description (avcC), the samples are
 * AVCC (each NAL unit after its length, big-endian, 4 bytes).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wels/codec_api.h"

static char video_flip;
static void read_video_flip(void);

typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef const void *CMBaseClassID;
typedef void *VTEncoderSessionRef;
typedef void *VTEncoderFrameToken;

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
struct encoder_class {
	uint64_t        version;                /* 3 */
	OSStatus        (*start_session)(CMBaseObjectRef, VTEncoderSessionRef, uint64_t, CFDictionaryRef);
	OSStatus        (*encode_frame)(CMBaseObjectRef, VTEncoderFrameToken, CVPixelBufferRef, const CMTime *, const CMTime *,
	    CFDictionaryRef, uint32_t *);
	OSStatus        (*complete_frames)(CMBaseObjectRef, const CMTime *);
	OSStatus        (*copy_supported_property_dictionary)(CMBaseObjectRef, CFDictionaryRef *);
	OSStatus        (*set_properties)(CMBaseObjectRef, CFDictionaryRef);
	void *          slot6;
	OSStatus        (*prepare_to_encode_frames)(CMBaseObjectRef);
	void *          rest[16];               /* passes, tiles: not offered */
};
struct vtable {
	const void *    reserved;
	const struct base_class *base;
	const struct encoder_class *derived;
};

#define ERR_PARAM               (-12902)        /* kVTParameterErr */
#define ERR_NOT_SUPPORTED       (-12900)        /* kVTPropertyNotSupportedErr */
#define ERR_ALLOC               (-12904)        /* kVTAllocationFailedErr */
#define ERR_ENCODER             (-12911)        /* kVTVideoEncoderMalfunctionErr */
#define INFO_DROPPED            (1u << 1)       /* kVTEncodeInfo_FrameDropped */

static OSStatus (*derived_create)(CFAllocatorRef, const struct vtable *, CMBaseClassID, CMBaseObjectRef *);
static void *(*derived_storage)(CMBaseObjectRef);
static CMBaseClassID (*encoder_class_id)(void);
static OSStatus (*register_encoder)(FourCharCode, CFDictionaryRef, OSStatus (*)(FourCharCode, CFAllocatorRef, CMBaseObjectRef *));
static OSStatus (*session_set_pixel_attributes)(VTEncoderSessionRef, CFDictionaryRef);
static OSStatus (*session_emit)(VTEncoderSessionRef, VTEncoderFrameToken, OSStatus, uint32_t, CMSampleBufferRef);

/* the compression properties' names, looked up once (VideoToolbox's own CFStrings) */
static CFStringRef k_bitrate, k_max_key_interval, k_max_key_duration, k_expected_rate, k_real_time, k_profile_level,
    k_allow_reordering, k_allow_temporal, k_using_hardware, k_data_rate_limits, k_quality, k_entropy, k_force_key,
    k_prop_type, k_prop_type_number, k_prop_type_boolean, k_prop_type_enum, k_rw_status, k_rw_readwrite;

struct encoder {
	VTEncoderSessionRef session;
	ISVCEncoder *   svc;
	int32_t         width, height;
	int32_t         bitrate;                /* bits a second */
	float           fps;
	int32_t         key_interval;           /* frames */
	bool            started;
	CMVideoFormatDescriptionRef format;
	uint8_t         sps[64], pps[64];
	size_t          sps_len, pps_len;
	uint8_t *       i420;
	size_t          i420_size;
	uint8_t *       out;
	size_t          out_size;
	uint64_t        frames, bytes;
	pthread_mutex_t lock;
};

static void
say(const char *format, ...)
{
	char line[300];
	va_list ap;
	int n = snprintf(line, sizeof(line), "redwood: h264: ");

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
}

static struct encoder *
enc_of(CMBaseObjectRef o)
{
	return derived_storage(o);
}

static CFStringRef
vt_string(void *vt, const char *name)
{
	CFStringRef *p = dlsym(vt, name);
	return p ? *p : NULL;
}

static bool
number_of(CFTypeRef v, double *out)
{
	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
		return CFNumberGetValue(v, kCFNumberDoubleType, out);
	}
	return false;
}

/* ---- OpenH264 ---------------------------------------------------------------------------------------- */

static void
svc_down(struct encoder *e)
{
	if (e->svc != NULL) {
		(*e->svc)->Uninitialize(e->svc);
		WelsDestroySVCEncoder(e->svc);
		e->svc = NULL;
	}
}

static bool
svc_up(struct encoder *e)
{
	SEncParamExt p;
	int fmt = videoFormatI420;

	if (e->svc != NULL) {
		return true;
	}
	if (WelsCreateSVCEncoder(&e->svc) != 0 || e->svc == NULL) {
		e->svc = NULL;
		return false;
	}
	memset(&p, 0, sizeof(p));
	(*e->svc)->GetDefaultParams(e->svc, &p);
	p.iUsageType = CAMERA_VIDEO_REAL_TIME;
	p.iPicWidth = e->width;
	p.iPicHeight = e->height;
	p.iTargetBitrate = e->bitrate;
	p.iMaxBitrate = UNSPECIFIED_BIT_RATE;
	p.iRCMode = RC_BITRATE_MODE;
	p.fMaxFrameRate = e->fps;
	p.bEnableFrameSkip = false;                     /* (the camera's frames: all of them, at the bitrate's cost) */
	p.uiIntraPeriod = (unsigned int)e->key_interval;
	p.iMultipleThreadIdc = 6;                       /* six of the eight cores; slices let them share a frame */
	p.iComplexityMode = LOW_COMPLEXITY;             /* the camera's pace first (40 ms a frame at MEDIUM beside the ISP) */
	p.iSpatialLayerNum = 1;
	p.iTemporalLayerNum = 1;
	p.bEnableDenoise = false;
	p.bEnableBackgroundDetection = false;          /* (both cost time a frame; the bitrate is ample) */
	p.bEnableAdaptiveQuant = false;
	p.bEnableLongTermReference = false;
	p.eSpsPpsIdStrategy = CONSTANT_ID;            /* one SPS/PPS for the whole file: a movie has one format description */
	p.bEnableSceneChangeDetect = false;            /* key frames on the schedule only */
	p.iEntropyCodingModeFlag = 0;                   /* CAVLC (OpenH264's CABAC is for its High profile only) */
	p.sSpatialLayers[0].iVideoWidth = e->width;
	p.sSpatialLayers[0].iVideoHeight = e->height;
	p.sSpatialLayers[0].fFrameRate = e->fps;
	p.sSpatialLayers[0].iSpatialBitrate = e->bitrate;
	p.sSpatialLayers[0].iMaxSpatialBitrate = UNSPECIFIED_BIT_RATE;
	p.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
	p.sSpatialLayers[0].uiLevelIdc = e->width * e->height > 1920 * 1088 ? LEVEL_5_1 : LEVEL_4_1;
	p.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
	p.sSpatialLayers[0].sSliceArgument.uiSliceNum = 6;
	if ((*e->svc)->InitializeExt(e->svc, &p) != 0) {
		say("OpenH264 refused %dx%d at %.1f frames a second, %d bit/s", e->width, e->height, e->fps, e->bitrate);
		WelsDestroySVCEncoder(e->svc);
		e->svc = NULL;
		return false;
	}
	(*e->svc)->SetOption(e->svc, ENCODER_OPTION_DATAFORMAT, &fmt);
	say("OpenH264 up: %dx%d, %.1f frames a second, %d kbit/s, a key frame every %d", e->width, e->height, e->fps,
	    e->bitrate / 1000, e->key_interval);
	return true;
}

/* NV12 (420v/420f) -> I420 into e->i420 */
static bool
to_i420(struct encoder *e, CVPixelBufferRef pb)
{
	OSType f = CVPixelBufferGetPixelFormatType(pb);
	size_t w = (size_t)e->width, h = (size_t)e->height, need = w * h * 3 / 2;

	if ((f != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && f != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) ||
	    CVPixelBufferGetWidth(pb) < w || CVPixelBufferGetHeight(pb) < h) {
		return false;
	}
	if (e->i420_size < need) {
		free(e->i420);
		e->i420 = malloc(need);
		e->i420_size = e->i420 ? need : 0;
		if (e->i420 == NULL) {
			return false;
		}
	}
	if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
		return false;
	}
	const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
	size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
	uint8_t *oy = e->i420, *ou = oy + w * h, *ov = ou + w * h / 4;

	for (size_t r = 0; r < h; r++) {
		memcpy(oy + r * w, y + r * ys, w);
	}
	for (size_t r = 0; r < h / 2; r++) {
		const uint8_t *in = uv + r * uvs;
		uint8_t *u = ou + r * (w / 2), *v = ov + r * (w / 2);

		for (size_t c = 0; c < w / 2; c++) {
			u[c] = in[2 * c];
			v[c] = in[2 * c + 1];
		}
	}
	CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
	return true;
}

static bool
out_room(struct encoder *e, size_t need)
{
	if (e->out_size >= need) {
		return true;
	}
	free(e->out);
	e->out = malloc(need);
	e->out_size = e->out ? need : 0;
	return e->out != NULL;
}

/* ---- the encoder class ----------------------------------------------------------------------------- */

static OSStatus
start_session(CMBaseObjectRef o, VTEncoderSessionRef session, uint64_t dims, CFDictionaryRef extra)
{
	struct encoder *e = enc_of(o);
	int32_t w = (int32_t)(uint32_t)dims, h = (int32_t)(uint32_t)(dims >> 32);

	(void)extra;
	if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) {
		say("StartSession: %dx%d not taken", w, h);
		return ERR_PARAM;
	}
	pthread_mutex_lock(&e->lock);
	e->session = session;
	e->width = w;
	e->height = h;
	if (e->bitrate <= 0) {
		e->bitrate = (int32_t)((double)w * h * (e->fps > 0 ? e->fps : 30) * 0.2);     /* ~12 Mbit/s at 1080p30 */
	}
	pthread_mutex_unlock(&e->lock);

	/* NV12 in either range, the size of the session */
	int32_t formats[2] = { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange };
	CFNumberRef f0 = CFNumberCreate(NULL, kCFNumberSInt32Type, &formats[0]), f1 = CFNumberCreate(NULL, kCFNumberSInt32Type, &formats[1]);
	CFTypeRef fs[2] = { f0, f1 };
	CFArrayRef list = CFArrayCreate(NULL, fs, 2, &kCFTypeArrayCallBacks);
	CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &w), nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &h);
	CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *keys[4] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey };
	const void *values[4] = { list, nw, nh, empty };
	CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	OSStatus st = session_set_pixel_attributes ? session_set_pixel_attributes(session, attrs) : 0;

	CFRelease(attrs); CFRelease(empty); CFRelease(nh); CFRelease(nw); CFRelease(list); CFRelease(f1); CFRelease(f0);
	read_video_flip();
	say("StartSession: %dx%d (pixel buffer attributes: %d), flip %c", w, h, (int)st, video_flip);
	return 0;
}

static OSStatus
prepare_to_encode_frames(CMBaseObjectRef o)
{
	struct encoder *e = enc_of(o);
	bool ok;

	pthread_mutex_lock(&e->lock);
	ok = svc_up(e);
	pthread_mutex_unlock(&e->lock);
	return ok ? 0 : ERR_ENCODER;
}


/*
 * The front camera's videos came out mirrored left to right while its preview and photographs were right
 * (2026-11-12): the movie is turned for portrait by its matrix, so left/right on the screen is up/down in the
 * buffer -- the rows are put the other way round here, for the movie only. /var/root/redwood-video-flip may say
 * otherwise: "v" (rows, the default), "h" (columns) or "0" (none); read at each session's start.
 */

static void
read_video_flip(void)
{
	FILE *f = fopen("/var/root/redwood-video-flip", "r");
	int c;

	video_flip = '0';               /* (none: with cam_turn 7 the movie comes out right, as iOS turns it) */
	if (f != NULL) {
		c = fgetc(f);
		if (c == 'v' || c == 'h' || c == '0') {
			video_flip = (char)c;
		}
		fclose(f);
	}
}

static void
flip_plane(uint8_t *p, int w, int h, char how)
{
	if (how == 'v') {
		uint8_t tmp[4096];

		for (int r = 0; r < h / 2 && w <= (int)sizeof(tmp); r++) {
			uint8_t *a = p + (size_t)r * w, *b = p + (size_t)(h - 1 - r) * w;

			memcpy(tmp, a, (size_t)w);
			memcpy(a, b, (size_t)w);
			memcpy(b, tmp, (size_t)w);
		}
	} else if (how == 'h') {
		for (int r = 0; r < h; r++) {
			uint8_t *a = p + (size_t)r * w, *b = a + w - 1;

			for (; a < b; a++, b--) {
				uint8_t t = *a;
				*a = *b;
				*b = t;
			}
		}
	}
}

static OSStatus
encode_frame_inner(CMBaseObjectRef o, VTEncoderFrameToken token, CVPixelBufferRef pb, const CMTime *pts, const CMTime *duration,
    CFDictionaryRef options, uint32_t *info_out, bool *emitted)
{
	struct encoder *e = enc_of(o);
	SSourcePicture pic;
	SFrameBSInfo bs;
	CMSampleBufferRef sample = NULL;
	CMBlockBufferRef block = NULL;
	OSStatus st = 0;
	size_t len = 0;
	bool key = false;

	if (info_out) {
		*info_out = 0;
	}
	pthread_mutex_lock(&e->lock);
	if (!svc_up(e) || !to_i420(e, pb)) {
		pthread_mutex_unlock(&e->lock);
		say("EncodeFrame: no encoder or an unusable pixel buffer (%#x)", (unsigned)CVPixelBufferGetPixelFormatType(pb));
		return ERR_ENCODER;
	}
	if (options != NULL && k_force_key != NULL) {
		CFTypeRef v = CFDictionaryGetValue(options, k_force_key);

		if (v == kCFBooleanTrue) {
			(*e->svc)->ForceIntraFrame(e->svc, true);
		}
	}
	if (video_flip != '0') {
		flip_plane(e->i420, e->width, e->height, video_flip);
		flip_plane(e->i420 + (size_t)e->width * e->height, e->width / 2, e->height / 2, video_flip);
		flip_plane(e->i420 + (size_t)e->width * e->height * 5 / 4, e->width / 2, e->height / 2, video_flip);
	}
	memset(&pic, 0, sizeof(pic));
	pic.iColorFormat = videoFormatI420;
	pic.iPicWidth = e->width;
	pic.iPicHeight = e->height;
	pic.iStride[0] = e->width;
	pic.iStride[1] = pic.iStride[2] = e->width / 2;
	pic.pData[0] = e->i420;
	pic.pData[1] = e->i420 + (size_t)e->width * e->height;
	pic.pData[2] = pic.pData[1] + (size_t)e->width * e->height / 4;
	pic.uiTimeStamp = (long long)(CMTimeGetSeconds(*pts) * 1000.0);
	memset(&bs, 0, sizeof(bs));
	if ((*e->svc)->EncodeFrame(e->svc, &pic, &bs) != cmResultSuccess) {
		pthread_mutex_unlock(&e->lock);
		return ERR_ENCODER;
	}
	if (bs.eFrameType == videoFrameTypeSkip || bs.eFrameType == videoFrameTypeInvalid) {
		pthread_mutex_unlock(&e->lock);
		if (info_out) {
			*info_out |= INFO_DROPPED;
		}
		*emitted = true;
		return session_emit(e->session, token, 0, INFO_DROPPED, NULL);
	}
	key = bs.eFrameType == videoFrameTypeIDR;

	/* the NAL units: SPS and PPS kept for the format description, the rest as AVCC */
	size_t total = 0;
	for (int l = 0; l < bs.iLayerNum; l++) {
		for (int n = 0; n < bs.sLayerInfo[l].iNalCount; n++) {
			total += (size_t)bs.sLayerInfo[l].pNalLengthInByte[n];
		}
	}
	if (!out_room(e, total + 64)) {
		pthread_mutex_unlock(&e->lock);
		return ERR_ALLOC;
	}
	bool params_changed = false;
	for (int l = 0; l < bs.iLayerNum; l++) {
		const uint8_t *p = bs.sLayerInfo[l].pBsBuf;

		for (int n = 0; n < bs.sLayerInfo[l].iNalCount; n++) {
			size_t nl = (size_t)bs.sLayerInfo[l].pNalLengthInByte[n], sc = 0;

			while (sc < nl && sc < 4 && p[sc] == 0) {
				sc++;
			}
			if (sc < nl && p[sc] == 1) {
				sc++;                   /* (past the start code: 00 00 00 01 or 00 00 01) */
			} else {
				sc = 0;
			}
			const uint8_t *nal = p + sc;
			size_t nal_len = nl - sc;
			uint8_t type = nal_len ? (nal[0] & 0x1f) : 0;

			if (type == 7 || type == 8) {
				uint8_t *dst = type == 7 ? e->sps : e->pps;
				size_t *dl = type == 7 ? &e->sps_len : &e->pps_len;

				if (nal_len <= sizeof(e->sps) && (*dl != nal_len || memcmp(dst, nal, nal_len) != 0)) {
					memcpy(dst, nal, nal_len);
					*dl = nal_len;
					params_changed = true;
				}
			} else if (nal_len > 0) {
				e->out[len] = (uint8_t)(nal_len >> 24);
				e->out[len + 1] = (uint8_t)(nal_len >> 16);
				e->out[len + 2] = (uint8_t)(nal_len >> 8);
				e->out[len + 3] = (uint8_t)nal_len;
				memcpy(e->out + len + 4, nal, nal_len);
				len += 4 + nal_len;
			}
			p += nl;
		}
	}
	if ((params_changed || e->format == NULL) && e->sps_len && e->pps_len) {
		const uint8_t *sets[2] = { e->sps, e->pps };
		size_t sizes[2] = { e->sps_len, e->pps_len };
		CMVideoFormatDescriptionRef fd = NULL;

		if (CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &fd) == 0) {
			if (e->format) {
				CFRelease(e->format);
			}
			e->format = fd;
		}
	}
	if (e->format == NULL || len == 0) {
		pthread_mutex_unlock(&e->lock);
		say("EncodeFrame: no picture data or no parameter sets yet (%zu bytes)", len);
		return ERR_ENCODER;
	}
	st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, len, NULL, NULL, 0, len, kCMBlockBufferAssureMemoryNowFlag, &block);
	if (st == 0) {
		st = CMBlockBufferReplaceDataBytes(e->out, block, 0, len);
	}
	if (st == 0) {
		CMSampleTimingInfo t = { .duration = duration ? *duration : kCMTimeInvalid, .presentationTimeStamp = *pts,
			.decodeTimeStamp = *pts };

		st = CMSampleBufferCreateReady(NULL, block, e->format, 1, 1, &t, 1, &len, &sample);
	}
	if (st == 0) {
		CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sample, true);

		if (att != NULL && CFArrayGetCount(att) > 0) {
			CFMutableDictionaryRef a = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(att, 0);

			CFDictionarySetValue(a, kCMSampleAttachmentKey_NotSync, key ? kCFBooleanFalse : kCFBooleanTrue);
			CFDictionarySetValue(a, kCMSampleAttachmentKey_DependsOnOthers, key ? kCFBooleanFalse : kCFBooleanTrue);
		}
	}
	e->frames++;
	e->bytes += len;
	if (e->frames == 1 || e->frames % 30 == 0) {
		say("frame %llu: %zu bytes%s, %.1f kbit/s so far", e->frames, len, key ? " (key)" : "",
		    e->frames > 1 ? (double)e->bytes * 8 / ((double)e->frames / (e->fps > 0 ? e->fps : 30)) / 1000 : 0.0);
	}
	pthread_mutex_unlock(&e->lock);
	if (block) {
		CFRelease(block);
	}
	if (st != 0) {
		return st;
	}
	*emitted = true;
	st = session_emit(e->session, token, 0, 0, sample);
	CFRelease(sample);
	return st;
}

/*
 * Every frame asked for is answered, with its picture or with an error: a frame that failed and was only refused
 * left VideoToolbox waiting for it when the recording stopped ("Timed out waiting for the capture graph to stop",
 * mediaserverd SIGTRAP, the movie never finished: 2026-11-12).
 */
static OSStatus
encode_frame(CMBaseObjectRef o, VTEncoderFrameToken token, CVPixelBufferRef pb, const CMTime *pts, const CMTime *duration,
    CFDictionaryRef options, uint32_t *info_out)
{
	struct encoder *e = enc_of(o);
	bool emitted = false;
	uint64_t t0 = mach_absolute_time();
	OSStatus st = encode_frame_inner(o, token, pb, pts, duration, options, info_out, &emitted);
	/* the recorder's pace: how often EncodeFrame comes, and how long it takes (every 30 calls) */
	static uint64_t calls, busy, first;
	uint64_t t1 = mach_absolute_time();

	if (calls == 0 || first == 0) {
		first = t0;
	}
	calls++;
	busy += t1 - t0;
	if (calls % 30 == 0) {
		say("EncodeFrame: %llu calls (%llu frames out), %.1f calls a second, %.1f ms each, pts %.3f s",
		    calls, e->frames, (double)30 * 24000000.0 / (double)(t1 - first), (double)busy / 24000.0 / 30,
		    pts && pts->timescale ? (double)pts->value / pts->timescale : -1.0);
		busy = 0;
		first = t1;
	}

	if (!emitted) {
		static int told;
		if (told++ < 10) {
			say("EncodeFrame failed (%d) for a %#x %zux%zu buffer: answered as dropped", (int)st,
			    pb ? (unsigned)CVPixelBufferGetPixelFormatType(pb) : 0u, pb ? CVPixelBufferGetWidth(pb) : 0, pb ? CVPixelBufferGetHeight(pb) : 0);
		}
		if (info_out) {
			*info_out |= INFO_DROPPED;
		}
		(void)session_emit(e->session, token, st ? st : ERR_ENCODER, INFO_DROPPED, NULL);
		return 0;
	}
	return st;
}

static OSStatus
complete_frames(CMBaseObjectRef o, const CMTime *until)
{
	struct encoder *e = enc_of(o);

	(void)until;
	say("CompleteFrames after %llu frames", e->frames);
	return 0;                               /* (every frame is given back from EncodeFrame) */
}

static OSStatus set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value);

static void
set_one(const void *key, const void *value, void *context)
{
	(void)set_property(context, key, value);
}

static OSStatus
set_properties(CMBaseObjectRef o, CFDictionaryRef props)
{
	if (props != NULL) {
		CFDictionaryApplyFunction(props, set_one, o);
	}
	return 0;
}

static void
add_supported(CFMutableDictionaryRef d, CFStringRef key, CFStringRef type)
{
	if (key == NULL) {
		return;
	}
	const void *k[2] = { k_prop_type, k_rw_status };
	const void *v[2] = { type, k_rw_readwrite };
	CFDictionaryRef info = CFDictionaryCreate(NULL, k, v, k_prop_type && k_rw_status && type && k_rw_readwrite ? 2 : 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	CFDictionarySetValue(d, key, info);
	CFRelease(info);
}

static OSStatus
copy_supported_property_dictionary(CMBaseObjectRef o, CFDictionaryRef *out)
{
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	(void)o;
	add_supported(d, k_bitrate, k_prop_type_number);
	add_supported(d, k_data_rate_limits, k_prop_type_number);
	add_supported(d, k_max_key_interval, k_prop_type_number);
	add_supported(d, k_max_key_duration, k_prop_type_number);
	add_supported(d, k_expected_rate, k_prop_type_number);
	add_supported(d, k_quality, k_prop_type_number);
	add_supported(d, k_real_time, k_prop_type_boolean);
	add_supported(d, k_allow_reordering, k_prop_type_boolean);
	add_supported(d, k_allow_temporal, k_prop_type_boolean);
	add_supported(d, k_profile_level, k_prop_type_enum);
	add_supported(d, k_entropy, k_prop_type_enum);
	*out = d;
	return 0;
}

/* ---- the base class ------------------------------------------------------------------------------- */

static OSStatus
set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	struct encoder *e = enc_of(o);
	double v;

	if (key == NULL) {
		return ERR_PARAM;
	}
	pthread_mutex_lock(&e->lock);
	if (k_bitrate && CFEqual(key, k_bitrate) && number_of(value, &v) && v > 0) {
		/* 1.6 times what is asked: the rate is meant for the iPhone's encoder (High profile, CABAC); this one is
		 * baseline CAVLC at low complexity and looked soft at 13 Mbit/s (user, 2026-11-12) */
		e->bitrate = (int32_t)(v * 1.6);
	} else if (k_expected_rate && CFEqual(key, k_expected_rate) && number_of(value, &v) && v > 0) {
		e->fps = (float)v;
	} else if (k_max_key_interval && CFEqual(key, k_max_key_interval) && number_of(value, &v) && v > 0) {
		e->key_interval = (int32_t)v;
	} else if (k_max_key_duration && CFEqual(key, k_max_key_duration) && number_of(value, &v) && v > 0) {
		e->key_interval = (int32_t)(v * (e->fps > 0 ? e->fps : 30));
	}
	/* everything else offered (real time, profile, reordering, ...) is taken and has no effect: baseline, no B frames */
	pthread_mutex_unlock(&e->lock);
	return 0;
}

static OSStatus
copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef alloc, void *out)
{
	struct encoder *e = enc_of(o);

	(void)alloc;
	if (key == NULL || out == NULL) {
		return ERR_PARAM;
	}
	if (k_using_hardware && CFEqual(key, k_using_hardware)) {
		*(CFTypeRef *)out = kCFBooleanFalse;
		return 0;
	}
	if (k_bitrate && CFEqual(key, k_bitrate)) {
		*(CFTypeRef *)out = CFNumberCreate(NULL, kCFNumberSInt32Type, &e->bitrate);
		return 0;
	}
	if (k_allow_reordering && CFEqual(key, k_allow_reordering)) {
		*(CFTypeRef *)out = kCFBooleanFalse;
		return 0;
	}
	return ERR_NOT_SUPPORTED;
}

static OSStatus
invalidate(CMBaseObjectRef o)
{
	struct encoder *e = enc_of(o);

	say("Invalidate after %llu frames", e->frames);
	pthread_mutex_lock(&e->lock);
	svc_down(e);
	pthread_mutex_unlock(&e->lock);
	return 0;
}

static void
finalize(CMBaseObjectRef o)
{
	struct encoder *e = enc_of(o);

	svc_down(e);
	if (e->format) {
		CFRelease(e->format);
	}
	free(e->i420);
	free(e->out);
	if (e->frames) {
		say("encoder done: %llu frames, %llu bytes", e->frames, e->bytes);
	}
	pthread_mutex_destroy(&e->lock);
}

static const struct base_class base = {
	1, sizeof(struct encoder), NULL, invalidate, finalize, NULL, copy_property, set_property, NULL, NULL, NULL
};
static const struct encoder_class derived = {
	3, start_session, encode_frame, complete_frames, copy_supported_property_dictionary, set_properties, NULL,
	prepare_to_encode_frames, { NULL }
};
static const struct vtable table = { NULL, &base, &derived };

static OSStatus
create_instance(FourCharCode codec, CFAllocatorRef alloc, CMBaseObjectRef *out)
{
	CMBaseObjectRef o = NULL;
	OSStatus st;

	say("factory called: codec %#x, allocator %p, out %p", (unsigned)codec, (void *)alloc, (void *)out);
	if (codec != kCMVideoCodecType_H264 || out == NULL) {
		return ERR_PARAM;
	}
	st = derived_create(alloc, &table, encoder_class_id(), &o);
	if (st != 0 || o == NULL) {
		say("could not make an encoder object (%d)", (int)st);
		return st ? st : ERR_ALLOC;
	}
	struct encoder *e = enc_of(o);

	memset(e, 0, sizeof(*e));
	pthread_mutex_init(&e->lock, NULL);
	e->fps = 30;
	e->key_interval = 30;
	*out = o;
	say("an encoder made");
	return 0;
}

#define ENCODER_ID "com.redwood.videotoolbox.videoencoder.h264.openh264"

/* Register the encoder with this process's VideoToolbox; true when it is. */
bool
redwood_h264_register(void)
{
	static bool done, ok;
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_LAZY);
	void *cm = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_LAZY);

	if (done) {
		return ok;
	}
	done = true;
	if (vt == NULL || cm == NULL) {
		say("no VideoToolbox or CoreMedia");
		return false;
	}
	derived_create = dlsym(cm, "CMDerivedObjectCreate");
	derived_storage = dlsym(cm, "CMBaseObjectGetDerivedStorage");
	encoder_class_id = dlsym(vt, "VTVideoEncoderGetClassID");
	register_encoder = dlsym(vt, "VTRegisterVideoEncoderWithInfo");
	session_set_pixel_attributes = dlsym(vt, "VTEncoderSessionSetPixelBufferAttributes");
	session_emit = dlsym(vt, "VTEncoderSessionEmitEncodedFrame");
	if (!derived_create || !derived_storage || !encoder_class_id || !register_encoder || !session_emit) {
		say("VideoToolbox lacks what an encoder needs");
		return false;
	}
	k_bitrate = vt_string(vt, "kVTCompressionPropertyKey_AverageBitRate");
	k_data_rate_limits = vt_string(vt, "kVTCompressionPropertyKey_DataRateLimits");
	k_max_key_interval = vt_string(vt, "kVTCompressionPropertyKey_MaxKeyFrameInterval");
	k_max_key_duration = vt_string(vt, "kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration");
	k_expected_rate = vt_string(vt, "kVTCompressionPropertyKey_ExpectedFrameRate");
	k_real_time = vt_string(vt, "kVTCompressionPropertyKey_RealTime");
	k_profile_level = vt_string(vt, "kVTCompressionPropertyKey_ProfileLevel");
	k_allow_reordering = vt_string(vt, "kVTCompressionPropertyKey_AllowFrameReordering");
	k_allow_temporal = vt_string(vt, "kVTCompressionPropertyKey_AllowTemporalCompression");
	k_using_hardware = vt_string(vt, "kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder");
	k_quality = vt_string(vt, "kVTCompressionPropertyKey_Quality");
	k_entropy = vt_string(vt, "kVTCompressionPropertyKey_H264EntropyMode");
	k_force_key = vt_string(vt, "kVTEncodeFrameOptionKey_ForceKeyFrame");
	k_prop_type = vt_string(vt, "kVTPropertyTypeKey");
	k_prop_type_number = vt_string(vt, "kVTPropertyType_Number");
	k_prop_type_boolean = vt_string(vt, "kVTPropertyType_Boolean");
	k_prop_type_enum = vt_string(vt, "kVTPropertyType_Enumeration");
	k_rw_status = vt_string(vt, "kVTPropertyReadWriteStatusKey");
	k_rw_readwrite = vt_string(vt, "kVTPropertyReadWriteStatus_ReadWrite");

	CFStringRef id_key = CFSTR("CMClassImplementationID"), id_value = CFSTR(ENCODER_ID);
	CFDictionaryRef info = CFDictionaryCreate(NULL, (const void **)&id_key, (const void **)&id_value, 1,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	OSStatus st = register_encoder(kCMVideoCodecType_H264, info, create_instance);
	/* REDWOOD_H264_PLAIN: also the registration without info (VTRegisterVideoEncoder(codec, factory)) */
	if (getenv("REDWOOD_H264_PLAIN")) {
		OSStatus (*plain)(FourCharCode, OSStatus (*)(FourCharCode, CFAllocatorRef, CMBaseObjectRef *)) = dlsym(vt, "VTRegisterVideoEncoder");
		if (plain) say("VTRegisterVideoEncoder: %d", (int)plain(kCMVideoCodecType_H264, create_instance));
	}

	CFRelease(info);
	ok = st == 0;
	say("registered %s for avc1: %d", ENCODER_ID, (int)st);
	return ok;
}

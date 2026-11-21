/*
 * vt_enc_test: src/h264_encoder.c through VideoToolbox's public compression API, as the camera's recorder uses it.
 * Registers the OpenH264 encoder in this process, opens a VTCompressionSession for H.264 at WIDTHxHEIGHT, gives it
 * FRAMES moving NV12 test frames, and writes what comes back as an Annex-B elementary stream (play it on the Mac:
 * ffplay out.h264, or wrap it: ffmpeg -i out.h264 -c copy out.mp4).
 *
 * usage: vt_enc_test [WIDTH HEIGHT FRAMES [OUT.h264]]      (default 1920 1080 90 /var/tmp/vt_enc_test.h264)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool redwood_h264_register(void);

static FILE *out;
static int got, keys;
static size_t total;

static void
write_annexb(const uint8_t *p, size_t n)
{
	static const uint8_t sc[4] = { 0, 0, 0, 1 };

	fwrite(sc, 1, 4, out);
	fwrite(p, 1, n, out);
}

static void
done(void *ref, void *frame_ref, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
{
	(void)ref; (void)frame_ref;
	if (status != 0 || sample == NULL) {
		printf("vt_enc_test: frame status %d flags %#x%s\n", (int)status, (unsigned)flags, sample ? "" : " (no sample)");
		return;
	}
	CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sample, false);
	bool key = true;

	if (att && CFArrayGetCount(att) > 0) {
		CFDictionaryRef a = CFArrayGetValueAtIndex(att, 0);
		key = CFDictionaryGetValue(a, kCMSampleAttachmentKey_NotSync) != kCFBooleanTrue;
	}
	if (key) {
		CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sample);
		size_t count = 0;

		CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, NULL, NULL, &count, NULL);
		for (size_t i = 0; i < count; i++) {
			const uint8_t *ps;
			size_t psl;

			if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, i, &ps, &psl, NULL, NULL) == 0) {
				write_annexb(ps, psl);
			}
		}
		keys++;
	}
	CMBlockBufferRef b = CMSampleBufferGetDataBuffer(sample);
	size_t len = CMBlockBufferGetDataLength(b);
	uint8_t *data = malloc(len);

	CMBlockBufferCopyDataBytes(b, 0, len, data);
	for (size_t o = 0; o + 4 <= len;) {
		size_t n = (size_t)data[o] << 24 | (size_t)data[o + 1] << 16 | (size_t)data[o + 2] << 8 | data[o + 3];

		if (o + 4 + n > len) {
			printf("vt_enc_test: a bad NAL length at %zu\n", o);
			break;
		}
		write_annexb(data + o + 4, n);
		o += 4 + n;
	}
	free(data);
	total += len;
	got++;
}

int
main(int argc, char **argv)
{
	int w = argc > 3 ? atoi(argv[1]) : 1920, h = argc > 3 ? atoi(argv[2]) : 1080, frames = argc > 3 ? atoi(argv[3]) : 90;
	const char *path = argc > 4 ? argv[4] : "/var/tmp/vt_enc_test.h264";
	VTCompressionSessionRef s = NULL;
	OSStatus st;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (getenv("VT_LOCAL") && !redwood_h264_register()) {
		printf("vt_enc_test: the encoder did not register\n");
		return 1;
	}
	out = fopen(path, "wb");
	if (out == NULL) {
		perror(path);
		return 1;
	}
	{
		CFArrayRef list = NULL;
		if (VTCopyVideoEncoderList(NULL, &list) == 0 && list) {
			for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
				CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
				char id[160] = "", name[80] = "";
				CFStringRef sid = CFDictionaryGetValue(d, kVTVideoEncoderList_EncoderID), sn = CFDictionaryGetValue(d, kVTVideoEncoderList_DisplayName);
				CFNumberRef ct = CFDictionaryGetValue(d, kVTVideoEncoderList_CodecType);
				int c = 0;
				if (sid) CFStringGetCString(sid, id, sizeof(id), kCFStringEncodingUTF8);
				if (sn) CFStringGetCString(sn, name, sizeof(name), kCFStringEncodingUTF8);
				if (ct) CFNumberGetValue(ct, kCFNumberIntType, &c);
				printf("vt_enc_test: encoder %ld: %c%c%c%c %s (%s)\n", (long)i, c >> 24, c >> 16, c >> 8, c, id, name);
			}
			CFRelease(list);
		}
	}
	/* VT_ENC_ID=1: ask for our encoder by its ID (kVTVideoEncoderSpecification_EncoderID) */
	CFDictionaryRef spec = NULL;
	if (getenv("VT_ENC_ID")) {
		const void *sk[1] = { kVTVideoEncoderSpecification_EncoderID };
		const void *sv[1] = { CFSTR("com.redwood.videotoolbox.videoencoder.h264.openh264") };
		spec = CFDictionaryCreate(NULL, sk, sv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	st = VTCompressionSessionCreate(NULL, w, h, kCMVideoCodecType_H264, spec, NULL, NULL, done, NULL, &s);
	printf("vt_enc_test: VTCompressionSessionCreate %dx%d: %d\n", w, h, (int)st);
	if (st != 0) {
		return 1;
	}
	CFBooleanRef hw = NULL;

	if (VTSessionCopyProperty(s, CFSTR("UsingHardwareAcceleratedVideoEncoder"), NULL, &hw) == 0 && hw) {
		printf("vt_enc_test: hardware: %s\n", hw == kCFBooleanTrue ? "yes" : "no");
		CFRelease(hw);
	}
	int fps = 30, bitrate = w * h * 30 / 5;
	CFNumberRef nf = CFNumberCreate(NULL, kCFNumberIntType, &fps), nb = CFNumberCreate(NULL, kCFNumberIntType, &bitrate);

	printf("vt_enc_test: RealTime %d, ExpectedFrameRate %d, AverageBitRate %d, MaxKeyFrameInterval %d\n",
	    (int)VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue),
	    (int)VTSessionSetProperty(s, kVTCompressionPropertyKey_ExpectedFrameRate, nf),
	    (int)VTSessionSetProperty(s, kVTCompressionPropertyKey_AverageBitRate, nb),
	    (int)VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, nf));
	printf("vt_enc_test: PrepareToEncodeFrames %d\n", (int)VTCompressionSessionPrepareToEncodeFrames(s));

	CVPixelBufferRef pb = NULL;
	const void *k[1] = { kCVPixelBufferIOSurfacePropertiesKey };
	CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *v[1] = { empty };
	CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	if (CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &pb) != kCVReturnSuccess) {
		printf("vt_enc_test: no pixel buffer\n");
		return 1;
	}
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t t0 = mach_absolute_time();

	for (int i = 0; i < frames; i++) {
		CVPixelBufferLockBaseAddress(pb, 0);
		uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
		size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);

		for (int r = 0; r < h; r++) {
			for (int c = 0; c < w; c++) {
				y[r * ys + c] = (uint8_t)(((c + i * 8) ^ r) & 0xff);   /* a moving pattern */
			}
		}
		for (int r = 0; r < h / 2; r++) {
			for (int c = 0; c < w / 2; c++) {
				uv[r * uvs + 2 * c] = (uint8_t)(128 + 60 * (c * 4 / w * 2 - 1));
				uv[r * uvs + 2 * c + 1] = (uint8_t)(128 + (r + i) % 64 - 32);
			}
		}
		CVPixelBufferUnlockBaseAddress(pb, 0);
		st = VTCompressionSessionEncodeFrame(s, pb, CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
		if (st != 0) {
			printf("vt_enc_test: EncodeFrame %d: %d\n", i, (int)st);
			break;
		}
	}
	st = VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
	double secs = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9;

	printf("vt_enc_test: %d frames back of %d (%d key), %zu bytes, in %.2f s: %.1f frames a second; CompleteFrames %d\n",
	    got, frames, keys, total, secs, secs > 0 ? got / secs : 0, (int)st);
	VTCompressionSessionInvalidate(s);
	CFRelease(s);
	fclose(out);
	printf("vt_enc_test: wrote %s\n", path);
	return got == frames ? 0 : 1;
}

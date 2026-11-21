/*
 * rwshot OUT.bmp [SCALE]: what the screen shows, without hands (A, 2026-11-19). The display driver publishes the
 * IOSurface id of the frame on the panel (kern.redwood_fb_shown_sid); the surface is looked up, read, and written as
 * a 24-bit BMP, every SCALE-th pixel (default 2). For checking UI work from the Mac: rwshot /tmp/s.bmp, then scp.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static void
le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

int
main(int argc, char **argv)
{
	unsigned int sid = 0;
	size_t len = sizeof(sid);
	int scale = argc > 2 ? atoi(argv[2]) : 2;

	if (argc < 2 || scale < 1 || scale > 8) {
		fprintf(stderr, "usage: rwshot OUT.bmp [SCALE]\n");
		return 2;
	}
	if (sysctlbyname("kern.redwood_fb_shown_sid", &sid, &len, NULL, 0) != 0 || sid == 0) {
		fprintf(stderr, "rwshot: no frame on the panel (kern.redwood_fb_shown_sid)\n");
		return 1;
	}
	IOSurfaceRef s = IOSurfaceLookup(sid);
	if (s == NULL) {
		fprintf(stderr, "rwshot: surface %u not found\n", sid);
		return 1;
	}
	IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
	size_t w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s), bpr = IOSurfaceGetBytesPerRow(s);
	const uint8_t *base = IOSurfaceGetBaseAddress(s);
	uint32_t ow = (uint32_t)(w / (size_t)scale), oh = (uint32_t)(h / (size_t)scale), row = (ow * 3 + 3) & ~3u;
	uint8_t hdr[54] = { 'B', 'M' };
	FILE *f = fopen(argv[1], "wb");

	if (f == NULL || base == NULL || w == 0 || h == 0) {
		fprintf(stderr, "rwshot: cannot read surface %u or write %s\n", sid, argv[1]);
		return 1;
	}
	le32(hdr + 2, 54 + row * oh);
	le32(hdr + 10, 54);
	le32(hdr + 14, 40);
	le32(hdr + 18, ow);
	le32(hdr + 22, (uint32_t)-(int32_t)oh);        /* top-down */
	hdr[26] = 1;
	hdr[28] = 24;
	le32(hdr + 34, row * oh);
	fwrite(hdr, 1, sizeof(hdr), f);
	uint8_t *line = calloc(1, row);
	for (uint32_t y = 0; y < oh; y++) {
		const uint8_t *src = base + (size_t)y * (size_t)scale * bpr;
		for (uint32_t x = 0; x < ow; x++) {
			memcpy(line + 3 * x, src + (size_t)x * (size_t)scale * 4, 3);   /* BGRA -> BGR */
		}
		fwrite(line, 1, row, f);
	}
	fclose(f);
	free(line);
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	CFRelease(s);
	printf("rwshot: surface %u, %zux%zu -> %s (%ux%u)\n", sid, w, h, argv[1], ow, oh);
	return 0;
}

/*
 * libredwood-eagl.dylib: OpenGL ES for applications (games) by Mesa, the way the window server has it
 * (src/eagl.c), instead of iOS's own OpenGL ES.
 *
 * iOS's OpenGL ES runs on Metal (AppleMetalGLRenderer): it hands Metal its shaders in a private
 * form (-[MTLDevice newFunctionWithGLESIR:...], "GLIR") that only Apple's GPU compiler reads, so on
 * this GPU every game made with OpenGL ES drew nothing or died at its first shader. Here the
 * library's functions are not reached at all:
 *
 *   iOS's gl* functions (OpenGLES.framework) take a pointer from the thread's TSD slot 30
 *   (tpidrro_el0 + 0xf0) and call entry(P[4], arguments...) at P + 0x20 + offset, the offsets of
 *   src/eagl_slots.txt. +[EAGLContext setCurrentContext:] here puts there a table of ours made by
 *   mkeagl.py (each entry passes its arguments on to Mesa's function of that name), and makes a
 *   Mesa context (OSMesa, ES 2 or 3, panfrost on the Mali) current on the thread.
 *
 *   -renderbufferStorage:fromDrawable: gives the bound renderbuffer the layer's size in pixels;
 *   -presentRenderbuffer: reads it back into an IOSurface (a ring of three) and makes that the
 *   layer's contents.
 *
 * Which programs get it: kern.exec_inject (etc/inject.conf), by name. Off in a program whose
 * environment has REDWOOD_EAGL=0.
 */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef unsigned int GLenum, GLbitfield, GLuint;
typedef int GLint, GLsizei, GLfixed, GLclampx;
typedef unsigned char GLboolean, GLubyte;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned short GLushort, GLhalf;
typedef float GLfloat, GLclampf;
typedef double GLdouble, GLclampd;
typedef char GLchar;
typedef void GLvoid;
typedef long GLintptr, GLsizeiptr;
typedef int64_t GLint64;
typedef uint64_t GLuint64;
typedef struct __GLsync *GLsync;
typedef void *GLeglImageOES;
typedef void (*GLDEBUGPROC)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
typedef GLDEBUGPROC GLDEBUGPROCKHR;

#define GL_UNSIGNED_BYTE                0x1401
#define GL_RGBA                         0x1908
#define GL_RGBA8                        0x8058
#define GL_RGB565                       0x8d62
#define GL_BGRA                         0x80e1
#define GL_PACK_ROW_LENGTH              0x0d02
#define GL_PACK_ALIGNMENT               0x0d05
#define GL_RENDERBUFFER                 0x8d41
#define GL_RENDERBUFFER_BINDING         0x8ca7
#define GL_FRAMEBUFFER                  0x8d40
#define GL_READ_FRAMEBUFFER             0x8ca8
#define GL_READ_FRAMEBUFFER_BINDING     0x8caa
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_PIXEL_PACK_BUFFER_BINDING    0x88ed
#define GL_PIXEL_PACK_BUFFER            0x88eb
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_EXTENSIONS                   0x1f03

@interface NSObject (RedwoodEAGL)
- (NSDictionary *)drawableProperties;
- (void)setContentsChanged;
@end

/* ---- what the generated table needs -------------------------------------------------------------- */

static int eagl_tracing;
static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define eagl_trace(...)                 say(__VA_ARGS__)
static void eagl_trace_floats(const float *values, int count) { (void)values; (void)count; }
static long eagl_missing(void *context, unsigned offset);
#define EAGL_ENTER(context, offset)     ((void)(context))
#define EAGL_LEAVE(context, offset)     ((void)(context))

#define EAGL_GEN_DECLARATIONS
#include "eagl_app_gen.h"
#undef EAGL_GEN_DECLARATIONS
#define EAGL_GEN_TABLE
#include "eagl_app_gen.h"

/* ---- Mesa ---------------------------------------------------------------------------------------- */

#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_DEPTH_BITS               0x30
#define OSMESA_STENCIL_BITS             0x31
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE               0x38
typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);
extern void OSMesaDestroyContext(OSMesaContext context);
extern void glGetIntegerv(GLenum pname, GLint *data);
extern void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
extern void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
extern void glPixelStorei(GLenum pname, GLint param);
extern void glBindFramebuffer(GLenum target, GLuint framebuffer);
extern void glGenFramebuffers(GLsizei n, GLuint *framebuffers);
extern void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
extern void glBindBuffer(GLenum target, GLuint buffer);
extern GLenum glGetError(void);
extern const GLubyte *glGetString(GLenum name);
extern void (*OSMesaGetProcAddress(const char *name))(void);
extern void glGenTextures(GLsizei n, GLuint *textures);
extern void glBindTexture(GLenum target, GLuint texture);
extern void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
extern GLenum glCheckFramebufferStatus(GLenum target);
extern void glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
extern void glDisable(GLenum cap);
extern void glEnable(GLenum cap);
extern GLboolean glIsEnabled(GLenum cap);
extern void glFinish(void);
#define GL_TEXTURE_2D                   0x0de1
#define GL_TEXTURE_BINDING_2D           0x8069
#define GL_DRAW_FRAMEBUFFER             0x8ca9
#define GL_DRAW_FRAMEBUFFER_BINDING     0x8ca6
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_NEAREST                      0x2600
#define GL_SCISSOR_TEST                 0x0c11
#define GL_RASTERIZER_DISCARD           0x8c89

/* ---- the log ------------------------------------------------------------------------------------- */

static int console = -1;

static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	(void)write(console >= 0 ? console : 2, line, (size_t)n);
}

/* ---- contexts ------------------------------------------------------------------------------------ */

#define WINDOW 16
#define SURFACES 4

struct layer_target {                   /* a renderbuffer that is a layer's picture */
	struct layer_target *next;
	GLuint renderbuffer;
	__unsafe_unretained CALayer *layer;
	int width, height;
	IOSurfaceRef surfaces[SURFACES];
	GLuint textures[SURFACES], framebuffers[SURFACES];     /* the surfaces' own memory, drawn into by the GPU */
	int gpu;                        /* 1: copied by the GPU; -1: it cannot, by the CPU; 0: not tried yet */
	int shown;                      /* the surface the layer has */
	int said_size;
	unsigned turn;
	GLuint read_framebuffer;
	unsigned long frames;
};

struct app_context {
	void *before[4];                /* iOS's gl* functions find table[] 0x20 bytes after what TSD slot 30 holds */
	void *table[EAGL_TABLE_SIZE / 8];
	OSMesaContext mesa;
	int api;
	unsigned char window[WINDOW * WINDOW * 4];
	struct layer_target *targets;
	__unsafe_unretained id object;
	__unsafe_unretained id sharegroup;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef contexts;         /* EAGLContext -> struct app_context */
static CFMutableDictionaryRef sharegroups;      /* EAGLSharegroup -> OSMesaContext (the first of the group) */
static __thread __unsafe_unretained id current;
static Class sharegroup_class;
static int missing_said[EAGL_TABLE_SIZE / 8];

static long
eagl_missing(void *context, unsigned offset)
{
	(void)context;
	if (offset / 8 < EAGL_TABLE_SIZE / 8 && !missing_said[offset / 8]++) {
		say("redwood-eagl: %s: %s (%#x) is not in Mesa's OpenGL ES; does nothing\n", getprogname(),
		    eagl_slot_name(offset), offset);
	}
	return 0;
}

static struct app_context *
context_of(id object)
{
	struct app_context *c;

	pthread_mutex_lock(&lock);
	c = contexts != NULL ? (struct app_context *)CFDictionaryGetValue(contexts, (__bridge const void *)object) : NULL;
	pthread_mutex_unlock(&lock);
	return c;
}

static void
tsd_set(void *value)
{
	void **tsd;

	__asm__ volatile ("mrs %0, tpidrro_el0" : "=r"(tsd));
	tsd[30] = value;
}

static id
my_initWithAPI_sharegroup(id self, SEL selector, NSUInteger api, id sharegroup)
{
	struct objc_super up = { self, [NSObject class] };
	struct app_context *c;
	OSMesaContext share = NULL;
	int attributes[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
		OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, 3, OSMESA_CONTEXT_MINOR_VERSION, 0,
		0
	};

	(void)selector;
	if (api != 2 && api != 3) {
		say("redwood-eagl: %s: OpenGL ES %lu asked for; only 2 and 3 are had\n", getprogname(), (unsigned long)api);
		return nil;     /* (the caller releases self) */
	}
	self = ((id (*)(struct objc_super *, SEL))objc_msgSendSuper)(&up, @selector(init));
	if (self == nil) {
		return nil;
	}
	if ((c = calloc(1, sizeof(*c))) == NULL) {
		return nil;
	}
	pthread_mutex_lock(&lock);
	if (sharegroup != nil && sharegroups != NULL) {
		share = (OSMesaContext)CFDictionaryGetValue(sharegroups, (__bridge const void *)sharegroup);
	}
	pthread_mutex_unlock(&lock);
	c->mesa = OSMesaCreateContextAttribs(attributes, share);
	if (c->mesa == NULL) {
		say("redwood-eagl: %s: Mesa made no OpenGL ES %lu context\n", getprogname(), (unsigned long)api);
		free(c);
		return nil;
	}
	c->api = (int)api;
	c->object = self;
	eagl_fill_table(c->table);
	c->table[0] = c;
	if (sharegroup == nil) {
		sharegroup = class_createInstance(sharegroup_class, 0);         /* (never freed: a few bytes a group) */
		CFRetain((__bridge CFTypeRef)sharegroup);
	}
	c->sharegroup = sharegroup;
	pthread_mutex_lock(&lock);
	if (contexts == NULL) {
		contexts = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
		sharegroups = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
	}
	CFDictionarySetValue(contexts, (__bridge const void *)self, c);
	if (!CFDictionaryContainsKey(sharegroups, (__bridge const void *)sharegroup)) {
		CFDictionarySetValue(sharegroups, (__bridge const void *)sharegroup, c->mesa);
	}
	pthread_mutex_unlock(&lock);
	say("redwood-eagl: %s: OpenGL ES %d context %p by Mesa%s\n", getprogname(), c->api, (__bridge void *)self,
	    share != NULL ? " (sharing with its group)" : "");
	return self;
}

static id
my_initWithAPI(id self, SEL selector, NSUInteger api)
{
	return my_initWithAPI_sharegroup(self, selector, api, nil);
}

static id
my_initWithAPI_properties(id self, SEL selector, NSUInteger api, id properties)
{
	(void)properties;
	return my_initWithAPI_sharegroup(self, selector, api, nil);
}

static NSUInteger
my_API(id self, SEL selector)
{
	struct app_context *c = context_of(self);

	(void)selector;
	return c != NULL ? (NSUInteger)c->api : 0;
}

static id
my_sharegroup(id self, SEL selector)
{
	struct app_context *c = context_of(self);

	(void)selector;
	return c != NULL ? c->sharegroup : nil;
}

static BOOL
my_setCurrentContext(id class, SEL selector, id object)
{
	struct app_context *c = object != nil ? context_of(object) : NULL;

	(void)class;
	(void)selector;
	if (object != nil && c == NULL) {
		return NO;
	}
	if (c == NULL) {
		OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
		tsd_set(NULL);
		current = nil;
		return YES;
	}
	if (!OSMesaMakeCurrent(c->mesa, c->window, GL_UNSIGNED_BYTE, WINDOW, WINDOW)) {
		say("redwood-eagl: %s: context %p cannot be made current\n", getprogname(), (__bridge void *)object);
		return NO;
	}
	tsd_set(c->before);
	current = object;
	return YES;
}

static id
my_currentContext(id class, SEL selector)
{
	(void)class;
	(void)selector;
	return current;
}

static void
my_dealloc(id self, SEL selector)
{
	struct objc_super up = { self, [NSObject class] };
	struct app_context *c = context_of(self);

	if (c != NULL) {
		pthread_mutex_lock(&lock);
		CFDictionaryRemoveValue(contexts, (__bridge const void *)self);
		pthread_mutex_unlock(&lock);
		if (current == self) {
			OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
			tsd_set(NULL);
			current = nil;
		}
		for (struct layer_target *t = c->targets, *next; t != NULL; t = next) {
			next = t->next;
			for (unsigned i = 0; i < SURFACES; i++) {
				if (t->surfaces[i] != NULL) {
					CFRelease(t->surfaces[i]);
				}
			}
			free(t);
		}
		/* the group's first Mesa context is kept: the others share its objects through it */
		pthread_mutex_lock(&lock);
		bool first = CFDictionaryGetValue(sharegroups, (__bridge const void *)c->sharegroup) == c->mesa;
		pthread_mutex_unlock(&lock);
		if (!first) {
			OSMesaDestroyContext(c->mesa);
		}
		free(c);
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&up, selector);
}

/* ---- the layer --------------------------------------------------------------------------------- */

static struct layer_target *
target_of(struct app_context *c, GLuint renderbuffer)
{
	for (struct layer_target *t = c->targets; t != NULL; t = t->next) {
		if (t->renderbuffer == renderbuffer) {
			return t;
		}
	}
	return NULL;
}

static BOOL
my_renderbufferStorage(id self, SEL selector, NSUInteger target, id drawable)
{
	struct app_context *c = context_of(self);
	CALayer *layer = drawable;
	GLint renderbuffer = 0;
	struct layer_target *t;
	CGFloat scale;
	CGSize size;
	int width, height;
	GLenum format = GL_RGBA8;

	(void)selector;
	glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
	say("redwood-eagl: %s: renderbufferStorage:%#lx fromDrawable:%s (context %p%s, renderbuffer %d)\n", getprogname(),
	    (unsigned long)target, drawable != nil ? object_getClassName(drawable) : "nil", (__bridge void *)self,
	    c == NULL ? ", not ours" : "", renderbuffer);
	if (c == NULL || target != GL_RENDERBUFFER || renderbuffer == 0) {
		return NO;
	}
	if (layer == nil) {             /* "detach": the renderbuffer is no longer the layer's */
		if ((t = target_of(c, (GLuint)renderbuffer)) != NULL) {
			t->layer = nil;
		}
		return YES;
	}
	scale = layer.contentsScale;
	size = layer.bounds.size;
	width = (int)(size.width * scale + 0.5);
	height = (int)(size.height * scale + 0.5);
	if ([layer respondsToSelector:@selector(drawableProperties)]) {
		NSDictionary *properties = [(id)layer drawableProperties];
		NSString *color = properties[@"EAGLDrawablePropertyColorFormat"];

		if ([color isEqual:@"RGB565"]) {
			format = GL_RGB565;
		}
	}
	if (width <= 0 || height <= 0) {
		width = height = 1;
	}
	glRenderbufferStorage(GL_RENDERBUFFER, format, width, height);
	if ((t = target_of(c, (GLuint)renderbuffer)) == NULL) {
		t = calloc(1, sizeof(*t));
		t->renderbuffer = (GLuint)renderbuffer;
		t->next = c->targets;
		c->targets = t;
	}
	if (t->width != width || t->height != height) {
		for (unsigned i = 0; i < SURFACES; i++) {
			if (t->surfaces[i] != NULL) {
				CFRelease(t->surfaces[i]);
				t->surfaces[i] = NULL;
			}
		}
	}
	t->layer = layer;
	t->width = width;
	t->height = height;
	layer.contentsGravity = kCAGravityResize;
	say("redwood-eagl: %s: renderbuffer %d is a layer of %dx%d pixels (%gx%g points at %g)%s\n", getprogname(),
	    renderbuffer, width, height, size.width, size.height, scale, format == GL_RGB565 ? ", RGB565" : "");
	return YES;
}

static IOSurfaceRef
surface_make(int width, int height)
{
	NSDictionary *properties = @{
		(id)kIOSurfaceWidth: @(width),
		(id)kIOSurfaceHeight: @(height),
		(id)kIOSurfaceBytesPerElement: @4,
		(id)kIOSurfaceBytesPerRow: @((width * 4 + 255) & ~255),
		(id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
	};

	return IOSurfaceCreate((__bridge CFDictionaryRef)properties);
}

/*
 * The renderbuffer into the surface by the GPU, turned the right way up on the way: the surface's
 * memory is a texture (Mesa's glEGLImageTargetTexture2DOES takes an IOSurface's ID here) and a
 * framebuffer's colour; glBlitFramebuffer copies. Nothing is read by the CPU, and the surface on
 * the screen is never written.
 */
static bool
gpu_copy(struct layer_target *t, unsigned slot, GLuint renderbuffer, GLuint read_before)
{
	static void (*image_target)(GLenum target, void *image);
	GLint draw_before = 0, texture_before = 0;
	GLboolean scissor, discard;

	if (image_target == NULL) {
		image_target = (void (*)(GLenum, void *))OSMesaGetProcAddress("glEGLImageTargetTexture2DOES");
		if (image_target == NULL) {
			t->gpu = -1;
			return false;
		}
	}
	while (glGetError() != 0) {
	}
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_before);
	if (t->read_framebuffer == 0) {
		glGenFramebuffers(1, &t->read_framebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, t->read_framebuffer);
		glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
	}
	if (t->framebuffers[slot] == 0) {
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_before);
		glGenTextures(1, &t->textures[slot]);
		glBindTexture(GL_TEXTURE_2D, t->textures[slot]);
		image_target(GL_TEXTURE_2D, (void *)(uintptr_t)IOSurfaceGetID(t->surfaces[slot]));
		glBindTexture(GL_TEXTURE_2D, (GLuint)texture_before);
		glGenFramebuffers(1, &t->framebuffers[slot]);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->framebuffers[slot]);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->textures[slot], 0);
		if (glGetError() != 0 || glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			say("redwood-eagl: %s: the GPU cannot draw into the layer's surface; the CPU copies\n", getprogname());
			t->gpu = -1;
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_before);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, read_before);
			return false;
		}
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, t->read_framebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->framebuffers[slot]);
	scissor = glIsEnabled(GL_SCISSOR_TEST);
	discard = glIsEnabled(GL_RASTERIZER_DISCARD);
	if (scissor) {
		glDisable(GL_SCISSOR_TEST);
	}
	if (discard) {
		glDisable(GL_RASTERIZER_DISCARD);
	}
	glBlitFramebuffer(0, 0, t->width, t->height, 0, t->height, t->width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	if (scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	if (discard) {
		glEnable(GL_RASTERIZER_DISCARD);
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_before);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, read_before);
	if (glGetError() != 0) {
		say("redwood-eagl: %s: the GPU's copy failed; the CPU copies\n", getprogname());
		t->gpu = -1;
		return false;
	}
	glFinish();                     /* (the picture is whole before the layer has it) */
	if (t->gpu == 0) {
		t->gpu = 1;
		say("redwood-eagl: %s: frames are copied by the GPU into the layer's surfaces\n", getprogname());
	}
	return true;
}

static BOOL
present(id self, NSUInteger target)
{
	struct app_context *c = context_of(self);
	GLint renderbuffer = 0, read_before = 0, pack_buffer = 0;
	struct layer_target *t;
	IOSurfaceRef surface = NULL;
	uint8_t *base, *row;
	size_t stride;

	if (c == NULL || target != GL_RENDERBUFFER) {
		return NO;
	}
	glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
	if ((t = target_of(c, (GLuint)renderbuffer)) == NULL || t->layer == nil) {
		return NO;
	}
	/* a surface the layer is not showing any more */
	for (unsigned i = 0; i < SURFACES; i++) {
		unsigned k = (t->turn + i) % SURFACES;

		if (t->surfaces[k] == NULL) {
			t->surfaces[k] = surface_make(t->width, t->height);
		}
		if (t->surfaces[k] != NULL && (int)k != t->shown - 1 && !IOSurfaceIsInUse(t->surfaces[k])) {
			surface = t->surfaces[k];
			t->turn = k + 1;
			break;
		}
	}
	if (surface == NULL) {
		unsigned k = t->turn++ % SURFACES;

		if ((int)k == t->shown - 1) {
			k = t->turn++ % SURFACES;
		}
		surface = t->surfaces[k];
	}
	if (surface == NULL) {
		return NO;
	}
	unsigned slot = 0;
	while (slot < SURFACES && t->surfaces[slot] != surface) {
		slot++;
	}
	/* the renderbuffer, read from a framebuffer of our own */
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_before);
	if (t->gpu >= 0 && gpu_copy(t, slot, (GLuint)renderbuffer, (GLuint)read_before)) {
		goto show;
	}
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
	if (t->read_framebuffer == 0) {
		glGenFramebuffers(1, &t->read_framebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, t->read_framebuffer);
		glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, (GLuint)renderbuffer);
	} else {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, t->read_framebuffer);
	}
	if (pack_buffer != 0) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}
	IOSurfaceLock(surface, 0, NULL);
	base = IOSurfaceGetBaseAddress(surface);
	stride = IOSurfaceGetBytesPerRow(surface);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(stride / 4));
	glReadPixels(0, 0, t->width, t->height, GL_BGRA, GL_UNSIGNED_BYTE, base);
	if (glGetError() != 0) {
		glReadPixels(0, 0, t->width, t->height, GL_RGBA, GL_UNSIGNED_BYTE, base);
		/* blue first for the surface */
		for (int y = 0; y < t->height; y++) {
			uint32_t *p = (uint32_t *)(base + (size_t)y * stride);

			for (int x = 0; x < t->width; x++) {
				uint32_t v = p[x];

				p[x] = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
			}
		}
	}
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	/* OpenGL's rows go up, the surface's down */
	row = malloc(stride);
	for (int y = 0; y < t->height / 2; y++) {
		uint8_t *a = base + (size_t)y * stride, *b = base + (size_t)(t->height - 1 - y) * stride;

		memcpy(row, a, stride);
		memcpy(a, b, stride);
		memcpy(b, row, stride);
	}
	free(row);
	IOSurfaceUnlock(surface, 0, NULL);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_before);
	if (pack_buffer != 0) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer);
	}

show:;
	t->shown = (int)slot + 1;
	CALayer *layer = t->layer;
	id contents = (__bridge id)surface;

	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	layer.contents = contents;
	if ([layer respondsToSelector:@selector(setContentsChanged)]) {
		[layer setContentsChanged];
	}
	[CATransaction commit];
	if (![NSThread isMainThread]) {
		[CATransaction flush];
	}
	{
		CGRect b = layer.bounds;
		CGFloat sc = layer.contentsScale;

		if ((int)(b.size.width * sc + 0.5) != t->width || (int)(b.size.height * sc + 0.5) != t->height) {
			if (t->said_size++ < 3) {
				say("redwood-eagl: %s: the layer is now %gx%g points at %g, its renderbuffer still %dx%d\n",
				    getprogname(), b.size.width, b.size.height, sc, t->width, t->height);
			}
		}
	}
	/* a picture of this frame, when asked for: touch $TMPDIR/eagl-shot (a raw BGRA file is written beside it) */
	if (t->frames % 30 == 0) {
		NSString *ask = [NSTemporaryDirectory() stringByAppendingPathComponent:@"eagl-shot"];

		if (access(ask.fileSystemRepresentation, F_OK) == 0) {
			unlink(ask.fileSystemRepresentation);
			IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
			NSData *bytes = [NSData dataWithBytes:IOSurfaceGetBaseAddress(surface)
			    length:IOSurfaceGetBytesPerRow(surface) * IOSurfaceGetHeight(surface)];
			IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
			NSString *out = [NSTemporaryDirectory() stringByAppendingPathComponent:
			    [NSString stringWithFormat:@"eagl-shot-%dx%d-%zu.bgra", t->width, t->height, IOSurfaceGetBytesPerRow(surface)]];
			[bytes writeToFile:out atomically:NO];
			say("redwood-eagl: %s: frame written to %s\n", getprogname(), out.fileSystemRepresentation);
		}
	}
	if (t->frames++ == 0) {
		say("redwood-eagl: %s: first frame shown (%dx%d)\n", getprogname(), t->width, t->height);
	}
	return YES;
}

static BOOL
my_presentRenderbuffer(id self, SEL selector, NSUInteger target)
{
	(void)selector;
	return present(self, target);
}

static BOOL
my_presentRenderbuffer_atTime(id self, SEL selector, NSUInteger target, double time)
{
	(void)selector;
	(void)time;
	return present(self, target);
}

static BOOL
my_presentRenderbuffer_after(id self, SEL selector, NSUInteger target, double duration)
{
	(void)selector;
	(void)duration;
	return present(self, target);
}

static void
my_setter_ignored(id self, SEL selector, long value)
{
	(void)self;
	(void)selector;
	(void)value;
}

static BOOL
my_no(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return NO;
}

static id
my_nil(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return nil;
}

static long
my_unknown(id self, SEL selector)
{
	static CFMutableSetRef said;

	pthread_mutex_lock(&lock);
	if (said == NULL) {
		said = CFSetCreateMutable(NULL, 0, NULL);
	}
	bool first = !CFSetContainsValue(said, selector);
	CFSetAddValue(said, selector);
	pthread_mutex_unlock(&lock);
	if (first) {
		say("redwood-eagl: %s: -[%s %s] asked for; not had here (returns 0)\n", getprogname(), object_getClassName(self),
		    sel_getName(selector));
	}
	return 0;
}

/* ---- in ---------------------------------------------------------------------------------------- */

static void
replace(Class class, const char *name, IMP ours, const char *types)
{
	SEL selector = sel_registerName(name);
	Method method = class_getInstanceMethod(class, selector);

	if (method == NULL || method == class_getInstanceMethod(class_getSuperclass(class), selector)) {
		class_addMethod(class, selector, ours, method != NULL ? method_getTypeEncoding(method) : types);
	} else {
		method_setImplementation(method, ours);
	}
}

__attribute__((constructor))
static void
redwood_eagl_start(void)
{
	const char *off = getenv("REDWOOD_EAGL");
	Class class, meta;

	if (off != NULL && strcmp(off, "0") == 0) {
		return;
	}
	if (dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW | RTLD_NOLOAD) == NULL ||
	    (class = objc_getClass("EAGLContext")) == Nil) {
		return;                 /* not an OpenGL ES program */
	}
	sharegroup_class = objc_getClass("EAGLSharegroup");
	if (sharegroup_class == Nil) {
		sharegroup_class = [NSObject class];
	}
	meta = object_getClass(class);
	{
		/* every method not replaced below says when it is used, and does nothing */
		unsigned count = 0;
		Method *methods = class_copyMethodList(class, &count);

		for (unsigned i = 0; i < count; i++) {
			const char *name = sel_getName(method_getName(methods[i]));

			if (strncmp(name, ".cxx", 4) != 0) {
				method_setImplementation(methods[i], (IMP)my_unknown);
			}
		}
		free(methods);
		methods = class_copyMethodList(meta, &count);
		for (unsigned i = 0; i < count; i++) {
			const char *name = sel_getName(method_getName(methods[i]));

			if (strcmp(name, "initialize") != 0 && strcmp(name, "load") != 0) {
				method_setImplementation(methods[i], (IMP)my_unknown);
			}
		}
		free(methods);
	}
	replace(class, "initWithAPI:", (IMP)my_initWithAPI, "@@:Q");
	replace(class, "initWithAPI:sharegroup:", (IMP)my_initWithAPI_sharegroup, "@@:Q@");
	replace(class, "initWithAPI:properties:", (IMP)my_initWithAPI_properties, "@@:Q@");
	replace(class, "API", (IMP)my_API, "Q@:");
	replace(class, "sharegroup", (IMP)my_sharegroup, "@@:");
	replace(class, "dealloc", (IMP)my_dealloc, "v@:");
	replace(class, "renderbufferStorage:fromDrawable:", (IMP)my_renderbufferStorage, "B@:Q@");
	replace(class, "presentRenderbuffer:", (IMP)my_presentRenderbuffer, "B@:Q");
	replace(class, "presentRenderbuffer:atTime:", (IMP)my_presentRenderbuffer_atTime, "B@:Qd");
	replace(class, "presentRenderbuffer:afterMinimumDuration:", (IMP)my_presentRenderbuffer_after, "B@:Qd");
	replace(class, "setMultiThreaded:", (IMP)my_setter_ignored, "v@:B");
	replace(class, "isMultiThreaded", (IMP)my_no, "B@:");
	replace(class, "setDebugLabel:", (IMP)my_setter_ignored, "v@:@");
	replace(class, "debugLabel", (IMP)my_nil, "@@:");
	replace(meta, "setCurrentContext:", (IMP)my_setCurrentContext, "B@:@");
	replace(meta, "currentContext", (IMP)my_currentContext, "@@:");
	{
		extern const void *_dyld_get_shared_cache_range(size_t *length);
		size_t length = 0;
		const void *cache = _dyld_get_shared_cache_range(&length);

		say("redwood-eagl: %s: OpenGL ES by Mesa (EAGLContext replaced); cache slide %#lx, program at %p\n",
		    getprogname(), (unsigned long)((uintptr_t)cache - 0x180000000UL), (const void *)_dyld_get_image_header(0));
	}
}

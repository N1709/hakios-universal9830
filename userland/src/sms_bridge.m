/* MIT. RedwoodSMS: the S20's texts in the Messages app (loaded into imagent by the port's tweak loader).
 *
 * There is no CommCenter: texts travel over VoLTE (IMS) through redwood-ims. This bridge, inside imagent,
 *  - delivers received texts: /var/tmp/redwood-cell/sms-in/<id>.txt (sender, unix time, text) becomes a CTMessage
 *    handed to the SMS plugin's own -[SMSServiceSession _processReceivedSMSMessage:storageContext:withCompletionBlock:],
 *    so they land in Messages with the usual notification;
 *  - sends: -[SMSServiceSession sendCTMessageFromIMMessageItem:forChat:chat:style:] (where Messages hands an SMS to
 *    CoreTelephony) writes /var/tmp/redwood-cell/sms-out/<guid>.msg (number, text) instead; redwood-ims sends it and
 *    answers <guid>.ok / <guid>.fail, which become "Delivered" (didSendMessage:forChat:style:) or "Not Delivered".
 * Log: /var/mobile/Library/Logs/redwood-sms.log (no message text in it). Off: delete the dylib or its plist. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

static NSString *const RWIn = @"/var/tmp/redwood-cell/sms-in";
static NSString *const RWOut = @"/var/tmp/redwood-cell/sms-out";

static void rw_log(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
static void rw_log(NSString *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSString *line = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    FILE *f = fopen("/var/mobile/Library/Logs/redwood-sms.log", "a");
    if (f) { fprintf(f, "%s %s\n", [[[NSDate date] description] UTF8String], line.UTF8String); fclose(f); }
}

static id rw_session(void)
{
    Class ac = objc_getClass("IMDAccountController");
    if (!ac) return nil;
    id controller = ((id (*)(Class, SEL))objc_msgSend)(ac, sel_registerName("sharedAccountController"));
    return controller ? ((id (*)(id, SEL, id))objc_msgSend)(controller, sel_registerName("anySessionForServiceName:"), @"SMS") : nil;
}

/* ---- probe: what the SMS plugin asks of an incoming message (trigger: /var/tmp/redwood-cell/sms-probe) ---- */
@interface RWProbe : NSObject
@property (copy) NSString *tag;
@end
@implementation RWProbe
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
    NSMethodSignature *sig = [super methodSignatureForSelector:sel];
    return sig ? sig : [NSMethodSignature signatureWithObjCTypes:"@@:@@@@"];
}
- (BOOL)respondsToSelector:(SEL)sel { rw_log(@"probe %@: respondsToSelector %@", self.tag, NSStringFromSelector(sel)); return YES; }
- (void)forwardInvocation:(NSInvocation *)inv
{
    rw_log(@"probe %@: %@ (returns %s)", self.tag, NSStringFromSelector(inv.selector), inv.methodSignature.methodReturnType);
    char z[64] = {0};
    if (inv.methodSignature.methodReturnLength && inv.methodSignature.methodReturnLength <= sizeof(z)) [inv setReturnValue:z];
}
@end
/* the text part: BlastDoorSMSParserTextPart <- BlastDoorSMSCTPartContentWrapper <- the message's item */
@interface RWText : RWProbe
@property (copy) NSString *text;
@end
@implementation RWText
@end
@interface RWContent : RWProbe
@property (strong) RWText *plain;
@end
@implementation RWContent
- (unsigned long)type { rw_log(@"content: type asked"); return 1; }
- (id)smil { return nil; }
- (id)attachment { return nil; }
@end
@interface RWPart : RWProbe
@property (strong) RWContent *content;
@end
@implementation RWPart
- (NSString *)contentType { return @"text/plain"; }
- (id)contentId { return nil; }
- (id)contentLocation { return nil; }
@end
/* stands in for BlastDoorSMSMessage (what BlastDoor makes of CommCenter's dictionary); unknown questions are logged */
@interface RWSMS : RWProbe
@property (copy) NSString *GUID, *sender, *text;
@property (strong) NSDate *date;
@end
@implementation RWSMS
- (BOOL)respondsToSelector:(SEL)sel { return [super respondsToSelector:sel]; }
- (NSString *)version { return @"1"; }
- (NSArray *)items
{
    RWText *t = [RWText new]; t.tag = @"text"; t.text = self.text;
    RWContent *c = [RWContent new]; c.tag = @"content"; c.plain = t;
    RWPart *part = [RWPart new]; part.tag = @"part"; part.content = c;
    return @[part];
}
- (long)type { return 0; }
- (NSString *)countryCode { return @"in"; }
- (NSString *)serviceCenter { return nil; }
- (NSString *)contentType { return nil; }
- (NSArray *)recipients { return @[]; }
- (NSString *)subject { return nil; }
- (long)replaceMessage { return 0; }
- (long)spamCategory { return 0; }
- (BOOL)has_sendEnabled { return NO; }
- (BOOL)has_iMessageCapability { return NO; }
- (BOOL)has_replaceMessage { return NO; }
- (BOOL)has_spamCategory { return NO; }
- (BOOL)has_trackMessage { return NO; }
- (long)iMessageCapability { return 0; }
- (id)originatedDeviceEmail { return nil; }
- (id)originatedDeviceNumber { return nil; }
- (id)originatedDeviceSIM { return nil; }
- (BOOL)sendEnabled { return NO; }
- (BOOL)trackMessage { return NO; }
@end
@interface RWProbeDict : NSDictionary
@end
@implementation RWProbeDict
- (NSUInteger)count { return 1; }
- (id)objectForKey:(id)key
{
    /* answer every key with a plausible value so the reader goes on and shows the whole format */
    rw_log(@"probe dict: objectForKey %@", key);
    if ([key isEqual:@"g"]) return [[NSUUID UUID] UUIDString];
    return [NSString stringWithFormat:@"probe-%@", key];
}
- (id)valueForKey:(NSString *)key { rw_log(@"probe dict: valueForKey %@", key); return [self objectForKey:key]; }
- (NSEnumerator *)keyEnumerator { return [@[] objectEnumerator]; }
@end
static void rw_probe(void)
{
    if (![[NSFileManager defaultManager] fileExistsAtPath:@"/var/tmp/redwood-cell/sms-probe"]) return;
    [[NSFileManager defaultManager] removeItemAtPath:@"/var/tmp/redwood-cell/sms-probe" error:nil];
    id session = rw_session();
    if (!session) { rw_log(@"probe: no session"); return; }
    RWSMS *p = [RWSMS new]; p.tag = @"message"; p.GUID = [[NSUUID UUID] UUIDString]; p.sender = @"+919999999999";
    p.date = [NSDate date]; p.text = @"probe";
    @try { ((void (*)(id, SEL, id, id, id))objc_msgSend)(session, sel_registerName("_processReceivedSMSMessage:storageContext:withCompletionBlock:"), p, nil, ^(void) {}); }
    @catch (NSException *e) { rw_log(@"probe message threw %@: %@", e.name, e.reason); }
    @try { ((void (*)(id, SEL, id, id, id))objc_msgSend)(session, sel_registerName("_processReceivedDictionary:storageContext:withCompletionBlock:"), [RWProbeDict new], nil, ^(void) {}); }
    @catch (NSException *e) { rw_log(@"probe dict threw %@: %@", e.name, e.reason); }
    rw_log(@"probe done");
}

/* ---- incoming: real subclasses of BlastDoor's classes, built at run time, getters answering from associated values ---- */
static void rw_trace(id self, SEL _cmd, id v)
{   /* which getters the plugin reads (names only), while /var/tmp/redwood-cell/sms-trace exists */
    if ([[NSFileManager defaultManager] fileExistsAtPath:@"/var/tmp/redwood-cell/sms-trace"])
        rw_log(@"read %s.%s -> %@", class_getName(object_getClass(self)) + 3, sel_getName(_cmd), v ? NSStringFromClass([v class]) : @"nil");
}
static id rw_get_obj(id self, SEL _cmd) { id v = objc_getAssociatedObject(self, (const void *)_cmd); rw_trace(self, _cmd, v); return v; }
static long rw_get_int(id self, SEL _cmd) { id v = objc_getAssociatedObject(self, (const void *)_cmd); rw_trace(self, _cmd, v); return [v longValue]; }
static BOOL rw_get_bool(id self, SEL _cmd) { id v = objc_getAssociatedObject(self, (const void *)_cmd); rw_trace(self, _cmd, v); return [v boolValue]; }
static id rw_make(const char *base, NSDictionary *values)
{
    Class super = objc_getClass(base);
    if (!super) { rw_log(@"no class %s", base); return nil; }
    char name[128];snprintf(name, sizeof(name), "RW_%s", base);
    Class sub = objc_getClass(name);
    if (!sub) {
        sub = objc_allocateClassPair(super, name, 0);
        unsigned n = 0;Method *ms = class_copyMethodList(super, &n);
        for (unsigned i = 0; i < n; i++) {
            SEL sel = method_getName(ms[i]);const char *t = method_getTypeEncoding(ms[i]);
            if (method_getNumberOfArguments(ms[i]) != 2 || !t || sel == @selector(init) || sel == @selector(description) || sel_getName(sel)[0] == '.') continue;
            IMP imp = t[0] == '@' ? (IMP)rw_get_obj : t[0] == 'B' ? (IMP)rw_get_bool : (t[0] == 'q' || t[0] == 'Q') ? (IMP)rw_get_int : NULL;
            if (imp) class_addMethod(sub, sel, imp, t);
        }
        free(ms);
        objc_registerClassPair(sub);
    }
    /* alloc without init: these are Swift wrappers whose plain -init traps ("unimplemented initializer");
     * every getter is answered by us, so their own storage stays unused */
    id o = ((id (*)(Class, SEL))objc_msgSend)(sub, @selector(alloc));
    for (NSString *k in values)
        objc_setAssociatedObject(o, (const void *)NSSelectorFromString(k), values[k], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return o;
}
static id rw_sms(NSString *guid, NSString *from, NSDate *date, NSString *text)
{
    long ptype = 0;   /* BlastDoorSMSCTPartContentWrapper -type: 0 plain (1 is SMIL: the plugin then reads -smil); a file overrides */
    NSString *pt = [NSString stringWithContentsOfFile:@"/var/tmp/redwood-cell/sms-plain-type" encoding:NSUTF8StringEncoding error:nil];
    if (pt.length) ptype = pt.integerValue;
    id plain = rw_make("BlastDoorSMSCTPartPlain", @{@"plainTextContentBody": text});
    id wrap = rw_make("BlastDoorSMSCTPartContentWrapper", @{@"type": @(ptype), @"plain": plain});
    id part = rw_make("BlastDoorSMSCTPart", @{@"content": wrap, @"contentType": @"text/plain"});
    return rw_make("BlastDoorSMSMessage", @{@"GUID": guid, @"sender": from, @"date": date, @"items": @[part],
        @"version": @"1", @"countryCode": @"in", @"recipients": @[], @"type": @0});
}

/* ---- incoming ---- */
static void rw_deliver_inbox(void)
{
    /* incoming delivery is on; /var/mobile/Library/Preferences/com.redwood.sms-bridge-off turns it off (a text is taken out
     * of the inbox before it is handed over, so a crash in imagent never comes back for the same text) */
    if ([[NSFileManager defaultManager] fileExistsAtPath:@"/var/mobile/Library/Preferences/com.redwood.sms-bridge-off"]) return;
    NSArray *names = [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:RWIn error:nil]
        sortedArrayUsingSelector:@selector(compare:)];
    if (!names.count) return;
    id session = rw_session();
    if (!session) { rw_log(@"no SMS session in imagent yet; %lu text(s) wait", (unsigned long)names.count); return; }
    SEL process = sel_registerName("_processReceivedSMSMessage:storageContext:withCompletionBlock:");
    if (![session respondsToSelector:process]) { rw_log(@"SMS plugin lacks the receive entry point"); return; }
    for (NSString *name in names) {
        if (![name hasSuffix:@".txt"]) continue;
        NSString *path0 = [RWIn stringByAppendingPathComponent:name];
        /* taken out of the inbox before anything else: a crash below cannot come back for the same text */
        NSString *held = [RWIn stringByAppendingPathComponent:@"taken"];   /* inside the inbox: ours (mobile) to write */
        [[NSFileManager defaultManager] createDirectoryAtPath:held withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *path = [held stringByAppendingPathComponent:name];
        if (rename(path0.fileSystemRepresentation, path.fileSystemRepresentation)) {rw_log(@"inbox: cannot take %@ (errno %d)", name, errno);continue;}
        NSString *all = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
        NSArray *lines = [all componentsSeparatedByString:@"\n"];
        if (lines.count < 3) { [[NSFileManager defaultManager] removeItemAtPath:path error:nil]; continue; }
        NSString *from = lines[0];
        NSDate *date = [NSDate dateWithTimeIntervalSince1970:[lines[1] doubleValue]];
        NSString *text = [[lines subarrayWithRange:NSMakeRange(2, lines.count - 2)] componentsJoinedByString:@"\n"];
        /* 12 digits starting 91 = an Indian number without its plus */
        if (from.length == 12 && [from hasPrefix:@"91"] && [from rangeOfCharacterFromSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]].location == NSNotFound)
            from = [@"+" stringByAppendingString:from];
        id msg = rw_sms([[NSUUID UUID] UUIDString], from, date, text);
        if (!msg) return;
        @try {
            ((void (*)(id, SEL, id, id, id))objc_msgSend)(session, process, msg, nil, ^(void) {});
            rw_log(@"delivered a text from %@ (%lu characters) to Messages", from.length > 4 ? [from substringFromIndex:from.length - 4] : from,
                (unsigned long)text.length);
        } @catch (NSException *e) {
            rw_log(@"receive entry point threw %@: %@", e.name, e.reason);
        }
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    }
}

/* ---- outgoing ---- */
static NSMutableDictionary *rw_pending;          /* guid -> @[session, item, chat identifier, style] */
static void (*rw_orig_send)(id, SEL, id, id, id, unsigned char);

static void rw_send(id self, SEL _cmd, id item, id chatIdentifier, id chat, unsigned char style)
{
    NSString *guid = [item respondsToSelector:@selector(guid)] ? ((id (*)(id, SEL))objc_msgSend)(item, sel_registerName("guid")) : nil;
    id body = [item respondsToSelector:sel_registerName("body")] ? ((id (*)(id, SEL))objc_msgSend)(item, sel_registerName("body")) : nil;
    NSString *text = [body respondsToSelector:@selector(string)] ? [body string] : nil;
    if (!text.length && [item respondsToSelector:sel_registerName("plainBody")])
        text = ((id (*)(id, SEL))objc_msgSend)(item, sel_registerName("plainBody"));
    if (!text.length && [item respondsToSelector:sel_registerName("associatedMessageType")]) {
        /* a reaction (tapback): the text iPhones send over SMS for it, e.g. Liked "..." */
        long type = ((long (*)(id, SEL))objc_msgSend)(item, sel_registerName("associatedMessageType"));
        id info = [item respondsToSelector:sel_registerName("messageSummaryInfo")] ? ((id (*)(id, SEL))objc_msgSend)(item, sel_registerName("messageSummaryInfo")) : nil;
        Class tb = objc_getClass("IMTapback");
        SEL make = sel_registerName("tapbackWithAssociatedMessageType:messageSummaryInfo:");
        if (type && tb && [tb respondsToSelector:make]) {
            id tap = ((id (*)(Class, SEL, long, id))objc_msgSend)(tb, make, type, info);
            SEL str = sel_registerName("backwardCompatibilityStringWithMessageSummaryInfo:");
            if ([tap respondsToSelector:str]) text = ((id (*)(id, SEL, id))objc_msgSend)(tap, str, info);
            rw_log(@"send: reaction (type %ld) as text: %s", type, text.length ? "yes" : "no text");
        }
    }
    NSString *to = [chatIdentifier isKindOfClass:NSString.class] ? chatIdentifier : nil;
    if (!guid || !text.length || !to.length) {
        rw_log(@"send: not a plain text (guid %d text %d to %d); left to the original path", guid != nil, text.length > 0, to.length > 0);
        rw_orig_send(self, _cmd, item, chatIdentifier, chat, style);
        return;
    }
    [[NSFileManager defaultManager] createDirectoryAtPath:RWOut withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *tmp = [RWOut stringByAppendingPathComponent:[guid stringByAppendingString:@".tmp"]];
    NSString *msg = [RWOut stringByAppendingPathComponent:[guid stringByAppendingString:@".msg"]];
    [[NSString stringWithFormat:@"%@\n%@", to, text] writeToFile:tmp atomically:NO encoding:NSUTF8StringEncoding error:nil];
    rename(tmp.fileSystemRepresentation, msg.fileSystemRepresentation);
    @synchronized (rw_pending) { rw_pending[guid] = @[self, item, to, @(style), [NSDate date]]; }
    rw_log(@"send: queued for VoLTE (%lu characters)", (unsigned long)text.length);
}

static void rw_check_results(void)
{
    NSDictionary *copy;
    @synchronized (rw_pending) { copy = [rw_pending copy]; }
    for (NSString *guid in copy) {
        NSArray *p = copy[guid];
        NSString *ok = [RWOut stringByAppendingPathComponent:[guid stringByAppendingString:@".ok"]];
        NSString *fail = [RWOut stringByAppendingPathComponent:[guid stringByAppendingString:@".fail"]];
        BOOL sent = [[NSFileManager defaultManager] fileExistsAtPath:ok];
        BOOL failed = [[NSFileManager defaultManager] fileExistsAtPath:fail] || -[p[4] timeIntervalSinceNow] > 180;   /* (redwood-ims renews a dead registration and sends once more: up to ~100 s) */
        if (!sent && !failed) continue;
        id session = p[0], item = p[1];
        if (sent) {
            /* keep the message's own time: the confirmation can come after a reply (texting yourself), which must sort below */
            id when = [item respondsToSelector:sel_registerName("time")] ? ((id (*)(id, SEL))objc_msgSend)(item, sel_registerName("time")) : nil;
            SEL forced = sel_registerName("didSendMessage:forChat:style:forceDate:");
            if (when && [session respondsToSelector:forced])
                ((void (*)(id, SEL, id, id, unsigned char, id))objc_msgSend)(session, forced, item, p[2], (unsigned char)[p[3] unsignedCharValue], when);
            else
                ((void (*)(id, SEL, id, id, unsigned char))objc_msgSend)(session, sel_registerName("didSendMessage:forChat:style:"),
                    item, p[2], (unsigned char)[p[3] unsignedCharValue]);
            rw_log(@"send: delivered to the SMSC");
        } else {
            ((void (*)(id, SEL, unsigned int, id, BOOL))objc_msgSend)(session, sel_registerName("didReceiveError:forMessageID:forceError:"),
                4, guid, YES);
            rw_log(@"send: failed");
        }
        [[NSFileManager defaultManager] removeItemAtPath:ok error:nil];
        [[NSFileManager defaultManager] removeItemAtPath:fail error:nil];
        @synchronized (rw_pending) { [rw_pending removeObjectForKey:guid]; }
    }
}

static void rw_work(void)
{
    @autoreleasepool {
        @try { rw_probe(); rw_deliver_inbox(); rw_check_results(); }
        @catch (NSException *e) { rw_log(@"tick threw %@: %@", e.name, e.reason); }
    }
}

/* Texts in and send results are picked up the moment their file appears (a vnode watch on each folder); the timer
 * is only the fallback, e.g. when a folder is recreated (A, 2026-11-15: was a 2 s poll, up to 2 s each way). */
static void rw_watch(NSString *dir)
{
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    int fd = open(dir.fileSystemRepresentation, O_EVTONLY);
    if (fd < 0) { rw_log(@"watch %@: %s (timer only)", dir.lastPathComponent, strerror(errno)); return; }
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, (uintptr_t)fd,
        DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME, dispatch_get_main_queue());
    if (!src) { close(fd); return; }
    dispatch_source_set_event_handler(src, ^{
        unsigned long f = dispatch_source_get_data(src);
        rw_work();
        if (f & (DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME)) {   /* the folder itself went: watch the new one */
            dispatch_source_cancel(src);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{ rw_watch(dir); });
        }
    });
    dispatch_source_set_cancel_handler(src, ^{ close(fd); });
    dispatch_resume(src);
}

static void rw_tick(void *ctx)
{
    (void)ctx;
    rw_work();
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, rw_tick);
}

static void rw_install(void *ctx)
{
    (void)ctx;
    Class c = objc_getClass("SMSServiceSession");
    Method m = c ? class_getInstanceMethod(c, sel_registerName("sendCTMessageFromIMMessageItem:forChat:chat:style:")) : NULL;
    if (m && !rw_orig_send) {
        rw_orig_send = (void (*)(id, SEL, id, id, id, unsigned char))method_setImplementation(m, (IMP)rw_send);
        rw_log(@"send hook installed");
    } else if (!m) {
        rw_log(@"SMSServiceSession not loaded yet; retrying");
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, rw_install);
        return;
    }
    rw_watch(RWIn);
    rw_watch(RWOut);
    rw_tick(NULL);
}

__attribute__((constructor)) static void rw_init(void)
{
    if (strcmp(getprogname(), "imagent")) return;
    rw_pending = [NSMutableDictionary dictionary];
    rw_log(@"RedwoodSMS loaded in imagent (pid %d)", getpid());
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, rw_install);
}

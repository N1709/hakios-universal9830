/*
 * redwood-telephonyd (B, 2026-11-15): the phone's telephony call provider -- the part of Apple's CommCenter that iOS's
 * call system talks to. It registers with CallKit under CommCenter's identity (application-identifier
 * com.apple.coretelephony + com.apple.callkit private-provider-api), so callservicesd lists it as THE telephony
 * provider ("com.apple.coretelephony telephony=1 system=1", checked with proposals/native-calls/provprobe): the Phone
 * app, Contacts, Recents, Siri and the call screen use it exactly as on an iPhone -- no app of ours, no dial redirect,
 * no "isTelephonyProvider" spoofing. (It replaces RedwoodPhone.app, the third-party CallKit app of 2026-11-14.)
 *
 * The network side is redwood-ims (tools/cellular/ims-call.h); the two talk through /var/tmp/redwood-cell/call-ui/:
 *   calls   redwood-ims's snapshot of every call (read here every 150 ms; level-triggered: whatever was missed, the
 *           next snapshot says how things are, and a restart of either side ends the calls the other no longer has)
 *   cmd.*   our commands (dial/answer/end/hold/dtmf/mute), one per file, named in order
 *   alive   touched here every few seconds (without it redwood-ims answers incoming calls 486)
 * Actions are fulfilled when the network has done them (answer: when the call connects; hold: when the re-INVITE is
 * accepted), so the call screen never shows a state the network does not have.
 * Log: /var/mobile/Library/Logs/redwood-telephonyd.log (no phone numbers).
 */
#import <Foundation/Foundation.h>
#import <CallKit/CallKit.h>
#import <AVFoundation/AVFoundation.h>
#import <objc/message.h>
#include <sys/stat.h>

static NSString *const Dir = @"/var/tmp/redwood-cell/call-ui";

static void tlog(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
static void tlog(NSString *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    static NSDateFormatter *df;
    if (!df) { df = [NSDateFormatter new]; df.dateFormat = @"HH:mm:ss.SSS"; }
    FILE *f = fopen("/var/mobile/Library/Logs/redwood-telephonyd.log", "a");
    if (f) { fprintf(f, "%s %s\n", [[df stringFromDate:[NSDate date]] UTF8String], s.UTF8String); fclose(f); }
}

static void send_cmd(NSString *line, BOOL quiet)
{
    static unsigned seq;
    NSString *fn = [Dir stringByAppendingFormat:@"/cmd.%013.0f.%04u", [[NSDate date] timeIntervalSince1970] * 1000, (++seq) % 10000];
    NSString *tmp = [fn stringByAppendingString:@".tmp"];
    [[line stringByAppendingString:@"\n"] writeToFile:tmp atomically:NO encoding:NSUTF8StringEncoding error:nil];
    rename(tmp.fileSystemRepresentation, fn.fileSystemRepresentation);
    tlog(@"-> %@", quiet ? [[line componentsSeparatedByString:@" "] firstObject] : line);
}

/* one call as redwood-ims last described it */
@interface RWCall : NSObject
@property unsigned cid;
@property BOOL outgoing;
@property (copy) NSString *state, *number, *tag, *reason;
@property int hold;
@end
@implementation RWCall
@end

@interface TD : NSObject <CXProviderDelegate, CXCallObserverDelegate>
@property (strong) CXProvider *provider;
@property (strong) CXCallObserver *observer;
@property (strong) NSMutableDictionary<NSNumber *, NSUUID *> *uuids;       /* call id -> CallKit UUID */
@property (strong) NSMutableDictionary<NSString *, NSUUID *> *dialing;     /* tag -> UUID until redwood-ims names it */
@property (strong) NSMutableDictionary<NSString *, NSDate *> *dialAt;
@property (strong) NSMutableSet<NSUUID *> *finished;                       /* reported ended (or ended by the user) */
@property (strong) NSMutableSet<NSUUID *> *connectingReported, *connectedReported;
@property (strong) NSMutableDictionary<NSUUID *, CXAnswerCallAction *> *answers;
@property (strong) NSMutableDictionary<NSUUID *, CXSetHeldCallAction *> *holds;
@property (strong) NSMutableDictionary<NSUUID *, NSDate *> *actionAt;
@property (strong) NSMutableDictionary<NSNumber *, RWCall *> *last;        /* the previous snapshot's calls */
@property int imsPid;
@property BOOL registered;
@property (copy) NSString *lastText;
@property (strong) NSDate *lastFresh;
@end

static int td_registered;

@implementation TD

- (instancetype)init
{
    if (!(self = [super init])) return nil;
    CXProviderConfiguration *c = [[CXProviderConfiguration alloc] init];
    if ([c respondsToSelector:@selector(setLocalizedName:)]) [c setValue:@"Phone" forKey:@"localizedName"];
    c.supportsVideo = NO;
    c.maximumCallGroups = 2;                        /* an active call and a held (or waiting) one */
    c.maximumCallsPerCallGroup = 1;                 /* (no conference yet) */
    c.includesCallsInRecents = YES;
    c.supportedHandleTypes = [NSSet setWithObject:@(CXHandleTypePhoneNumber)];
    @try { [c setValue:@YES forKey:@"supportsAudioOnly"]; [c setValue:@YES forKey:@"supportsRinging"]; } @catch (NSException *e) {}
    _provider = [[CXProvider alloc] initWithConfiguration:c];
    [_provider setDelegate:self queue:nil];
    _observer = [CXCallObserver new];
    [_observer setDelegate:self queue:nil];
    _uuids = [NSMutableDictionary new]; _dialing = [NSMutableDictionary new]; _dialAt = [NSMutableDictionary new];
    _finished = [NSMutableSet new]; _connectingReported = [NSMutableSet new]; _connectedReported = [NSMutableSet new];
    _answers = [NSMutableDictionary new]; _holds = [NSMutableDictionary new]; _actionAt = [NSMutableDictionary new];
    _last = [NSMutableDictionary new];
    [[NSFileManager defaultManager] createDirectoryAtPath:Dir withIntermediateDirectories:YES attributes:nil error:nil];
    return self;
}

- (NSNumber *)idFor:(NSUUID *)u
{
    for (NSNumber *k in self.uuids) if ([self.uuids[k] isEqual:u]) return k;
    return nil;
}
- (BOOL)isRegistered
{
    return self.registered && [[NSFileManager defaultManager] fileExistsAtPath:@"/var/tmp/redwood-cell/ims-registered"];
}
static NSString *iso_country(void)
{   /* the SIM's country, for number formatting on the call screen (redwood-ims writes no IMSI where mobile reads) */
    NSString *mcc = [NSString stringWithContentsOfFile:@"/var/tmp/redwood-cell/call-ui/mcc" encoding:NSUTF8StringEncoding error:nil];
    int m = mcc.intValue;
    return m == 404 || m == 405 || m == 406 ? @"in" : nil;
}
- (CXCallUpdate *)updateFor:(RWCall *)c
{
    CXCallUpdate *u = [CXCallUpdate new];
    if (c.number.length && ![c.number isEqualToString:@"-"])
        u.remoteHandle = [[CXHandle alloc] initWithType:CXHandleTypePhoneNumber value:c.number];
    u.hasVideo = NO;
    u.supportsHolding = YES;
    u.supportsGrouping = NO;
    u.supportsUngrouping = NO;
    u.supportsDTMF = YES;
    @try {
        [u setValue:@YES forKey:@"supportsAddCall"];
        NSString *iso = iso_country(); if (iso) [u setValue:iso forKey:@"ISOCountryCode"];
    } @catch (NSException *e) {}
    return u;
}
static CXCallEndedReason ended_reason(RWCall *c)
{
    NSString *r = c.reason ?: @"";
    if ([r isEqualToString:@"remote"]) return CXCallEndedReasonRemoteEnded;
    if ([r isEqualToString:@"missed"]) return CXCallEndedReasonUnanswered;
    if ([r isEqualToString:@"answered-elsewhere"]) return CXCallEndedReasonAnsweredElsewhere;
    if ([r isEqualToString:@"declined-elsewhere"]) return CXCallEndedReasonDeclinedElsewhere;
    if ([r isEqualToString:@"no-answer"]) return CXCallEndedReasonUnanswered;
    if ([r isEqualToString:@"local"] || [r isEqualToString:@"declined"]) return CXCallEndedReasonRemoteEnded;  /* (ours: already ended in iOS) */
    return CXCallEndedReasonFailed;                 /* busy, invalid number, network, ... */
}

- (void)endUUID:(NSUUID *)u reason:(CXCallEndedReason)r why:(NSString *)why
{
    if (!u || [self.finished containsObject:u]) return;
    /* (bounded: an ended call only needs guarding against a second end report for a while) */
    if (self.finished.count > 64) [self.finished removeAllObjects];
    [self.finished addObject:u];
    [self.connectingReported removeObject:u]; [self.connectedReported removeObject:u];
    CXAnswerCallAction *a = self.answers[u]; if (a) { [a fail]; [self.answers removeObjectForKey:u]; }
    CXSetHeldCallAction *h = self.holds[u]; if (h) { [h fail]; [self.holds removeObjectForKey:u]; }
    tlog(@"call ended (%@) -> reason %ld", why, (long)r);
    [self.provider reportCallWithUUID:u endedAtDate:[NSDate date] reason:r];
}

/* ---- the snapshot ---- */
- (void)tick
{
    static NSDate *alive;
    if (!alive || -[alive timeIntervalSinceNow] > 4) {
        alive = [NSDate date];
        NSString *a = [Dir stringByAppendingString:@"/alive"];
        if (![[NSFileManager defaultManager] fileExistsAtPath:a]) [@"" writeToFile:a atomically:NO encoding:NSUTF8StringEncoding error:nil];
        [[NSFileManager defaultManager] setAttributes:@{NSFileModificationDate: [NSDate date]} ofItemAtPath:a error:nil];
    }
    [self timeouts];
    /* (cheap path, 2026-11-15) the list changes every ~2 s and this runs 7 times a second: unchanged file (same mtime
     * and size) -> only the staleness check on what was read last, no read or parse */
    static struct timespec seen_m; static off_t seen_n = -1; static int seen_pid; static long seen_t;
    struct stat cst;
    NSString *cpath = [Dir stringByAppendingString:@"/calls"];
    if (!stat(cpath.UTF8String, &cst) && cst.st_size == seen_n && cst.st_mtimespec.tv_sec == seen_m.tv_sec &&
        cst.st_mtimespec.tv_nsec == seen_m.tv_nsec && self.lastText) {
        if (time(NULL) - seen_t > 12 || (seen_pid > 0 && kill(seen_pid, 0) != 0 && errno == ESRCH)) [self imsGone:@"redwood-ims stopped"];
        return;
    }
    NSString *text = [NSString stringWithContentsOfFile:cpath encoding:NSUTF8StringEncoding error:nil];
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    NSArray<NSString *> *head = lines.count ? [lines[0] componentsSeparatedByString:@" "] : @[];
    if (head.count < 5 || ![head[0] isEqualToString:@"ims"]) { [self imsGone:@"no snapshot"]; return; }
    int pid = head[1].intValue; long t = head[2].longLongValue;
    seen_m = cst.st_mtimespec; seen_n = cst.st_size; seen_pid = pid; seen_t = t;
    self.registered = head[4].intValue != 0;
    /* redwood-ims rewrites it every 2 s: older than 12 s, or its process gone, means no calls exist at the network side
     * any more as far as anybody can act on them */
    if (time(NULL) - t > 12 || (pid > 0 && kill(pid, 0) != 0 && errno == ESRCH)) { [self imsGone:@"redwood-ims stopped"]; return; }
    if (self.imsPid && pid != self.imsPid) [self imsGone:@"redwood-ims restarted"];
    self.imsPid = pid;
    if ([text isEqualToString:self.lastText]) return;
    self.lastText = text;
    NSMutableDictionary<NSNumber *, RWCall *> *now = [NSMutableDictionary new];
    for (NSString *l in lines) {
        NSArray<NSString *> *w = [l componentsSeparatedByString:@" "];
        if (w.count < 8 || ![w[0] isEqualToString:@"call"]) continue;
        RWCall *c = [RWCall new];
        c.cid = (unsigned)w[1].longLongValue; c.outgoing = [w[2] isEqualToString:@"out"]; c.state = w[3]; c.hold = w[4].intValue;
        c.number = w[5]; c.tag = w[6]; c.reason = w[7];
        now[@(c.cid)] = c;
    }
    for (NSNumber *k in now) [self apply:now[k]];
    /* calls we know that redwood-ims no longer lists */
    for (NSNumber *k in [self.uuids allKeys]) {
        if (now[k]) continue;
        NSUUID *u = self.uuids[k];
        [self endUUID:u reason:CXCallEndedReasonFailed why:@"no longer listed by redwood-ims"];
        [self.uuids removeObjectForKey:k];
    }
    self.last = now;
}

- (void)imsGone:(NSString *)why
{
    if (self.uuids.count || self.dialing.count) tlog(@"%@: ending every call", why);
    for (NSNumber *k in [self.uuids allKeys]) [self endUUID:self.uuids[k] reason:CXCallEndedReasonFailed why:why];
    for (NSString *t in [self.dialing allKeys]) [self endUUID:self.dialing[t] reason:CXCallEndedReasonFailed why:why];
    [self.uuids removeAllObjects]; [self.dialing removeAllObjects]; [self.dialAt removeAllObjects];
    self.imsPid = 0; self.lastText = nil; self.registered = NO;
}

- (void)apply:(RWCall *)c
{
    NSNumber *k = @(c.cid);
    NSUUID *u = self.uuids[k];
    BOOL ended = [c.state isEqualToString:@"ended"];
    if (!u && c.outgoing && ![c.tag isEqualToString:@"-"] && self.dialing[c.tag]) {
        u = self.dialing[c.tag];                    /* our dial got its number */
        self.uuids[k] = u;
        [self.dialing removeObjectForKey:c.tag]; [self.dialAt removeObjectForKey:c.tag];
        tlog(@"dial %@ is call %u", c.tag, c.cid);
    }
    if (!u) {
        if (!c.outgoing && [c.state isEqualToString:@"ringing"]) {
            u = [NSUUID UUID];
            self.uuids[k] = u;
            NSUInteger live = 0; for (NSUUID *x in self.uuids.allValues) if (![self.finished containsObject:x]) live++;
            tlog(@"call %u: incoming%@", c.cid, live > 1 ? @" (call waiting)" : @"");
            unsigned cid = c.cid;
            [self.provider reportNewIncomingCallWithUUID:u update:[self updateFor:c] completion:^(NSError *e) {
                if (!e) return;
                /* blocked, Do Not Disturb, Silence Unknown Callers, too many calls: declined at the network too */
                tlog(@"call %u: iOS did not ring it (%@): declining", cid, e.localizedDescription);
                [self.finished addObject:u];
                send_cmd([NSString stringWithFormat:@"end %u", cid], NO);
            }];
        }
        return;                                     /* (an ended or foreign call we never had) */
    }
    if (ended) {
        [self endUUID:u reason:ended_reason(c) why:c.reason];
        return;
    }
    if (c.outgoing) {
        if (![self.connectingReported containsObject:u]) {
            [self.connectingReported addObject:u];
            [self.provider reportOutgoingCallWithUUID:u startedConnectingAtDate:[NSDate date]];
        }
        if ([c.state isEqualToString:@"active"] && ![self.connectedReported containsObject:u]) {
            [self.connectedReported addObject:u];
            [self.provider reportOutgoingCallWithUUID:u connectedAtDate:[NSDate date]];
            tlog(@"call %u: connected", c.cid);
        }
    } else if ([c.state isEqualToString:@"connecting"] || [c.state isEqualToString:@"active"]) {
        CXAnswerCallAction *a = self.answers[u];
        if (a) { [a fulfillWithDateConnected:[NSDate date]]; [self.answers removeObjectForKey:u]; tlog(@"call %u: answered", c.cid); }
    }
    CXSetHeldCallAction *h = self.holds[u];
    if (h && (((c.hold & 1) != 0) == h.onHold)) {
        [h fulfill]; [self.holds removeObjectForKey:u];
        tlog(@"call %u: %@", c.cid, h.onHold ? @"held" : @"resumed");
    }
    RWCall *was = self.last[k];
    if (was && (was.hold & 2) != (c.hold & 2)) tlog(@"call %u: the other side %@", c.cid, (c.hold & 2) ? @"holds" : @"resumed");
}

/* actions the network never confirmed */
- (void)timeouts
{
    NSDate *now = [NSDate date];
    for (NSUUID *u in [self.answers allKeys]) if ([now timeIntervalSinceDate:self.actionAt[u]] > 12) {
        tlog(@"answer not confirmed in 12 s: failed"); [self.answers[u] fail]; [self.answers removeObjectForKey:u];
        NSNumber *k = [self idFor:u]; if (k) send_cmd([NSString stringWithFormat:@"end %@", k], NO);
    }
    for (NSUUID *u in [self.holds allKeys]) if ([now timeIntervalSinceDate:self.actionAt[u]] > 12) {
        tlog(@"hold not confirmed in 12 s: failed"); [self.holds[u] fail]; [self.holds removeObjectForKey:u];
    }
    for (NSString *t in [self.dialAt allKeys]) if ([now timeIntervalSinceDate:self.dialAt[t]] > 10) {
        tlog(@"dial %@ never reached redwood-ims: failed", t);
        [self endUUID:self.dialing[t] reason:CXCallEndedReasonFailed why:@"dial not taken"];
        [self.dialing removeObjectForKey:t]; [self.dialAt removeObjectForKey:t];
    }
}

/* ---- CallKit ---- */
- (void)providerDidBegin:(CXProvider *)p { td_registered = 1; tlog(@"provider registered as the telephony provider"); }
- (void)providerDidReset:(CXProvider *)p
{
    tlog(@"provider reset by iOS: ending every call");
    for (NSNumber *k in self.uuids) if (![self.finished containsObject:self.uuids[k]]) send_cmd([NSString stringWithFormat:@"end %@", k], NO);
    for (NSString *t in self.dialing) send_cmd([NSString stringWithFormat:@"cancel %@", t], NO);
    [self.finished addObjectsFromArray:self.uuids.allValues];
    [self.uuids removeAllObjects]; [self.dialing removeAllObjects]; [self.dialAt removeAllObjects];
    [self.answers removeAllObjects]; [self.holds removeAllObjects];
}
- (void)provider:(CXProvider *)p performStartCallAction:(CXStartCallAction *)a
{
    NSString *num = [[a.handle.value componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] componentsJoinedByString:@""];
    if (![self isRegistered]) { tlog(@"dial refused: VoLTE is not registered"); [a fail]; return; }
    if (!num.length) { [a fail]; return; }
    NSString *tag = [[a.callUUID UUIDString] substringToIndex:8];
    self.dialing[tag] = a.callUUID; self.dialAt[tag] = [NSDate date];
    send_cmd([NSString stringWithFormat:@"dial %@ %@", tag, num], YES);
    [a fulfillWithDateStarted:[NSDate date]];
    RWCall *c = [RWCall new]; c.number = num;
    [p reportCallWithUUID:a.callUUID updated:[self updateFor:c]];
}
- (void)provider:(CXProvider *)p performAnswerCallAction:(CXAnswerCallAction *)a
{
    NSNumber *k = [self idFor:a.callUUID];
    if (!k) { [a fail]; return; }
    self.answers[a.callUUID] = a; self.actionAt[a.callUUID] = [NSDate date];
    send_cmd([NSString stringWithFormat:@"answer %@", k], NO);
}
- (void)provider:(CXProvider *)p performEndCallAction:(CXEndCallAction *)a
{
    NSNumber *k = [self idFor:a.callUUID];
    [self.finished addObject:a.callUUID];
    CXAnswerCallAction *pend = self.answers[a.callUUID]; if (pend) { [pend fail]; [self.answers removeObjectForKey:a.callUUID]; }
    if (k) send_cmd([NSString stringWithFormat:@"end %@", k], NO);
    else for (NSString *t in [self.dialing allKeysForObject:a.callUUID]) {
        send_cmd([NSString stringWithFormat:@"cancel %@", t], NO);
        [self.dialing removeObjectForKey:t]; [self.dialAt removeObjectForKey:t];
    }
    [a fulfillWithDateEnded:[NSDate date]];
}
- (void)provider:(CXProvider *)p performSetHeldCallAction:(CXSetHeldCallAction *)a
{
    NSNumber *k = [self idFor:a.callUUID];
    if (!k) { [a fail]; return; }
    RWCall *c = self.last[k];
    if (c && ((c.hold & 1) != 0) == a.onHold) { [a fulfill]; return; }
    self.holds[a.callUUID] = a; self.actionAt[a.callUUID] = [NSDate date];
    send_cmd([NSString stringWithFormat:@"hold %@ %d", k, a.onHold ? 1 : 0], NO);
}
- (void)provider:(CXProvider *)p performSetMutedCallAction:(CXSetMutedCallAction *)a
{
    send_cmd([NSString stringWithFormat:@"mute %d", a.muted ? 1 : 0], NO);
    [a fulfill];
}
- (void)provider:(CXProvider *)p performPlayDTMFCallAction:(CXPlayDTMFCallAction *)a
{
    NSNumber *k = [self idFor:a.callUUID];
    NSString *d = [[a.digits componentsSeparatedByCharactersInSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789*#ABCDabcd"] invertedSet]] componentsJoinedByString:@""];
    if (k && d.length) send_cmd([NSString stringWithFormat:@"dtmf %@ %@", k, d], YES);
    [a fulfill];
}
- (void)provider:(CXProvider *)p performSetGroupCallAction:(CXSetGroupCallAction *)a { tlog(@"merge asked: not supported yet"); [a fail]; }
- (void)provider:(CXProvider *)p timedOutPerformingAction:(CXAction *)a { tlog(@"iOS timed out an action: %@", NSStringFromClass([a class])); }
- (void)provider:(CXProvider *)p didActivateAudioSession:(AVAudioSession *)s { tlog(@"call audio session on"); }
- (void)provider:(CXProvider *)p didDeactivateAudioSession:(AVAudioSession *)s { tlog(@"call audio session off"); }

/* safety net: a call iOS has ended must not stay up at the network */
- (void)callObserver:(CXCallObserver *)o callChanged:(CXCall *)c
{
    if (!c.hasEnded || [self.finished containsObject:c.UUID]) return;
    NSNumber *k = [self idFor:c.UUID];
    if (!k) return;
    [self.finished addObject:c.UUID];
    tlog(@"call %@ ended by iOS without an end action: hanging up", k);
    send_cmd([NSString stringWithFormat:@"end %@", k], NO);
}
@end

/* CallKit must accept us as the telephony provider within 30 s; if not, exit non-zero and launchd starts us again
 * (KeepAlive SuccessfulExit false, ThrottleInterval 3) -- a plain retry, there is no other provider to fall back to */
static void td_watchdog(void)
{
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (td_registered) return;
        tlog(@"not registered as the telephony provider after 30 s: starting again");
        exit(1);
    });
}

int main(void)
{
    @autoreleasepool {
        tlog(@"redwood-telephonyd starting (pid %d)", getpid());
        td_watchdog();
        static TD *td; td = [TD new];
        [NSTimer scheduledTimerWithTimeInterval:0.15 repeats:YES block:^(NSTimer *t) { [td tick]; }];
        [[NSRunLoop mainRunLoop] run];
    }
    return 0;
}

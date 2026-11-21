/* callctl (B, 2026-11-15): drive calls the way the Phone app does, through TelephonyUtilities (callservicesd), for tests
 *   callctl list | dial NUMBER | answer | end [all] | hold | unhold | swap | dtmf DIGITS | providers
 * Prints each call's status (TUCallStatus: 1 active, 2 held, 3 sending, 4 ringing, 5 disconnecting, 6 idle). */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <dlfcn.h>
#define S(o,n) ((id(*)(id,SEL))objc_msgSend)(o, NSSelectorFromString(@n))
#define S1(o,n,a) ((id(*)(id,SEL,id))objc_msgSend)(o, NSSelectorFromString(@n), a)
static int st(id c) { return ((int(*)(id,SEL))objc_msgSend)(c, NSSelectorFromString(@"status")); }
static void list(id cc)
{
    for (id c in S(cc, "currentCalls"))
        printf("call status=%d outgoing=%d held=%d provider=%s telephony=%d\n", st(c),
            ((BOOL(*)(id,SEL))objc_msgSend)(c, NSSelectorFromString(@"isOutgoing")),
            ((BOOL(*)(id,SEL))objc_msgSend)(c, NSSelectorFromString(@"isOnHold")),
            [S(S(c, "provider"), "identifier") UTF8String],
            ((BOOL(*)(id,SEL))objc_msgSend)(S(c, "provider"), NSSelectorFromString(@"isTelephonyProvider")));
}
int main(int argc, char **argv) { @autoreleasepool {
    dlopen("/System/Library/PrivateFrameworks/TelephonyUtilities.framework/TelephonyUtilities", RTLD_NOW);
    id cc = S(NSClassFromString(@"TUCallCenter"), "sharedInstance");
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.5]];
    NSString *cmd = argc > 1 ? @(argv[1]) : @"list";
    NSArray *calls = S(cc, "currentCalls");
    id ringing = nil, active = nil, held = nil;
    for (id c in calls) { int s = st(c); if (s == 4 && !((BOOL(*)(id,SEL))objc_msgSend)(c, NSSelectorFromString(@"isOutgoing"))) ringing = c; else if (s == 1 || s == 3) active = c; else if (s == 2) held = c; }
    if ([cmd isEqualToString:@"dial"] && argc > 2) {
        id prov = S(S(cc, "providerManager"), "telephonyProvider");
        id req = S1([NSClassFromString(@"TUDialRequest") alloc], "initWithProvider:", prov);
        id h = S1(NSClassFromString(@"TUHandle"), "normalizedPhoneNumberHandleForValue:isoCountryCode:", @(argv[2]));
        if (!h) h = ((id(*)(id,SEL,id,id))objc_msgSend)(NSClassFromString(@"TUHandle"), NSSelectorFromString(@"normalizedPhoneNumberHandleForValue:isoCountryCode:"), @(argv[2]), @"in");
        S1(req, "setHandle:", h);
        id call = S1(cc, "dialWithRequest:", req);
        printf("dial -> %s\n", call ? "started" : "refused");
    } else if ([cmd isEqualToString:@"answer"] && ringing) { S1(cc, "answerCall:", ringing); printf("answer\n"); }
    else if ([cmd isEqualToString:@"end"]) {
        if (argc > 2) S(cc, "disconnectAllCalls");
        else { id c = ringing ?: active ?: held; if (c) S1(cc, "disconnectCall:", c); printf("end %s\n", c ? "sent" : "(no call)"); }
    }
    else if ([cmd isEqualToString:@"hold"] && active) { S1(cc, "holdCall:", active); printf("hold\n"); }
    else if ([cmd isEqualToString:@"unhold"] && held) { S1(cc, "unholdCall:", held); printf("unhold\n"); }
    else if ([cmd isEqualToString:@"swap"]) { S(cc, "swapCalls"); printf("swap\n"); }
    else if ([cmd isEqualToString:@"dtmf"] && argc > 2 && active) {
        for (const char *d = argv[2]; *d; d++) { ((void(*)(id,SEL,unsigned char))objc_msgSend)(active, NSSelectorFromString(@"playDTMFToneForKey:"), (unsigned char)(*d=='*'?10:*d=='#'?11:*d-'0')); usleep(300000); }
        printf("dtmf\n");
    }
    else if ([cmd isEqualToString:@"providers"]) {
        for (id p in S(S(cc, "providerManager"), "providers")) printf("%s telephony=%d\n", [S(p, "identifier") UTF8String], ((BOOL(*)(id,SEL))objc_msgSend)(p, NSSelectorFromString(@"isTelephonyProvider")));
    }
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
    list(cc);
} return 0; }

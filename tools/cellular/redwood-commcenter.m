/* MIT. redwood-commcenter: stands in for Apple's CommCenter (absent: there is no Apple baseband) on the
 * S20 port. Claims com.apple.commcenter.coretelephony.xpc, the NSXPC service CoreTelephonyClient talks
 * to, and answers from redwood-cellulard's state (/var/tmp/redwood-cell/state).
 *
 * Every method of CTXPCServiceInterface arrives here through -forwardInvocation:. Methods we know get
 * real answers (table below); every other one gets its reply block called with zeros/nil, so no client
 * waits forever, and is logged once (/var/tmp/redwood-commcenter.log) -- that log is the to-do list.
 * Kill switch: /var/root/cell-nocommcenter (exits at start). */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <sys/stat.h>

extern const char *_Block_signature(void *);
struct blk {void *isa;int flags,reserved;void (*invoke)(void *,...);};

static Protocol *svc_proto;
static NSMutableSet *seen;

static NSDictionary *cell_state(void)
{
    NSString *s=[NSString stringWithContentsOfFile:@"/var/tmp/redwood-cell/state" encoding:NSUTF8StringEncoding error:nil];
    NSMutableDictionary *d=[NSMutableDictionary dictionary];
    for (NSString *line in [s componentsSeparatedByString:@"\n"]) {
        NSRange r=[line rangeOfString:@" "];if (r.location==NSNotFound) continue;
        d[[line substringToIndex:r.location]]=[line substringFromIndex:r.location+1];
    }
    return d;
}

static const char *method_types(SEL sel)
{   /* search the composed protocol (it adopts ~20 sub-protocols) */
    struct objc_method_description m=protocol_getMethodDescription(svc_proto,sel,YES,YES);
    if (!m.types) m=protocol_getMethodDescription(svc_proto,sel,NO,YES);
    return m.types;
}

/* call a reply block with every argument zero (arm64: x1-x7 and d0-d7 cleared covers ints, pointers, floats) */
static void reply_zero(id block)
{
    if (!block) return;
    struct blk *b=(__bridge struct blk *)block;
    ((void (*)(void *,long,long,long,long,long,long,long,double,double,double,double,double,double,double,double))b->invoke)
        ((__bridge void *)block,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);
}

@interface RWCommCenter : NSObject <NSXPCListenerDelegate>
@end
@implementation RWCommCenter
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
    const char *t=method_types(sel);
    if (t) return [NSMethodSignature signatureWithObjCTypes:t];
    return [super methodSignatureForSelector:sel];
}
- (BOOL)respondsToSelector:(SEL)sel {return method_types(sel)!=NULL || [super respondsToSelector:sel];}
- (void)forwardInvocation:(NSInvocation *)inv
{
    NSString *name=NSStringFromSelector(inv.selector);
    BOOL first=NO;@synchronized (seen) {if (![seen containsObject:name]) {[seen addObject:name];first=YES;}}
    if (first) {NSLog(@"redwood-commcenter: called %@",name);}
    NSMethodSignature *sig=inv.methodSignature;
    for (NSUInteger i=2;i<sig.numberOfArguments;i++) {
        const char *t=[sig getArgumentTypeAtIndex:i];
        if (t[0]=='@' && t[1]=='?') {__unsafe_unretained id blk=nil;[inv getArgument:&blk atIndex:(NSInteger)i];reply_zero(blk);}
    }
}
- (BOOL)listener:(NSXPCListener *)l shouldAcceptNewConnection:(NSXPCConnection *)c
{
    c.exportedInterface=[NSXPCInterface interfaceWithProtocol:svc_proto];
    c.exportedObject=self;
    NSLog(@"redwood-commcenter: client pid %d",c.processIdentifier);
    [c resume];return YES;
}
@end

int main(void)
{
    @autoreleasepool {
        struct stat st;if (!stat("/var/root/cell-nocommcenter",&st)) {NSLog(@"redwood-commcenter: kill switch set, not starting");return 0;}
        dlopen("/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony",RTLD_NOW);
        svc_proto=objc_getProtocol("CTXPCServiceInterface");
        if (!svc_proto) {NSLog(@"redwood-commcenter: no CTXPCServiceInterface");return 1;}
        seen=[NSMutableSet set];
        static RWCommCenter *cc;cc=[RWCommCenter new];
        NSXPCListener *l=((id (*)(id,SEL,id))objc_msgSend)([NSXPCListener alloc],sel_registerName("initWithMachServiceName:"),@"com.apple.commcenter.coretelephony.xpc");   /* (marked unavailable on iOS in the SDK; it exists) */
        l.delegate=cc;[l resume];
        NSLog(@"redwood-commcenter: listening (modem state: %@)",cell_state()[@"operator"]?:@"none");
        [[NSRunLoop mainRunLoop] run];
    }
    return 0;
}

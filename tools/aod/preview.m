/* Calls the native CoverSheet AOD appearance transition for 12 seconds.
 * No hardware doze, capability spoof, brightness or authentication changes. */
#import <UIKit/UIKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <notify.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "preview_token.h"
#include "config.h"
static id cover;
static NSTimer *watchdog;
static UIGestureRecognizer *touchObserver;
static BOOL active;
static double deadline;
static uint64_t usedNonce;
static int displayToken;
static int statusToken=-1;
static void status(uint64_t value){if(statusToken>=0)notify_set_state(statusToken,value);}
static int (*bagState)(CFDictionaryRef);
static SEL transition;
static void finish(void);
static id getObject(id object,const char *name){
    SEL sel=sel_registerName(name);
    NSMethodSignature *sig=[object methodSignatureForSelector:sel];
    if(!sig || sig.numberOfArguments!=2 || sig.methodReturnType[0]!='@')return nil;
    return ((id(*)(id,SEL))objc_msgSend)(object,sel);
}
static BOOL getBool(id object,const char *name,BOOL fallback){
    SEL sel=sel_registerName(name);
    NSMethodSignature *sig=[object methodSignatureForSelector:sel];
    if(!sig || sig.numberOfArguments!=2 || (sig.methodReturnType[0]!='B'&&sig.methodReturnType[0]!='c'))return fallback;
    return ((BOOL(*)(id,SEL))objc_msgSend)(object,sel);
}
static id manager(void){return getObject((id)objc_getClass("SBLockScreenManager"),"sharedInstance");}
static BOOL displayAwake(void){uint64_t on=0;return notify_get_state(displayToken,&on)==NOTIFY_STATUS_OK&&on;}
static BOOL safeForPreview(id c){
    return !rw_aod_value(rw_aod_config(),@"Enabled") && bagState && bagState(NULL)==1 && displayAwake() &&
        getBool(manager(),"isUILocked",NO) &&
        getBool(c,"isMainPageVisible",NO) &&
        !getBool(c,"isPasscodeLockVisible",YES) &&
        !getBool(c,"isInlinePasscodeLockVisible",YES);
}
static BOOL nativeMethodMatches(void){
    Class cls=objc_getClass("CSCoverSheetViewController");
    Method method=cls?class_getInstanceMethod(cls,transition):NULL;
    if(!method||method_getNumberOfArguments(method)!=3)return NO;
    char ret[16]={0},arg[16]={0};
    method_getReturnType(method,ret,sizeof(ret));method_getArgumentType(method,2,arg,sizeof(arg));
    if(ret[0]!='v'||(arg[0]!='B'&&arg[0]!='c'))return NO;
    for(uint32_t i=0;i<_dyld_image_count();i++){
        const char *name=_dyld_get_image_name(i);
        if(name && strstr(name,"/CoverSheet.framework/CoverSheet"))
            return (uintptr_t)method_getImplementation(method)==UINT64_C(0x1c48950e0)+_dyld_get_image_vmaddr_slide(i);
    }
    return NO;
}
static BOOL consumeToken(void){
    int parent=open(RW_AOD_DIRECTORY,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK);if(parent<0)return NO;
    struct stat st;
    if(fstat(parent,&st)||!S_ISDIR(st.st_mode)||st.st_uid||(st.st_mode&0022)){close(parent);return NO;}
    int fd=openat(parent,"arm",O_RDONLY|O_NOFOLLOW|O_NONBLOCK);close(parent);if(fd<0)return NO;
    struct rw_aod_token token={0};
    BOOL ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&!st.st_uid&&st.st_nlink==1&&
        (st.st_mode&0777)==0644&&st.st_size==sizeof(token)&&read(fd,&token,sizeof(token))==sizeof(token);
    close(fd);
    if(!ok||!rw_aod_token_valid(token,NSProcessInfo.processInfo.systemUptime,usedNonce))return NO;
    usedNonce=token.nonce;return YES;
}
@interface RWAODTouchObserver:UIGestureRecognizer
@end
@implementation RWAODTouchObserver
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event{
    (void)touches;(void)event;self.state=UIGestureRecognizerStateFailed;finish();
}
@end
static void finish(void){
    if(!active)return;active=NO;
    [watchdog invalidate];watchdog=nil;
    if([cover isKindOfClass:UIViewController.class])[((UIViewController*)cover).view removeGestureRecognizer:touchObserver];
    touchObserver=nil;
    int nativeToken=-1;uint64_t flags=0;
    if(notify_register_check(RW_AOD_SERVICE_STATUS,&nativeToken)==NOTIFY_STATUS_OK){notify_get_state(nativeToken,&flags);notify_cancel(nativeToken);}
    BOOL nativeOwnsAppearance=(flags&7)==7&&((flags>>8)&15)==1;
    BOOL restored=NO;
    @try {if(!nativeOwnsAppearance&&cover&&nativeMethodMatches()){
        ((void(*)(id,SEL,BOOL))objc_msgSend)(cover,transition,NO);restored=YES;
    }}
    @catch(NSException *exception){(void)exception;NSLog(@"RedwoodAODPreview: appearance restoration raised an exception");}
    cover=nil;status(nativeOwnsAppearance?7:restored?4:6);
    NSLog(@"RedwoodAODPreview: preview ended; %@",nativeOwnsAppearance?@"native service owns appearance":restored?@"normal appearance requested":@"restoration failed");
}
static void begin(void){
    if(active||!consumeToken())return;
    @try {
        id candidate=getObject(manager(),"coverSheetViewController");
        if(!candidate||!nativeMethodMatches()||!safeForPreview(candidate)||
            ![candidate isKindOfClass:UIViewController.class]||![(UIViewController*)candidate isViewLoaded]||
            !((UIViewController*)candidate).view.window){
            status(5);NSLog(@"RedwoodAODPreview: declined; expected native method or locked/awake main page missing");return;
        }
        cover=candidate;active=YES;deadline=NSProcessInfo.processInfo.systemUptime+12;
        /* This observation does not recognize a gesture or delay delivery. */
        touchObserver=[[RWAODTouchObserver alloc]initWithTarget:nil action:NULL];
        touchObserver.cancelsTouchesInView=NO;touchObserver.delaysTouchesBegan=NO;touchObserver.delaysTouchesEnded=NO;
        [((UIViewController*)cover).view addGestureRecognizer:touchObserver];
        watchdog=[NSTimer timerWithTimeInterval:.2 repeats:YES block:^(NSTimer *timer){
            (void)timer;if(NSProcessInfo.processInfo.systemUptime>=deadline||!safeForPreview(cover))finish();
        }];
        [NSRunLoop.mainRunLoop addTimer:watchdog forMode:NSRunLoopCommonModes];
        ((void(*)(id,SEL,BOOL))objc_msgSend)(cover,transition,YES);
        status(3);NSLog(@"RedwoodAODPreview: native appearance requested for 12 seconds; hardware AOD is not enabled");
    } @catch(NSException *exception){(void)exception;finish();status(6);NSLog(@"RedwoodAODPreview: native appearance test aborted");}
}
__attribute__((constructor))static void start(void){
    if(strcmp(getprogname(),"SpringBoard"))return;
    dispatch_async(dispatch_get_main_queue(),^{
        notify_register_check(RW_AOD_STATUS,&statusToken);
        transition=sel_registerName("_updateAppearanceForAODTransitionToInactive:");
        void *mkb=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_NOW);
        bagState=mkb?dlsym(mkb,"MKBGetDeviceLockState"):NULL;
        if(notify_register_dispatch("com.apple.iokit.hid.displayStatus",&displayToken,dispatch_get_main_queue(),^(int token){
            (void)token;if(!displayAwake())finish();
        })!=NOTIFY_STATUS_OK)return;
        static int beginToken,endToken;
        notify_register_dispatch(RW_AOD_BEGIN,&beginToken,dispatch_get_main_queue(),^(int token){(void)token;begin();});
        notify_register_dispatch(RW_AOD_END,&endToken,dispatch_get_main_queue(),^(int token){(void)token;finish();});
        status(nativeMethodMatches()?1:2);
        NSLog(@"RedwoodAODPreview: loaded, idle; native appearance method %@",nativeMethodMatches()?@"verified":@"unavailable");
    });
}

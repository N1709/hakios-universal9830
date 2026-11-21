/* Persistent iOS AOD policy on the S20. Native BacklightServices owns the
 * transition, clock, wake, focus and suppression logic. Panel ALPM is separate. */
#import <UIKit/UIKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <QuartzCore/QuartzCore.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <sys/sysctl.h>
#include "config.h"
void rw_alwayson_settings_start(void);
static atomic_bool enabled,ready,configured,osSupports,osNativeSupported,osObserved;
static _Atomic(long long) lastState,completedState;
static atomic_bool requestObserved,completionObserved;
static atomic_uint_fast64_t transitionGeneration;
static int statusToken=-1,lowPowerToken=-1;
/* Low Power Mode suspends AOD, as on iPhone; the saved setting is kept. */
static BOOL lowPowerMode(void){
    uint64_t on=0;
    if(lowPowerToken<0&&notify_register_check("com.apple.system.lowpowermode",&lowPowerToken)!=NOTIFY_STATUS_OK)return NO;
    return notify_get_state(lowPowerToken,&on)==NOTIFY_STATUS_OK&&on;
}
/* Kernel AOD panel dimming (kern.redwood.display_aod): absent on old kernels. */
static void panelAOD(BOOL on){
    int value=on?1:0;(void)sysctlbyname("kern.redwood.display_aod",NULL,NULL,&value,sizeof(value));
}
static __weak id platform;
static BOOL (*osSupportOriginal)(id,SEL);
typedef id (*RWEngineInit)(id __attribute__((ns_consumed)),SEL,id,id,id,id) __attribute__((ns_returns_retained));
static RWEngineInit engineInitOriginal;
static void (*pseudoOriginal)(id,SEL,BOOL);
static void (*bindOriginal)(id,SEL);
static void (*updateOriginal)(id,SEL);
static void (*transitionOriginal)(id,SEL,id,long long,id);
static void (*completeOriginal)(id,SEL,id,long long,id);
static id object(id obj,const char *name){
    SEL sel=sel_registerName(name);NSMethodSignature *sig=[obj methodSignatureForSelector:sel];
    return sig&&sig.numberOfArguments==2&&sig.methodReturnType[0]=='@'?((id(*)(id,SEL))objc_msgSend)(obj,sel):nil;
}
static BOOL boolean(id obj,const char *name){
    SEL sel=sel_registerName(name);NSMethodSignature *sig=[obj methodSignatureForSelector:sel];
    return sig&&sig.numberOfArguments==2&&(sig.methodReturnType[0]=='B'||sig.methodReturnType[0]=='c')?
        ((BOOL(*)(id,SEL))objc_msgSend)(obj,sel):NO;
}
static BOOL setBool(id obj,const char *name,BOOL value){
    SEL sel=sel_registerName(name);NSMethodSignature *sig=[obj methodSignatureForSelector:sel];
    if(!sig||sig.numberOfArguments!=3||sig.methodReturnType[0]!='v')return NO;
    const char *type=[sig getArgumentTypeAtIndex:2];if(type[0]!='B'&&type[0]!='c')return NO;
    ((void(*)(id,SEL,BOOL))objc_msgSend)(obj,sel,value);return YES;
}
static void publish(void){
    if(statusToken<0)return;
    uint64_t flags=RW_AOD_STATUS_VERSION|(ready?RW_AOD_READY:0)|(enabled?RW_AOD_ENABLED:0)|(boolean(platform,"isAlwaysOnEnabled")?RW_AOD_NATIVE_ENABLED:0);
    flags|=requestObserved?RW_AOD_REQUEST_VALID:0;flags|=completionObserved?RW_AOD_COMPLETED_VALID:0;
    flags|=osSupports?16:0;flags|=osNativeSupported?32:0;flags|=osObserved?64:0;
    flags|=((uint64_t)lastState&15)<<8;flags|=((uint64_t)completedState&15)<<12;notify_set_state(statusToken,flags);
}
static void applyDomain(id domain){
    NSDictionary *config=rw_aod_config();enabled=rw_aod_value(config,@"Enabled")&&!lowPowerMode();
    BOOL ok=YES;
    /* Native development support flag; not a global MobileGestalt override. */
    ok=setBool(domain,"setForceAndromedaSupport:",YES)&&ok;
    ok=setBool(domain,"setShowWallpaperInAlwaysOn:",rw_aod_value(config,@"ShowWallpaper"))&&ok;
    ok=setBool(domain,"setShowNotificationsInAlwaysOn:",rw_aod_value(config,@"ShowNotifications"))&&ok;
    /* Apply appearance before enabling: avoid briefly rendering old wallpaper. */
    ok=setBool(domain,"setEnableAlwaysOn:",ok&&enabled)&&ok;
    configured=ok;
    if(!ok){enabled=NO;setBool(domain,"setEnableAlwaysOn:",NO);}
    publish();
}
static void defaultsBound(id self,SEL cmd){
    bindOriginal(self,cmd);
    @try {applyDomain(self);}@catch(NSException *exception){(void)exception;enabled=NO;}
}
static BOOL observedOSSupport(id self,SEL cmd){
    osNativeSupported=osSupportOriginal(self,cmd);osObserved=YES;
    /* S20 has an AOD-capable panel. Adapt the iPhone-only capability test;
     * this does not claim the panel driver has entered ALPM. */
    osSupports=YES;publish();return YES;
}
/* The port has no CAFlipBook capture backend. Apple's own pseudo-flipbook
 * keeps the native presentation engine on live composition instead. Never
 * suppress the renderer's fatal checks or pretend a missing frame rendered. */
static void useLiveComposition(id self,SEL cmd,BOOL requested){
    (void)requested;pseudoOriginal(self,cmd,YES);
}
static id engineInitialized(id self __attribute__((ns_consumed)),SEL cmd,id delegate,id provider,id os,id budget) __attribute__((ns_returns_retained));
static id engineInitialized(id self __attribute__((ns_consumed)),SEL cmd,id delegate,id provider,id os,id budget){
    id result=engineInitOriginal(self,cmd,delegate,provider,os,budget);
    if(result)setBool(result,"setUsePseudoFlipbook:",YES);
    return result;
}
static void restoreUnusedOSOverride(void){
    NSUserDefaults *metadata=[[NSUserDefaults alloc]initWithSuiteName:@"com.redwood.alwayson-compatibility"];
    if(![metadata boolForKey:@"SavedOriginalOverride"])return;
    NSUserDefaults *native=[[NSUserDefaults alloc]initWithSuiteName:@"com.apple.BacklightServices"];
    id old=[metadata objectForKey:@"OriginalOverride"];
    if([metadata boolForKey:@"OriginalOverridePresent"]&&[old isKindOfClass:NSNumber.class])
        [native setObject:old forKey:@"deviceSupportsAlwaysOnOverride"];
    else [native removeObjectForKey:@"deviceSupportsAlwaysOnOverride"];
    [native synchronize];
    [metadata removeObjectForKey:@"SavedOriginalOverride"];[metadata synchronize];
}
static BOOL s20Support(id self,SEL cmd){(void)self;(void)cmd;return YES;}
static void policyUpdated(id self,SEL cmd){
    platform=self;updateOriginal(self,cmd);publish();
}
static void revokeForInactive(void){
    id app=UIApplication.sharedApplication;id auth=object(app,"authenticationController");
    SEL revoke=sel_registerName("revokeAuthenticationImmediatelyForPublicReason:");
    NSMethodSignature *sig=[auth methodSignatureForSelector:revoke];
    if(sig&&sig.numberOfArguments==3&&sig.methodReturnType[0]=='v'&&[sig getArgumentTypeAtIndex:2][0]=='@')
        ((void(*)(id,SEL,id))objc_msgSend)(auth,revoke,@"Redwood Always-On: locked display");
}
/* With the AOD wallpaper off, iOS keeps the lock screen wallpaper on screen until
 * its slow AOD fade ends: a flash of wallpaper. The lock screen draws the wallpaper
 * in SBCoverSheetPanelBackgroundContainerView (a portal of the wallpaper window);
 * cover that with black at once (under the clock), take it away on wake. */
static UIView *blackout;
static NSArray *allWindows(void){
    NSMutableArray *windows=[NSMutableArray array];
    for(UIScene *scene in UIApplication.sharedApplication.connectedScenes)
        if([scene isKindOfClass:UIWindowScene.class])[windows addObjectsFromArray:((UIWindowScene*)scene).windows];
    return windows;
}
static UIView *findView(UIView *v,Class cls,int depth){
    if([v isKindOfClass:cls])return v;
    if(depth>6)return nil;
    for(UIView *c in v.subviews){UIView *f=findView(c,cls,depth+1);if(f)return f;}
    return nil;
}
/* Only when locking from the home screen or an app (the lock screen window was
 * not showing yet): from the lock screen itself iOS's own wallpaper fade stays. */
static BOOL lockScreenShowing(void){
    for(UIWindow *w in allWindows())
        if([NSStringFromClass(w.class) isEqualToString:@"SBCoverSheetWindow"])return !w.hidden&&w.alpha>0.01;
    return YES;
}
static void wallpaperForAOD(BOOL aod){
    if(!NSThread.isMainThread){dispatch_async(dispatch_get_main_queue(),^{wallpaperForAOD(aod);});return;}
    if(!aod){[blackout.layer removeAllAnimations];[blackout removeFromSuperview];blackout=nil;return;}
    if(blackout||rw_aod_value(rw_aod_config(),@"ShowWallpaper")||lockScreenShowing())return;
    Class container=objc_getClass("SBCoverSheetPanelBackgroundContainerView");
    if(!container)return;
    for(UIWindow *w in allWindows()){
        if(![NSStringFromClass(w.class) isEqualToString:@"SBCoverSheetWindow"])continue;
        UIView *host=findView(w,container,0);
        if(!host)continue;
        blackout=[[UIView alloc]initWithFrame:host.bounds];
        blackout.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        blackout.backgroundColor=UIColor.blackColor;blackout.userInteractionEnabled=NO;
        blackout.alpha=0;[host addSubview:blackout];
        /* the wallpaper fades to black with the panel's dimming (600 ms), a little ahead of it */
        [UIView animateWithDuration:0.35 delay:0 options:UIViewAnimationOptionCurveEaseOut animations:^{blackout.alpha=1;} completion:nil];
        break;
    }
}
static void backlightWillChange(id self,SEL cmd,id host,long long state,id event){
    lastState=state;requestObserved=YES;uint64_t generation=++transitionGeneration;
    NSLog(@"RedwoodAOD: will %lld at %.3f",state,CACurrentMediaTime());
    /* InactiveOn (1) is the AOD picture: dim the panel; awake states restore. */
    if(state==1)panelAOD(enabled);else if(state>=2)panelAOD(NO);
    @try {if(state==1&&enabled)wallpaperForAOD(YES);else if(state>=2)wallpaperForAOD(NO);}
    @catch(NSException *exception){(void)exception;}
    transitionOriginal(self,cmd,host,state,event);publish();
    /* InactiveOn is AOD, not an authenticated screen. Preserve the software
     * passcode policy even if displayStatus stays on in this native mode. */
    if(enabled&&rw_aod_inactive(state))dispatch_async(dispatch_get_main_queue(),^{
        if(!rw_aod_revoke_current(enabled,generation,transitionGeneration,lastState))return;
        @try {revokeForInactive();}@catch(NSException *exception){(void)exception;}
    });
}
static void backlightCompleted(id self,SEL cmd,id backlight,long long state,id event){
    completeOriginal(self,cmd,backlight,state,event);
    completedState=state;completionObserved=YES;publish();
    NSLog(@"RedwoodAOD: did %lld at %.3f",state,CACurrentMediaTime());
}
static BOOL pinned(Method method,const char *image,uintptr_t address,unsigned arguments,char result){
    if(!method||method_getNumberOfArguments(method)!=arguments)return NO;
    char type[16]={0};method_getReturnType(method,type,sizeof(type));
    if(type[0]!=result)return NO;
    for(uint32_t i=0;i<_dyld_image_count();i++)if(strstr(_dyld_get_image_name(i),image))
        return (uintptr_t)method_getImplementation(method)==address+_dyld_get_image_vmaddr_slide(i);
    return NO;
}
static void applySettings(void){
    @try {id domain=object(object((id)objc_getClass("SBDefaults"),"localDefaults"),"alwaysOnDefaults");
        if(domain)applyDomain(domain);
    }@catch(NSException *exception){(void)exception;enabled=NO;}
    publish();
}
__attribute__((constructor))static void start(void){
    if(!rw_aod_target())return;
    if(!strcmp(getprogname(),"Preferences")){rw_alwayson_settings_start();return;}
    if(strcmp(getprogname(),"SpringBoard"))return;
    Class sharedGuard=objc_getClass("SBBacklightPlatformProvider");
    const void *guardKey=(const void*)sel_registerName("redwoodAlwaysOnServiceInstalled");
    if(sharedGuard&&objc_getAssociatedObject((id)sharedGuard,guardKey))return;
    enabled=rw_aod_value(rw_aod_config(),@"Enabled")&&!lowPowerMode();
    panelAOD(NO); /* a SpringBoard restart never leaves the panel dimmed */
    notify_register_check(RW_AOD_SERVICE_STATUS,&statusToken);
    Class defaults=objc_getClass("SBAlwaysOnDefaults"),provider=objc_getClass("SBBacklightPlatformProvider"),controller=objc_getClass("SBBacklightController");
    Class engine=objc_getClass("BLSHAlwaysOnPresentationEngine");
    Method engineInit=class_getInstanceMethod(engine,sel_registerName("initWithDelegate:platformProvider:osInterfaceProvider:inactiveBudgetPolicy:"));
    Method pseudo=class_getInstanceMethod(engine,sel_registerName("setUsePseudoFlipbook:"));
    Method osSupport=class_getInstanceMethod(objc_getClass("BLSHBacklightOSInterfaceProvider"),sel_registerName("deviceSupportsAlwaysOn"));
    Method bind=class_getInstanceMethod(defaults,sel_registerName("_bindAndRegisterDefaults"));
    Method support=class_getClassMethod(provider,sel_registerName("deviceSupportsAlwaysOn"));
    Method update=class_getInstanceMethod(provider,sel_registerName("_updateAlwaysOnEnabled"));
    Method complete=class_getInstanceMethod(controller,sel_registerName("backlight:didCompleteUpdateToState:forEvent:"));
    Method transition=class_getInstanceMethod(controller,sel_registerName("backlightHost:willTransitionToState:forEvent:"));
    if(!pinned(complete,"/SpringBoard.framework/",0x1c5066388,5,'v')||
        !pinned(engineInit,"/BacklightServicesHost.framework/",0x1eebb29c4,6,'@')||
        !pinned(pseudo,"/BacklightServicesHost.framework/",0x1eebb8dd4,3,'v')||
        !pinned(osSupport,"/BacklightServicesHost.framework/",0x1eeba9734,2,'B')||
        !pinned(bind,"/SpringBoardFoundation.framework/",0x18e609e4c,2,'v')||
        !pinned(support,"/SpringBoard.framework/",0x1c4fb11f0,2,'B')||
        !pinned(update,"/SpringBoard.framework/",0x1c4fb15d0,2,'v')||
        !pinned(transition,"/SpringBoard.framework/",0x1c506650c,5,'v')){publish();return;}
    bindOriginal=(void*)method_setImplementation(bind,(IMP)defaultsBound);
    /* Enforce our default-off setting before making the native path available. */
    applySettings();
    if(!configured){method_setImplementation(bind,(IMP)bindOriginal);publish();return;}
    restoreUnusedOSOverride();
    pseudoOriginal=(void*)method_setImplementation(pseudo,(IMP)useLiveComposition);
    engineInitOriginal=(void*)method_setImplementation(engineInit,(IMP)engineInitialized);
    osSupportOriginal=(void*)method_setImplementation(osSupport,(IMP)observedOSSupport);
    method_setImplementation(support,(IMP)s20Support);
    updateOriginal=(void*)method_setImplementation(update,(IMP)policyUpdated);
    completeOriginal=(void*)method_setImplementation(complete,(IMP)backlightCompleted);
    transitionOriginal=(void*)method_setImplementation(transition,(IMP)backlightWillChange);
    ready=YES;objc_setAssociatedObject((id)sharedGuard,guardKey,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    dispatch_async(dispatch_get_main_queue(),^{applySettings();});
    static int token;notify_register_dispatch(RW_AOD_CHANGED,&token,dispatch_get_main_queue(),^(int t){(void)t;applySettings();});
    static int lpmToken;notify_register_dispatch("com.apple.system.lowpowermode",&lpmToken,dispatch_get_main_queue(),^(int t){(void)t;applySettings();});
    publish();
}

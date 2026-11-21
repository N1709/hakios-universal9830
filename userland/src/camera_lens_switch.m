/* Use Camera's existing lens selector. No overlay window or sensor writes here. */
#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#ifndef RW_LENS_CONTROL_PATH
#define RW_LENS_CONTROL_PATH @"/var/mobile/Library/Caches/com.apple.mediaserverd/redwood-rear-zoom"
#endif
static NSString *const control=RW_LENS_CONTROL_PATH;
static char proxyKey;
static BOOL defaultRearApplied;
#ifndef RW_LENS_PRIMARY_TEST_PATH
#define RW_LENS_PRIMARY_TEST_PATH "/var/mobile/Library/Caches/com.apple.mediaserverd/redwood-primary-test-default"
#endif
static NSString *defaultRearValue(void) {
    return access(RW_LENS_PRIMARY_TEST_PATH,F_OK)==0?@"1\n":@"0.9\n";
}
#ifndef RW_LENS_TELE_ENABLE_PATH
#define RW_LENS_TELE_ENABLE_PATH "/var/mobile/Library/Caches/com.apple.mediaserverd/redwood-gw2-enable"
#endif
static BOOL teleEnabled(void){return access(RW_LENS_TELE_ENABLE_PATH,F_OK)==0;}
static double maximumZoom(void){return teleEnabled()?16:5;}
static SEL configurationSelector(void) {
    return NSSelectorFromString(@"configureForDualDeviceWithZoomFactor:minimumZoomFactor:displayMinimumZoomFactor:maximumZoomFactor:displayMaximumZoomFactor:dualCameraSwitchOverZoomFactor:displayDualCameraSwitchOverZoomFactor:");
}
static double requestedZoom(void) {
    double z=[NSString stringWithContentsOfFile:control encoding:NSUTF8StringEncoding error:nil].doubleValue;
    return isfinite(z)&&z>=0.5?fmin(z,maximumZoom()):1;
}
/* Stock-style display: 0.5x full ultra-wide, 1x primary. No relabeling. */
static const double displayBase=1.0;
static void showZoom(id zoom,double factor) {
    SEL min=NSSelectorFromString(@"setMinAvailableZoomFactor:");
    if([zoom respondsToSelector:min])((void(*)(id,SEL,double))objc_msgSend)(zoom,min,0.5);
    ((void(*)(id,SEL,double,double,double,double,double,double,double))objc_msgSend)(zoom,configurationSelector(),factor,0.5,0.5/displayBase,maximumZoom(),maximumZoom()/displayBase,1,1/displayBase);
    if([zoom respondsToSelector:min])((void(*)(id,SEL,double))objc_msgSend)(zoom,min,0.5);
}
@interface RWNativeLensDelegate:NSObject
@property(nonatomic,weak) id original;
@property(nonatomic) BOOL rear;
@property(nonatomic) double factor;
@end
@implementation RWNativeLensDelegate
- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector]||[self.original respondsToSelector:selector];
}
- (id)forwardingTargetForSelector:(SEL)selector {return self.original;}
- (BOOL)writeZoom:(double)factor {
    if(!isfinite(factor))return NO;
    factor=fmax(0.5,fmin(maximumZoom(),factor));
    if(![[NSString stringWithFormat:@"%.6f\n",factor] writeToFile:control atomically:YES encoding:NSUTF8StringEncoding error:nil])return NO;
    self.factor=factor;return YES;
}
- (void)zoomControl:(id)zoom didChangeSelectedButtonIndex:(NSUInteger)index displayZoomFactor:(double)factor {
    if(!self.rear){
        if([self.original respondsToSelector:_cmd])((void(*)(id,SEL,id,NSUInteger,double))objc_msgSend)(self.original,_cmd,zoom,index,factor);
        return;
    }
    if([self writeZoom:factor*displayBase]){showZoom(zoom,self.factor);}
}
- (BOOL)canToggleBetweenZoomFactorsForZoomControl:(id)zoom {
    if(self.rear)return YES;
    SEL sel=_cmd;return [self.original respondsToSelector:sel]?((BOOL(*)(id,SEL,id))objc_msgSend)(self.original,sel,zoom):NO;
}
- (void)toggleBetweenZoomFactorsForZoomControl:(id)zoom {
    if(self.rear){if([self writeZoom:teleEnabled()?(requestedZoom()<1?1:requestedZoom()<3?3:0.5):(requestedZoom()<1?1:0.5)]){showZoom(zoom,self.factor);}return;}
    if([self.original respondsToSelector:_cmd])((void(*)(id,SEL,id))objc_msgSend)(self.original,_cmd,zoom);
}
- (void)zoomControl:(id)zoom didChangeZoomFactor:(double)factor interactionType:(NSInteger)type {
    if(self.rear){
        if(![self writeZoom:factor])return;
        /* Backend crop and metadata follow this same value. Forwarding to the
         * D22 graph can trigger a camera reconfiguration in the middle of video. */
        return;
    }
    if([self.original respondsToSelector:_cmd])((void(*)(id,SEL,id,double,NSInteger))objc_msgSend)(self.original,_cmd,zoom,factor,type);
}

@end
static IMP originalButtonTap,originalSingleToggle;
static BOOL nativeRearCycle(id zoom,double before){
    RWNativeLensDelegate *proxy=objc_getAssociatedObject(zoom,&proxyKey);
    if(!teleEnabled()||!proxy.rear)return NO;
    double next=before<1?1:before<3?3:0.5;
    if([proxy writeZoom:next])showZoom(zoom,next);
    SEL cleanup=NSSelectorFromString(@"_cleanupForCancelTracking"),hide=NSSelectorFromString(@"_setShouldShowZoomDial:");
    if([zoom respondsToSelector:cleanup])((void(*)(id,SEL))objc_msgSend)(zoom,cleanup);
    if([zoom respondsToSelector:hide])((void(*)(id,SEL,BOOL))objc_msgSend)(zoom,hide,NO);
    return YES;
}
static void nativeButtonTap(id zoom,SEL selector,id button){
    double before=requestedZoom();((void(*)(id,SEL,id))originalButtonTap)(zoom,selector,button);
    nativeRearCycle(zoom,before);
}
static void nativeSingleToggle(id zoom,SEL selector){
    double before=requestedZoom();((void(*)(id,SEL))originalSingleToggle)(zoom,selector);
    nativeRearCycle(zoom,before);
}
static void installNativeTapHooks(void){
    Class cls=NSClassFromString(@"CAMZoomControl");if(!cls||originalButtonTap)return;
    Method tap=class_getInstanceMethod(cls,NSSelectorFromString(@"_handleButtonTapped:"));
    Method toggle=class_getInstanceMethod(cls,NSSelectorFromString(@"_handleButtonTappedForSingleCameraZoomPlusToggle"));
    if(!tap||strcmp(method_getTypeEncoding(tap),"v24@0:8@16")||!toggle||strcmp(method_getTypeEncoding(toggle),"v16@0:8"))return;
    originalButtonTap=method_setImplementation(tap,(IMP)nativeButtonTap);
    originalSingleToggle=method_setImplementation(toggle,(IMP)nativeSingleToggle);
}
static void configureZoom(UIView *zoom) {
    SEL getDelegate=NSSelectorFromString(@"delegate"),setDelegate=NSSelectorFromString(@"setDelegate:");
    if(![zoom respondsToSelector:getDelegate]||![zoom respondsToSelector:setDelegate])return;
    id delegate=((id(*)(id,SEL))objc_msgSend)(zoom,getDelegate);
    RWNativeLensDelegate *proxy=objc_getAssociatedObject(zoom,&proxyKey);
    id controller=delegate==proxy?proxy.original:delegate;
    if(!controller)return;
    id graph=nil;
    @try {graph=[controller valueForKey:@"_currentGraphConfiguration"];}@catch(__unused NSException *e){return;}
    SEL position=NSSelectorFromString(@"devicePosition");
    if(![graph respondsToSelector:position])return;
    /* CameraUI uses 0=Back, 1=Front (not AVCaptureDevicePosition). */
    BOOL rear=((NSInteger(*)(id,SEL))objc_msgSend)(graph,position)==0;
    if(!rear) {
        if(delegate==proxy)((void(*)(id,SEL,id))objc_msgSend)(zoom,setDelegate,controller);
        proxy.rear=NO;return;
    }
    if(!defaultRearApplied){
        if([defaultRearValue() writeToFile:control atomically:YES encoding:NSUTF8StringEncoding error:nil])defaultRearApplied=YES;
    }
    if(!proxy){proxy=[RWNativeLensDelegate new];proxy.factor=-1;objc_setAssociatedObject(zoom,&proxyKey,proxy,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
    proxy.original=controller;proxy.rear=YES;installNativeTapHooks();
    if(delegate!=proxy)((void(*)(id,SEL,id))objc_msgSend)(zoom,setDelegate,proxy);
    double factor=requestedZoom();
    NSArray *factors=nil;
    @try{factors=[zoom valueForKey:@"_zoomFactors"];}@catch(__unused NSException *e){}
    if(proxy.factor!=factor||![factors containsObject:@0.5]){proxy.factor=factor;showZoom(zoom,factor);}
    /* The single-device graph can republish a 1x UI floor and zoom. The rear
     * route accepts 0.5x; retain that range and the user's nominal lens value. */
    SEL min=NSSelectorFromString(@"setMinAvailableZoomFactor:");
    if([zoom respondsToSelector:min])((void(*)(id,SEL,double))objc_msgSend)(zoom,min,0.5);
    SEL get=NSSelectorFromString(@"zoomFactor"),set=NSSelectorFromString(@"setZoomFactor:");
    if([zoom respondsToSelector:get]&&[zoom respondsToSelector:set]&&fabs(((double(*)(id,SEL))objc_msgSend)(zoom,get)-factor)>0.0001)
        ((void(*)(id,SEL,double))objc_msgSend)(zoom,set,factor);
}
static void walk(UIView *view,Class cls) {
    if([view isKindOfClass:cls]){configureZoom(view);return;}
    for(UIView *child in view.subviews)walk(child,cls);
}
__attribute__((constructor))static void start(void) {
    @autoreleasepool {
        if(![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.camera"])return;
        /* Override the earlier bring-up preference for the front camera. */
        CFStringRef app=CFSTR("com.apple.camera");
        CFPreferencesSetAppValue(CFSTR("CAMUserPreferenceCaptureDevice"),(__bridge CFTypeRef)@0,app);
        CFPreferencesSetAppValue(CFSTR("CAMUserPreferenceCaptureMode"),(__bridge CFTypeRef)@0,app);
        CFPreferencesSetAppValue(CFSTR("CAMUserPreferenceResetTimeoutDate"),(__bridge CFTypeRef)[NSDate dateWithTimeIntervalSinceNow:600],app);
        CFPreferencesAppSynchronize(app);
        /* Set preferred rear route before Camera starts its graph, avoiding a
         * primary cold start immediately followed by an ultra-wide cold start. */
        defaultRearApplied=[defaultRearValue() writeToFile:control atomically:YES encoding:NSUTF8StringEncoding error:nil];
        dispatch_async(dispatch_get_main_queue(),^{
            NSTimer *timer=[NSTimer timerWithTimeInterval:.5 repeats:YES block:^(__unused NSTimer *t){
                if(UIApplication.sharedApplication.applicationState!=UIApplicationStateActive)return;
                /* CameraUI may load after tweak constructors and the first main
                 * queue turn. Resolve it here so early loading cannot disable UI. */
                Class cls=NSClassFromString(@"CAMZoomControl");
                Method method=class_getInstanceMethod(cls,configurationSelector());
                if(!method||strcmp(method_getTypeEncoding(method),"v72@0:8d16d24d32d40d48d56d64"))return;
                for(UIScene *scene in UIApplication.sharedApplication.connectedScenes)
                    if([scene isKindOfClass:UIWindowScene.class]&&scene.activationState==UISceneActivationStateForegroundActive)
                        for(UIWindow *window in ((UIWindowScene*)scene).windows)walk(window,cls);
            }];
            [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
        });
    }
}

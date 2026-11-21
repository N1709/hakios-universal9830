/* Persistent display-only rule, loaded into Camera + SpringBoard by TweakInject.
 * Camera renews a foreground lease; SpringBoard owns native brightness writes
 * and a crash/reboot recovery record. No ISP, sensor, AE/AF or lock-screen hooks. */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/stat.h>
#include <unistd.h>
#include "camera_brightness_rule.h"
static const char *leaseName="com.redwood.camera.foreground";
static NSString *const statePath=@"/var/mobile/Library/Preferences/com.redwood.camera-brightness.restore.plist";
static const char *disabledPath="/var/jb/etc/redwood-camera-brightness.disabled";
static int leaseToken=-1;
static BOOL foreground;
static id client;
static struct cb_state state;
static NSDictionary *recovery;
static BOOL restoreSent;
static double readyAfter;
static int lastCamera=-2;
/* SpringBoard can identify its foreground app without injecting Camera. This
 * also works on existing read-only images whose loader list predates Camera.
 * An unsupported API returns unknown and falls back to the renewable lease. */
static int cameraFromSpringBoard(void){
    id app=UIApplication.sharedApplication;
    SEL front=sel_registerName("_accessibilityFrontMostApplication");
    if(![app respondsToSelector:front])return -1;
    id active=((id(*)(id,SEL))objc_msgSend)(app,front);
    if(!active)return 0;
    for(NSString *name in @[@"bundleIdentifier",@"displayIdentifier"]){
        SEL ident=NSSelectorFromString(name);
        if([active respondsToSelector:ident]){
            id value=((id(*)(id,SEL))objc_msgSend)(active,ident);
            if([value isKindOfClass:NSString.class])return [value isEqualToString:@"com.apple.camera"]?1:0;
        }
    }
    return -1;
}
static mach_timebase_info_data_t tb;
static double seconds(uint64_t ticks){return (double)ticks*tb.numer/tb.denom/1e9;}
static id property(NSString *key){
    CFTypeRef v=((CFTypeRef(*)(id,SEL,CFStringRef))objc_msgSend)(client,sel_registerName("copyPropertyForKey:"),(__bridge CFStringRef)key);
    return CFBridgingRelease(v);
}
static int boolean(NSString *key){id v=property(key);return [v isKindOfClass:NSNumber.class]?[v boolValue]:-1;}
static BOOL autoSet(BOOL value){return ((BOOL(*)(id,SEL,id,id))objc_msgSend)(client,sel_registerName("setProperty:forKey:"),@(value),@"DisplayBrightnessAuto");}
static io_service_t backlight(void){return IOServiceGetMatchingService(0,IOServiceMatching("AppleARMBacklight"));}
static double slider(void){
    io_service_t bl=backlight();if(!bl)return -1;
    id d=CFBridgingRelease(IORegistryEntryCreateCFProperty(bl,CFSTR("IODisplayParameters"),NULL,0));IOObjectRelease(bl);
    if(![d isKindOfClass:NSDictionary.class]||![d[@"brightness"] isKindOfClass:NSDictionary.class])return -1;
    id v=d[@"brightness"][@"value"],m=d[@"brightness"][@"max"];
    if(![v isKindOfClass:NSNumber.class]||![m isKindOfClass:NSNumber.class]||[m doubleValue]<=0)return -1;
    double ui=([v doubleValue]/[m doubleValue]-.025)/.95;
    return isfinite(ui)?fmax(0,fmin(1,ui)):-1;
}
static BOOL setSlider(double ui){
    if(!isfinite(ui)||ui<0||ui>1)return NO;
    io_service_t bl=backlight();if(!bl)return NO;
    id d=CFBridgingRelease(IORegistryEntryCreateCFProperty(bl,CFSTR("IODisplayParameters"),NULL,0));
    id m=[d isKindOfClass:NSDictionary.class]&&[d[@"brightness"] isKindOfClass:NSDictionary.class]?d[@"brightness"][@"max"]:nil;
    double max=[m isKindOfClass:NSNumber.class]?[m doubleValue]:0;
    kern_return_t(*writeProps)(io_registry_entry_t,CFTypeRef)=dlsym(RTLD_DEFAULT,"IORegistryEntrySetCFProperties");
    BOOL ok=NO;
    if(writeProps&&isfinite(max)&&max>0&&max<=UINT32_MAX){
        NSDictionary *r=@{@"brightness":@((uint32_t)llround((.025+.95*ui)*max))};
        ok=writeProps(bl,(__bridge CFTypeRef)r)==KERN_SUCCESS;
    }
    IOObjectRelease(bl);return ok;
}
static BOOL validRecord(NSDictionary *d){
    if(![d isKindOfClass:NSDictionary.class]||![d[@"Version"] isEqual:@1]||
       ![d[@"Auto"] isKindOfClass:NSNumber.class]||![d[@"UI"] isKindOfClass:NSNumber.class])return NO;
    double v=[d[@"UI"] doubleValue],a=[d[@"Auto"] doubleValue];
    return (a==0||a==1)&&isfinite(v)&&v>=0&&v<=1;
}
static NSDictionary *record(struct cb_state s){return @{@"Version":@1,@"Auto":@(s.prior_auto),@"UI":@(s.manual?s.target_ui:s.prior_ui)};}
static BOOL saveRecord(NSDictionary *d){
    if(![d writeToFile:statePath atomically:YES])return NO;
    return chmod(statePath.fileSystemRepresentation,0600)==0;
}
static void restoreTick(double now){
    int on=boolean(@"DisplayOn");
    if(on<0)return;
    /* Restore auto mode while locked/asleep, but do not send a nonzero panel
     * request while off. Keep the journal until normal display-on restoration. */
    if(on==0){autoSet([recovery[@"Auto"] boolValue]);restoreSent=NO;return;}
    if(!restoreSent){
        if(!setSlider([recovery[@"UI"] doubleValue])||!autoSet([recovery[@"Auto"] boolValue]))return;
        restoreSent=YES;return; /* Read-back on a later run-loop turn. */
    }
    if(boolean(@"DisplayBrightnessAuto")!=[recovery[@"Auto"] intValue]){restoreSent=NO;return;}
    if(![NSFileManager.defaultManager removeItemAtPath:statePath error:NULL])return;
    NSLog(@"redwood camera brightness: prior auto mode restored");
    recovery=nil;restoreSent=NO;state=(struct cb_state){0};readyAfter=now+1;
}
static void ruleTick(void){
    @autoreleasepool {
        double now=seconds(mach_absolute_time());
        if(!client){dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness",RTLD_NOW);client=[[NSClassFromString(@"BrightnessSystemClient") alloc] init];}
        if(!client)return;
        if(recovery){restoreTick(now);return;}
        if(now<readyAfter)return;
        uint64_t lease=0;if(notify_get_state(leaseToken,&lease)!=NOTIFY_STATUS_OK)lease=0;
        int detected=cameraFromSpringBoard();
        if(detected!=lastCamera){NSLog(@"redwood camera brightness: foreground Camera=%d",detected);lastCamera=detected;}
        struct cb_input input={.camera=detected>=0?detected!=0:cb_lease_valid(now,seconds(lease)),.enabled=access(disabledPath,F_OK)!=0,
            .display_on=boolean(@"DisplayOn"),.automatic=boolean(@"DisplayBrightnessAuto"),.ui=slider(),.now=now};
        struct cb_plan plan=cb_plan_step(state,input);
        if(plan.clear){recovery=record(state);restoreSent=NO;restoreTick(now);return;}
        NSDictionary *saved=plan.save?record(plan.next):nil;
        if(saved&&!saveRecord(saved)){NSLog(@"redwood camera brightness: could not save recovery; no override");return;}
        BOOL ok=(!plan.set_auto||autoSet(plan.auto_value))&&(!plan.set_ui||setSlider(plan.ui));
        if(!ok){
            recovery=[NSDictionary dictionaryWithContentsOfFile:statePath];restoreSent=NO;
            if(!validRecord(recovery))recovery=nil;
            NSLog(@"redwood camera brightness: write failed, restoring prior state");return;
        }
        if(!state.held&&plan.next.held)NSLog(@"redwood camera brightness: Camera hold70, prior auto=%d",plan.next.prior_auto);
        state=plan.next;
    }
}
static void heartbeat(void){
    BOOL on=foreground&&UIApplication.sharedApplication.applicationState!=UIApplicationStateBackground;
    notify_set_state(leaseToken,on?mach_absolute_time():0);notify_post(leaseName);
}
__attribute__((constructor)) static void start(void){
    NSString *bundle=NSBundle.mainBundle.bundleIdentifier;
    BOOL camera=[bundle isEqualToString:@"com.apple.camera"],springboard=[bundle isEqualToString:@"com.apple.springboard"];
    if(!camera&&!springboard)return;
    /* Guard loading from both tweak directories / multiple physical copies. */
    if(objc_lookUpClass("RWCameraBrightnessRuleGuard"))return;
    Class guard=objc_allocateClassPair(NSObject.class,"RWCameraBrightnessRuleGuard",0);if(!guard)return;objc_registerClassPair(guard);
    mach_timebase_info(&tb);
    dispatch_async(dispatch_get_main_queue(),^{
        if(notify_register_check(leaseName,&leaseToken)!=NOTIFY_STATUS_OK)return;
        if(camera){
            NSNotificationCenter *c=NSNotificationCenter.defaultCenter;
            [c addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(__unused NSNotification *n){foreground=YES;heartbeat();}];
            [c addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(__unused NSNotification *n){foreground=NO;heartbeat();}];
            foreground=UIApplication.sharedApplication.applicationState==UIApplicationStateActive;heartbeat();
        }else{
            recovery=[NSDictionary dictionaryWithContentsOfFile:statePath];
            if(recovery&&!validRecord(recovery)){NSLog(@"redwood camera brightness: invalid recovery record; rule not started");return;}
            if(!recovery&&access(statePath.fileSystemRepresentation,F_OK)==0){NSLog(@"redwood camera brightness: unreadable recovery record; rule not started");return;}
            ruleTick();
        }
        NSTimer *timer=[NSTimer timerWithTimeInterval:camera?.5:.25 repeats:YES block:^(__unused NSTimer *t){if(camera)heartbeat();else ruleTick();}];
        [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    });
}

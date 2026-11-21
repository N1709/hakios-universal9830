/* Native UIKit preview, scoped to Preferences and SpringBoard. This does not
 * advertise LocalAuthentication capability or inject BiometricKit results. */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <notify.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include "client.h"
@interface PSSpecifier:NSObject
+ (instancetype)preferenceSpecifierNamed:(NSString*)name target:(id)target set:(SEL)set get:(SEL)get detail:(Class)detail cell:(NSInteger)cell edit:(Class)edit;
- (id)propertyForKey:(NSString*)key;
- (void)setProperty:(id)value forKey:(NSString*)key;
@end
/* iOS 16.5 Preferences' own detail-controller contract, confirmed against the
 * local cache: PSViewController implements specifier/parent/root setters. */
@interface PSViewController:UIViewController
@end
static dispatch_queue_t requests;
static void request(const char *command,void (^done)(NSDictionary *)){
    NSString *copy=@(command);
    dispatch_async(requests,^{NSDictionary *reply=rw_fp_request(copy.UTF8String);
        dispatch_async(dispatch_get_main_queue(),^{if(done)done(reply);});});
}
static NSString *message(NSDictionary *s){
    if(!s)return @"Fingerprint service unavailable";
    NSString *p=s[@"phase"];
    if([p isEqual:@"registering"])return [NSString stringWithFormat:@"Lift and rest your finger · %@%%",s[@"progress"]?:@0];
    if([p isEqual:@"saved"])return @"Fingerprint Added";
    if([p isEqual:@"rejected"])return @"Try Again";
    if([p isEqual:@"unverified"])return @"Use Passcode";
    if([p isEqual:@"comparing"])return @"Rest Your Finger";
    if([p isEqual:@"unavailable"])return [s[@"message"] length]?s[@"message"]:@"Touch ID unavailable";
    return @"Place Your Finger";
}
static UIColor *tint(NSDictionary *s){
    if([s[@"phase"] isEqual:@"rejected"])return UIColor.systemRedColor;
    if([s[@"phase"] isEqual:@"saved"])return UIColor.systemGreenColor;
    return UIColor.labelColor;
}
@interface RWFingerprintPreviewController:PSViewController
@end
@implementation RWFingerprintPreviewController {
    UIImageView *finger,*sensorTarget;UILabel *heading,*detail;UIProgressView *progress;
    UIButton *enroll,*test,*enable;NSTimer *timer;BOOL visible,pending;uint64_t lastEvent;
}
- (void)viewDidLoad{
    [super viewDidLoad];self.title=@"Touch ID Preview";self.view.backgroundColor=UIColor.systemGroupedBackgroundColor;
    finger=[[UIImageView alloc]initWithImage:[UIImage systemImageNamed:@"touchid"]];
    finger.contentMode=UIViewContentModeScaleAspectFit;finger.tintColor=UIColor.systemRedColor;
    heading=[UILabel new];heading.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];heading.numberOfLines=2;heading.textAlignment=NSTextAlignmentCenter;
    heading.text=@"Place Your Finger";
    detail=[UILabel new];detail.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];detail.numberOfLines=0;detail.textAlignment=NSTextAlignmentCenter;
    detail.text=@"Enroll using the sensor near the bottom of the screen.\n\nFingerprint recognition is still in development. Unlock with your passcode.";
    progress=[UIProgressView new];progress.hidden=YES;
    enroll=[self button:@"Add a Fingerprint…" action:@selector(addFingerprint)];
    test=[self button:@"Try Fingerprint" action:@selector(compare)];
    enable=[self button:@"Enable Lock Screen Preview" action:@selector(togglePreview)];
    UIStackView *stack=[[UIStackView alloc]initWithArrangedSubviews:@[finger,heading,detail,progress,enroll,test,enable]];
    stack.axis=UILayoutConstraintAxisVertical;stack.spacing=22;stack.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[[stack.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:28],
        [stack.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-28],
        [stack.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:24],
        [finger.heightAnchor constraintEqualToConstant:88]]];
    sensorTarget=[[UIImageView alloc]initWithImage:[UIImage systemImageNamed:@"touchid"]];
    sensorTarget.tintColor=UIColor.systemRedColor;sensorTarget.contentMode=UIViewContentModeScaleAspectFit;
    sensorTarget.userInteractionEnabled=NO;[self.view addSubview:sensorTarget];
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc]initWithTitle:@"Cancel" style:UIBarButtonItemStylePlain target:self action:@selector(cancel)];
}
- (void)viewDidLayoutSubviews{
    [super viewDidLayoutSubviews];
    CGRect screen=UIScreen.mainScreen.bounds;
    CGPoint p=[self.view convertPoint:CGPointMake(CGRectGetMidX(screen),screen.size.height*(1860.0/2400.0)) fromView:nil];
    sensorTarget.frame=CGRectMake(p.x-22,p.y-22,44,44);
}
- (UIButton*)button:(NSString*)title action:(SEL)action{
    UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];[b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];[b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];return b;
}
- (void)viewDidAppear:(BOOL)animated{
    [super viewDidAppear:animated];[timer invalidate];visible=YES;[self poll];
    __weak typeof(self) weak=self;
    timer=[NSTimer scheduledTimerWithTimeInterval:.6 repeats:YES block:^(NSTimer *t){(void)t;[weak poll];}];
}
- (void)viewWillDisappear:(BOOL)animated{
    [super viewWillDisappear:animated];visible=NO;[timer invalidate];timer=nil;request("cancel",nil);
}
- (void)poll{
    if(pending||!visible)return;pending=YES;
    __weak typeof(self) weak=self;
    request("lease",^(NSDictionary *s){typeof(self) me=weak;if(!me)return;me->pending=NO;if(me->visible)[me render:s];});
}
- (void)render:(NSDictionary*)s{
    heading.text=message(s);finger.tintColor=tint(s);
    BOOL registering=[s[@"phase"] isEqual:@"registering"];
    progress.hidden=!registering;[progress setProgress:[s[@"progress"] floatValue]/100 animated:YES];
    BOOL busy=[s[@"busy"] boolValue];enroll.enabled=s&&!busy;test.enabled=s&&!busy&&[s[@"saved"] boolValue];
    [enroll setTitle:[s[@"saved"] boolValue]?@"Replace Fingerprint…":@"Add a Fingerprint…" forState:UIControlStateNormal];
    [enable setTitle:[s[@"enabled"] boolValue]?@"Disable Lock Screen Preview":@"Enable Lock Screen Preview" forState:UIControlStateNormal];
    enable.selected=[s[@"enabled"] boolValue];enable.enabled=s&&!busy;
    uint64_t e=[s[@"event"] unsignedLongLongValue];
    if(e!=lastEvent && [s[@"phase"] isEqual:@"rejected"]){
        CAKeyframeAnimation *shake=[CAKeyframeAnimation animationWithKeyPath:@"transform.translation.x"];
        shake.values=@[@0,@-10,@10,@-7,@7,@0];shake.duration=.35;[finger.layer addAnimation:shake forKey:@"retry"];
    }lastEvent=e;
}
- (void)addFingerprint{
    UIAlertController *a=[UIAlertController alertControllerWithTitle:@"Set Up Fingerprint" message:@"This replaces the preview fingerprint. The existing encrypted file will be backed up privately. Fingerprint unlock is not available yet; your passcode stays enabled." preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak typeof(self) weak=self;
    [a addAction:[UIAlertAction actionWithTitle:@"Continue" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){(void)action;request("enroll",^(NSDictionary *s){[weak render:s];});}]];
    [self presentViewController:a animated:YES completion:nil];
}
- (void)compare{__weak typeof(self) weak=self;request("compare",^(NSDictionary *s){[weak render:s];});}
- (void)cancel{__weak typeof(self) weak=self;request("cancel",^(NSDictionary *s){[weak render:s];});}
- (void)togglePreview{__weak typeof(self) weak=self;request(enable.selected?"disable":"enable",^(NSDictionary *s){[weak render:s];});}
@end

/* Settings root: add our own preview entry; leave native Face/Touch ID and
 * passcode controllers and their authentication requirements alone. */
static NSArray *(*originalSpecifiers)(id,SEL);
static BOOL installed;
static atomic_bool queued;
static void refreshRoot(UIViewController *controller,Class root,unsigned depth){
    if(!controller||depth>16)return;
    if([controller isKindOfClass:root]&&[controller respondsToSelector:NSSelectorFromString(@"reloadSpecifiers")]){
        [controller performSelector:NSSelectorFromString(@"reloadSpecifiers")];return;
    }
    for(UIViewController *child in controller.childViewControllers)refreshRoot(child,root,depth+1);
}
static void refreshSettings(Class root){
    for(UIWindow *window in UIApplication.sharedApplication.windows)refreshRoot(window.rootViewController,root,0);
    for(UIScene *scene in UIApplication.sharedApplication.connectedScenes){
        if(![scene isKindOfClass:UIWindowScene.class])continue;
        for(UIWindow *window in ((UIWindowScene*)scene).windows)refreshRoot(window.rootViewController,root,0);
    }
}
static NSArray *specifiers(id self,SEL cmd){
    NSArray *original=originalSpecifiers(self,cmd);if(![original isKindOfClass:NSArray.class])return original;
    NSMutableArray *items=[original mutableCopy];
    for(id item in items)if([item respondsToSelector:@selector(propertyForKey:)]&&[[item propertyForKey:@"id"] isEqual:@"redwood.fingerprint.preview"])return original;
    PSSpecifier *row=[PSSpecifier preferenceSpecifierNamed:@"Touch ID Preview" target:nil set:NULL get:NULL detail:RWFingerprintPreviewController.class cell:1 edit:Nil];
    [row setProperty:@"redwood.fingerprint.preview" forKey:@"id"];
    [row setProperty:[UIImage systemImageNamed:@"touchid"] forKey:@"iconImage"];
    NSUInteger at=items.count;
    for(NSUInteger i=0;i<items.count;i++)if([items[i] respondsToSelector:@selector(propertyForKey:)]){
        NSString *name=[items[i] propertyForKey:@"id"];
        if([name isEqual:@"PASSCODE"]||[name isEqual:@"TOUCHID_PASSCODE"]||[name isEqual:@"FACEID_PASSCODE"]){at=i+1;break;}
    }
    [items insertObject:row atIndex:at];
    Ivar ivar=class_getInstanceVariable(objc_getClass("PSListController"),"_specifiers");
    if(ivar&&ivar_getTypeEncoding(ivar)[0]=='@')object_setIvarWithStrongDefault(self,ivar,items);
    return items;
}
static void imageAdded(const struct mach_header *h,intptr_t slide){
    (void)h;(void)slide;if(atomic_exchange(&queued,true))return;
    dispatch_async(dispatch_get_main_queue(),^{atomic_store(&queued,false);if(installed)return;
        Class cls=objc_getClass("PSUIPrefsListController");Method m=class_getInstanceMethod(cls,@selector(specifiers));
        if(!m||method_getNumberOfArguments(m)!=2)return;
        char type[16];method_getReturnType(m,type,sizeof(type));if(type[0]!='@')return;
        originalSpecifiers=(void*)method_getImplementation(m);
        if(!class_addMethod(cls,@selector(specifiers),(IMP)specifiers,method_getTypeEncoding(m)))
            method_setImplementation(class_getInstanceMethod(cls,@selector(specifiers)),(IMP)specifiers);
        installed=YES;
        refreshSettings(cls);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_main_queue(),^{refreshSettings(cls);});
    });
}

/* Lock screen prompt: no hit testing, no key window, no unlock selector. */
static UIWindow *prompt;
static UIImageView *lockFinger;
static UILabel *lockText;
static BOOL awake,asked,requestPending;
static double attemptStarted;
static int (*lockState)(CFDictionaryRef);
static void lockPoll(void){
    if(requestPending)return;
    if(!awake||!lockState||lockState(NULL)!=1){prompt.hidden=YES;if(asked)request("cancel",nil);asked=NO;return;}
    requestPending=YES;
    request(asked?"lease":"status",^(NSDictionary *s){
        requestPending=NO;
        if(!awake||!lockState||lockState(NULL)!=1||![s[@"enabled"] boolValue]||![s[@"saved"] boolValue]){prompt.hidden=YES;return;}
        if(!asked && ![s[@"busy"] boolValue]){asked=YES;attemptStarted=NSProcessInfo.processInfo.systemUptime;request("compare",nil);}
        BOOL owns=[s[@"owns"] boolValue];
        prompt.hidden=NO;
        lockText.text=owns?message(s):((asked&&NSProcessInfo.processInfo.systemUptime-attemptStarted>1)?@"Use Passcode":@"Rest Your Finger");
        lockFinger.tintColor=owns&&[s[@"phase"] isEqual:@"rejected"]?UIColor.systemRedColor:UIColor.whiteColor;
    });
}
static void lockStart(void){
    void *mkb=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_NOW);
    lockState=mkb?dlsym(mkb,"MKBGetDeviceLockState"):NULL;
    UIWindowScene *scene=nil;
    for(UIScene *s in UIApplication.sharedApplication.connectedScenes)if([s isKindOfClass:UIWindowScene.class]){scene=(id)s;break;}
    /* SpringBoard may own ordinary windows without a UIWindowScene on this
     * port. Keep the prompt on its normal display in either case. */
    prompt=scene?[[UIWindow alloc]initWithWindowScene:scene]:[[UIWindow alloc]initWithFrame:UIScreen.mainScreen.bounds];
    prompt.frame=scene?scene.coordinateSpace.bounds:UIScreen.mainScreen.bounds;
    prompt.backgroundColor=UIColor.clearColor;prompt.windowLevel=UIWindowLevelAlert+1;prompt.userInteractionEnabled=NO;prompt.hidden=YES;
    UIViewController *controller=[UIViewController new];controller.view.backgroundColor=UIColor.clearColor;prompt.rootViewController=controller;
    CGFloat y=prompt.bounds.size.height*(1860.0/2400.0),x=prompt.bounds.size.width/2;
    lockFinger=[[UIImageView alloc]initWithImage:[UIImage systemImageNamed:@"touchid"]];
    lockFinger.frame=CGRectMake(x-24,y-24,48,48);lockFinger.tintColor=UIColor.whiteColor;
    lockText=[[UILabel alloc]initWithFrame:CGRectMake(20,y+30,prompt.bounds.size.width-40,50)];
    lockText.textAlignment=NSTextAlignmentCenter;lockText.numberOfLines=2;lockText.textColor=UIColor.whiteColor;lockText.font=[UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    [controller.view addSubview:lockFinger];[controller.view addSubview:lockText];
    static int token;
    if(notify_register_dispatch("com.apple.iokit.hid.displayStatus",&token,dispatch_get_main_queue(),^(int t){
        uint64_t on=0;awake=notify_get_state(t,&on)==NOTIFY_STATUS_OK&&on;
        if(!awake){prompt.hidden=YES;request("cancel",nil);asked=NO;}lockPoll();
    })!=NOTIFY_STATUS_OK)return;
    uint64_t on=0;awake=notify_get_state(token,&on)==NOTIFY_STATUS_OK&&on;
    [NSTimer scheduledTimerWithTimeInterval:.7 repeats:YES block:^(NSTimer *t){(void)t;lockPoll();}];
}
__attribute__((constructor))static void start(void){
    const char *name=getprogname();if(strcmp(name,"Preferences")&&strcmp(name,"SpringBoard"))return;
    requests=dispatch_queue_create("com.redwood.fingerprint.ui.requests",DISPATCH_QUEUE_SERIAL);
    if(!strcmp(name,"Preferences"))_dyld_register_func_for_add_image(imageAdded);
    else dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_main_queue(),^{lockStart();});
}

#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <stdatomic.h>
#include "config.h"
/* Distinct runtime class names let root-image and standalone copies coexist;
 * shared hook guards still install exactly one service and Settings row. */
#ifdef RW_ALWAYS_ON_ROOT_IMAGE
#define RWAlwaysOnController RWAlwaysOnImageController
#endif
@interface PSSpecifier:NSObject
+ (instancetype)preferenceSpecifierNamed:(NSString*)name target:(id)target set:(SEL)set get:(SEL)get detail:(Class)detail cell:(NSInteger)cell edit:(Class)edit;
- (id)propertyForKey:(NSString*)key;
- (void)setProperty:(id)value forKey:(NSString*)key;
@end
@interface PSListController:UIViewController
- (NSArray*)specifiers;
- (void)reloadSpecifiers;
@end
static void store(id self,NSArray *items){
    Ivar ivar=class_getInstanceVariable(PSListController.class,"_specifiers");
    if(ivar&&ivar_getTypeEncoding(ivar)[0]=='@')object_setIvarWithStrongDefault(self,ivar,items);
}
@interface RWAlwaysOnController:PSListController
@end
@implementation RWAlwaysOnController {
    int token;
}
- (NSArray*)specifiers{
    self.title=@"Always-On Display";
    PSSpecifier *group=[PSSpecifier preferenceSpecifierNamed:nil target:nil set:NULL get:NULL detail:Nil cell:0 edit:Nil];
    [group setProperty:@"Keep a dimmed Lock Screen visible after locking. Experimental on this device; battery use and low-power panel support are still being tested." forKey:@"footerText"];
    NSMutableArray *items=[NSMutableArray arrayWithObject:group];
    for(NSArray *row in @[@[@"Always-On Display",@"Enabled"],@[@"Show Wallpaper",@"ShowWallpaper"],@[@"Show Notifications",@"ShowNotifications"]]){
        PSSpecifier *s=[PSSpecifier preferenceSpecifierNamed:row[0] target:self set:@selector(setValue:specifier:) get:@selector(value:) detail:Nil cell:6 edit:Nil];
        [s setProperty:row[1] forKey:@"redwoodKey"];[s setProperty:@NO forKey:@"default"];
        [items addObject:s];
    }
    store(self,items);return items;
}
- (NSNumber*)value:(PSSpecifier*)s{return @(rw_aod_value(rw_aod_config(),[s propertyForKey:@"redwoodKey"]));}
- (void)setValue:(NSNumber*)value specifier:(PSSpecifier*)specifier{
    NSString *key=[specifier propertyForKey:@"redwoodKey"];
    int status=-1;uint64_t flags=0;
    if(notify_register_check(RW_AOD_SERVICE_STATUS,&status)==NOTIFY_STATUS_OK){notify_get_state(status,&flags);notify_cancel(status);}
    if([key isEqual:@"Enabled"]&&value.boolValue&&!(flags&1)){
        UIAlertController *a=[UIAlertController alertControllerWithTitle:@"Not Available" message:@"Always-On Display is not ready on this build." preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];[self presentViewController:a animated:YES completion:nil];
        [self reloadSpecifiers];return;
    }
    if(!rw_aod_set(key,value.boolValue)){
        UIAlertController *a=[UIAlertController alertControllerWithTitle:@"Unable to Save" message:@"The display preference could not be saved." preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];[self presentViewController:a animated:YES completion:nil];
    }
    [self reloadSpecifiers];
}
- (void)viewDidAppear:(BOOL)animated{
    [super viewDidAppear:animated];[self reloadSpecifiers];
    __weak typeof(self) weak=self;token=-1;
    notify_register_dispatch(RW_AOD_CHANGED,&token,dispatch_get_main_queue(),^(int t){(void)t;[weak reloadSpecifiers];});
}
- (void)viewWillDisappear:(BOOL)animated{
    [super viewWillDisappear:animated];if(token>=0)notify_cancel(token);token=-1;
}
@end
static NSArray*(*original)(id,SEL);
static BOOL hooked;
static atomic_bool queued;
static NSArray *rootSpecs(id self,SEL cmd){
    NSArray *base=original(self,cmd);if(![base isKindOfClass:NSArray.class])return base;
    NSMutableArray *items=[base mutableCopy];NSUInteger index=items.count;
    for(NSUInteger i=0;i<items.count;i++)if([items[i] respondsToSelector:@selector(propertyForKey:)]){
        NSString *key=[items[i] propertyForKey:@"id"];
        if([key isEqual:@"redwood.alwayson"])return base;
        if([key isEqual:@"DISPLAY_AND_BRIGHTNESS"])index=i+1;
    }
    PSSpecifier *row=[PSSpecifier preferenceSpecifierNamed:@"Always-On Display" target:nil set:NULL get:NULL detail:RWAlwaysOnController.class cell:1 edit:Nil];
    [row setProperty:@"redwood.alwayson" forKey:@"id"];
    [row setProperty:[UIImage systemImageNamed:@"sun.min"] forKey:@"iconImage"];
    [items insertObject:row atIndex:index];store(self,items);return items;
}
static void refresh(UIViewController *c,Class root,unsigned depth){
    if(!c||depth>16)return;
    if([c isKindOfClass:root]&&[c respondsToSelector:@selector(reloadSpecifiers)]){[(PSListController*)c reloadSpecifiers];return;}
    for(UIViewController *child in c.childViewControllers)refresh(child,root,depth+1);
}
static void install(void){
    if(hooked)return;Class root=objc_getClass("PSUIPrefsListController");
    const void *guard=(const void*)sel_registerName("redwoodAlwaysOnSettingsInstalled");
    if(root&&objc_getAssociatedObject((id)root,guard)){hooked=YES;return;}
    Method m=class_getInstanceMethod(root,@selector(specifiers));
    char type[16]={0};if(!m||method_getNumberOfArguments(m)!=2)return;method_getReturnType(m,type,sizeof(type));if(type[0]!='@')return;
    original=(void*)method_getImplementation(m);
    if(!class_addMethod(root,@selector(specifiers),(IMP)rootSpecs,method_getTypeEncoding(m)))method_setImplementation(class_getInstanceMethod(root,@selector(specifiers)),(IMP)rootSpecs);
    hooked=YES;objc_setAssociatedObject((id)root,guard,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    for(UIScene *s in UIApplication.sharedApplication.connectedScenes)if([s isKindOfClass:UIWindowScene.class])
        for(UIWindow *w in ((UIWindowScene*)s).windows)refresh(w.rootViewController,root,0);
}
static void imageAdded(const struct mach_header *h,intptr_t slide){
    (void)h;(void)slide;if(atomic_exchange(&queued,true))return;
    dispatch_async(dispatch_get_main_queue(),^{atomic_store(&queued,false);install();});
}
void rw_alwayson_settings_start(void){_dyld_register_func_for_add_image(imageAdded);}

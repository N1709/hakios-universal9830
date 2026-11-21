/* The iOS 16 home background is a poster composite (snapshot + blur/portals).
 * Move that whole home-only view using normal public CoreMotion device motion.
 * UIKit's gyro-only parallax path is not used. All UI work stays on main. */
#ifndef REDWOOD_WALLPAPER_MOTION_H
#define REDWOOD_WALLPAPER_MOTION_H
#include <math.h>
#include <mach/mach_time.h>
#include "wallpaper_pose.h"
struct rw_wall_affine { double a,b,c,d,tx,ty; };
struct rw_wall_t3d { double m[16]; };
struct rw_wall_rect { double x,y,w,h; };
struct rw_wall_point { double x,y; };
struct rw_wall_gravity { double x,y,z; };
#define WM_SEND(type,obj,name) ((type(*)(id,SEL))objc_msgSend)((id)(obj),sel_registerName(name))
#define WM_SEND1(type,obj,name,t,arg) ((type(*)(id,SEL,t))objc_msgSend)((id)(obj),sel_registerName(name),arg)
static const void *wallpaper_effect_key(void) { return (const void*)sel_registerName("redwoodHomePosterMotionV2"); }
static const void *wallpaper_transform_key(void) { return (const void*)sel_registerName("redwoodHomePosterOriginalTransformV2"); }
static const void *wallpaper_timer_key(void) { return (const void*)sel_registerName("redwoodHomePosterMotionTimerV2"); }
static bool wallpaper_kind(id object,const char *name)
{
    Class cls=objc_getClass(name);
    return object && cls && WM_SEND1(BOOL,object,"isKindOfClass:",Class,cls);
}
static id wallpaper_home_view(id view,unsigned depth)
{
    if(!view || depth>18) return nil;
    if(wallpaper_kind(WM_SEND(id,view,"nextResponder"),"PBUIPosterHomeViewController")) return view;
    id children=WM_SEND(id,view,"subviews");
    for(unsigned long i=0;i<WM_SEND(unsigned long,children,"count");i++) {
        id found=wallpaper_home_view(WM_SEND1(id,children,"objectAtIndex:",unsigned long,i),depth+1);
        if(found) return found;
    }
    return nil;
}
static id wallpaper_current_view(void)
{
    id app=WM_SEND(id,objc_getClass("UIApplication"),"sharedApplication");
    id windows=WM_SEND(id,app,"windows");
    for(unsigned long i=0;i<WM_SEND(unsigned long,windows,"count");i++) {
        id window=WM_SEND1(id,windows,"objectAtIndex:",unsigned long,i);
        if(!wallpaper_kind(window,"_SBWallpaperSecureWindow")) continue;
        id controller=WM_SEND(id,window,"rootViewController");
        id found=wallpaper_home_view(WM_SEND(id,controller,"view"),0);
        if(found) return found;
    }
    return nil;
}
/* Parallax belongs to Apple's own collection wallpapers; a photo or a picture laid over the
 * poster (custom_wallpaper.m's tag) stays still, which is also what costs nothing.
 * HomePosterMotionProviders (com.redwood.motion): comma-separated provider ids to allow. */
static id wallpaper_send_if(id obj,const char *name)
{
    if(!obj) return nil;
    SEL sel=sel_registerName(name);
    if(((BOOL(*)(id,SEL,SEL))objc_msgSend)(obj,sel_registerName("respondsToSelector:"),sel))
        return ((id(*)(id,SEL))objc_msgSend)(obj,sel);
    /* no getter: the instance variable, as key-value coding would read it */
    char ivar[96]; snprintf(ivar,sizeof(ivar),"_%s",name);
    for(Class k=object_getClass(obj);k;k=class_getSuperclass(k)) {
        Ivar iv=class_getInstanceVariable(k,ivar);
        if(iv) { const char *t=ivar_getTypeEncoding(iv); return t&&t[0]=='@'?object_getIvar(obj,iv):nil; }
    }
    return nil;
}
static void wallpaper_why(const char *why)
{
    static char last[160];
    if(strncmp(last,why,sizeof(last)-1)) { snprintf(last,sizeof(last),"%s",why); fprintf(stderr,"redwood: wallpaper motion v2: poster check: %s\n",why); }
}
static bool wallpaper_poster_moves(void)
{
    id app=WM_SEND(id,objc_getClass("UIApplication"),"sharedApplication");
    id windows=WM_SEND(id,app,"windows");
    for(unsigned long i=0;i<WM_SEND(unsigned long,windows,"count");i++) {
        id window=WM_SEND1(id,windows,"objectAtIndex:",unsigned long,i);
        if(WM_SEND1(id,window,"viewWithTag:",long,0x52575750)) { wallpaper_why("our still picture is shown"); return false; }
        if(!wallpaper_kind(window,"_SBWallpaperSecureWindow")) continue;
        id root=WM_SEND(id,window,"rootViewController");
        id inner=wallpaper_send_if(root,"viewController");
        if(!inner && root) { Ivar iv=class_getInstanceVariable(object_getClass(root),"_viewController"); if(iv) inner=object_getIvar(root,iv); }
        id provider=wallpaper_send_if(wallpaper_send_if(wallpaper_send_if(inner,"posterController"),"configuration"),"providerBundleIdentifier");
        if(!provider) {
            id pc=wallpaper_send_if(inner,"posterController"),cfg=wallpaper_send_if(pc,"configuration");
            wallpaper_why(!root?"no root controller":!inner?"no wallpaper controller":!pc?"no poster controller":!cfg?"no configuration":"no provider id");
            return false;
        }
        const char *p=WM_SEND(const char*,provider,"UTF8String");
        /* Apple's own posters move; the user's pictures (Photos, the legacy photo poster) stay still.
         * HomePosterMotionProviders, when set, replaces this rule with an exact list. */
        char allow[512]="";
        CFTypeRef v=CFPreferencesCopyAppValue(CFSTR("HomePosterMotionProviders"),CFSTR("com.redwood.motion"));
        if(v) { if(CFGetTypeID(v)==CFStringGetTypeID()) CFStringGetCString(v,allow,sizeof(allow),kCFStringEncodingUTF8); CFRelease(v); }
        if(!p) { wallpaper_why("?"); return false; }
        if(allow[0]) {
            for(char *tok=strtok(allow,", ");tok;tok=strtok(NULL,", ")) if(!strcmp(p,tok)) { wallpaper_why(p); return true; }
            wallpaper_why(p); return false;
        }
        bool apple=!strncmp(p,"com.apple.",10) && !strstr(p,"PhotosPosterProvider") && !strstr(p,"LegacyPoster");
        wallpaper_why(p);
        return apple;
    }
    wallpaper_why("no wallpaper window");
    return false;
}
static bool wallpaper_motion_enabled(void)
{
    bool (*reduce_motion)(void)=dlsym(RTLD_DEFAULT,"UIAccessibilityIsReduceMotionEnabled");
    if(!reduce_motion || reduce_motion()) return false;
    id process=WM_SEND(id,objc_getClass("NSProcessInfo"),"processInfo");
    if(WM_SEND(BOOL,process,"isLowPowerModeEnabled")) return false;
    CFTypeRef value=CFPreferencesCopyAppValue(CFSTR("HomePosterMotionEnabled"),CFSTR("com.redwood.motion"));
    bool enabled=true;
    if(value) {
        if(CFGetTypeID(value)==CFBooleanGetTypeID()) enabled=CFBooleanGetValue(value);
        else if(CFGetTypeID(value)==CFNumberGetTypeID()) {
            int n=0;enabled=CFNumberGetValue(value,kCFNumberIntType,&n)&&n!=0;
        } else enabled=false;
        CFRelease(value);
    }
    return enabled;
}
/* Live tunables (com.redwood.motion, re-read every second): HomePosterMotionAmplitude (pt, 40),
 * HomePosterMotionRange (degrees of tilt for full travel, 15), HomePosterMotionRecenter (s, 3). */
static double wallpaper_pref(CFStringRef key,double fallback,double lo,double hi)
{
    CFTypeRef v=CFPreferencesCopyAppValue(key,CFSTR("com.redwood.motion"));
    double d=fallback;
    if(v) { if(CFGetTypeID(v)==CFNumberGetTypeID()) CFNumberGetValue(v,kCFNumberDoubleType,&d); CFRelease(v); }
    return isfinite(d)?fmax(lo,fmin(hi,d)):fallback;
}
static double wallpaper_scale=1.15;
static bool wallpaper_original(id view,struct rw_wall_affine *original)
{
    CFDataRef saved=(CFDataRef)objc_getAssociatedObject(view,wallpaper_transform_key());
    if(!saved || CFDataGetLength(saved)!=sizeof(*original)) return false;
    CFDataGetBytes(saved,CFRangeMake(0,sizeof(*original)),(UInt8*)original);
    return true;
}
static void wallpaper_motion_apply(id view,bool enabled)
{
    if(!view) return;
    bool attached=objc_getAssociatedObject(view,wallpaper_effect_key())!=nil;
    if(!enabled) {
        if(!attached) return;
        struct rw_wall_affine original;
        if(wallpaper_original(view,&original)) WM_SEND1(void,view,"setTransform:",struct rw_wall_affine,original);
        struct rw_wall_t3d identity={{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
        WM_SEND1(void,WM_SEND(id,view,"layer"),"setSublayerTransform:",struct rw_wall_t3d,identity);
        objc_setAssociatedObject(view,wallpaper_effect_key(),nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        objc_setAssociatedObject(view,wallpaper_transform_key(),nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return;
    }
    if(attached) return;
    struct rw_wall_affine original=WM_SEND(struct rw_wall_affine,view,"transform");
    CFDataRef saved=CFDataCreate(NULL,(const UInt8*)&original,sizeof(original));
    objc_setAssociatedObject(view,wallpaper_transform_key(),(id)saved,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    CFRelease(saved);
    objc_setAssociatedObject(view,wallpaper_effect_key(),(id)kCFBooleanTrue,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
/* Move the layer's sublayers, not the view: a view transform makes PosterBoard's
 * PBUIEffectTrackingReplicaView lay itself out against it and cancel the movement. */
/* Smooth and cheap: the 20 Hz sensor step is handed to Core Animation as a short linear
 * animation from where the picture is now (presentation layer) to the new place, so the render
 * server glides it at the display's own rate and SpringBoard does one small update per sample. */
static void wallpaper_motion_translate(id view,double x,double y)
{
    if(!view) return;
    struct rw_wall_t3d t={{wallpaper_scale,0,0,0, 0,wallpaper_scale,0,0, 0,0,1,0, x,y,0,1}};
    id layer=WM_SEND(id,view,"layer");
    id presentation=WM_SEND(id,layer,"presentationLayer");
    Class anim=objc_getClass("CABasicAnimation"),value=objc_getClass("NSValue");
    if(presentation && anim && value) {
        struct rw_wall_t3d from=WM_SEND(struct rw_wall_t3d,presentation,"sublayerTransform");
        id a=WM_SEND1(id,anim,"animationWithKeyPath:",id,(id)CFSTR("sublayerTransform"));
        WM_SEND1(void,a,"setFromValue:",id,WM_SEND1(id,value,"valueWithCATransform3D:",struct rw_wall_t3d,from));
        WM_SEND1(void,a,"setToValue:",id,WM_SEND1(id,value,"valueWithCATransform3D:",struct rw_wall_t3d,t));
        WM_SEND1(void,a,"setDuration:",double,.066);
        WM_SEND1(void,layer,"setSublayerTransform:",struct rw_wall_t3d,t);
        ((void(*)(id,SEL,id,id))objc_msgSend)(layer,sel_registerName("addAnimation:forKey:"),a,(id)CFSTR("redwoodParallax"));
        return;
    }
    WM_SEND1(void,layer,"setSublayerTransform:",struct rw_wall_t3d,t);
}
static bool wallpaper_visible(id view)
{
    if(!view || !WM_SEND(id,view,"window")) return false;
    for(id v=view;v;v=WM_SEND(id,v,"superview"))
        if(WM_SEND(BOOL,v,"isHidden") || WM_SEND(double,v,"alpha")<.01) return false;
    return true;
}
static void wallpaper_motion_start(void)
{
    static dispatch_source_t timer;
    static id manager,current;
    static bool active,allowed;
    static double amplitude,x,y,last_clock,last_sample,scan_at;
    static unsigned samples;
    static struct rw_wall_pose pose;
    static int display_token=-1,status_token;
    static mach_timebase_info_data_t tb;
    Class app_class=objc_getClass("UIApplication");
    if(timer || !app_class || objc_getAssociatedObject((id)app_class,wallpaper_timer_key()) || access("/dev/redwood-sensors",F_OK)) return;
    char args[1024]={0};size_t n=sizeof(args)-1;
    if(!sysctlbyname("kern.bootargs",args,&n,NULL,0) && strstr(args,"sens_wallpaper_motion=0")) return;
    /* Retire the earlier experimental image-only effect if an older built-in
     * bridge still runs it. Preserve an explicit user disable on migration. */
    CFTypeRef old=CFPreferencesCopyAppValue(CFSTR("WallpaperMotionEnabled"),CFSTR("com.redwood.motion"));
    CFTypeRef setting=CFPreferencesCopyAppValue(CFSTR("HomePosterMotionEnabled"),CFSTR("com.redwood.motion"));
    if(!setting) CFPreferencesSetAppValue(CFSTR("HomePosterMotionEnabled"),old?old:kCFBooleanTrue,CFSTR("com.redwood.motion"));
    if(old) CFRelease(old);if(setting)CFRelease(setting);
    CFPreferencesSetAppValue(CFSTR("WallpaperMotionEnabled"),kCFBooleanFalse,CFSTR("com.redwood.motion"));
    CFPreferencesAppSynchronize(CFSTR("com.redwood.motion"));
    dlopen("/System/Library/Frameworks/CoreMotion.framework/CoreMotion",RTLD_NOW|RTLD_LOCAL);
    Class cls=objc_getClass("CMMotionManager");
    if(!cls) return;
    manager=WM_SEND(id,WM_SEND(id,cls,"alloc"),"init");
    WM_SEND1(void,manager,"setDeviceMotionUpdateInterval:",double,.05);
    if(notify_register_check("com.apple.iokit.hid.displayStatus",&display_token)!=NOTIFY_STATUS_OK) return;
    mach_timebase_info(&tb);
    timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),50*NSEC_PER_MSEC,5*NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer,^{
        double now=mach_absolute_time()*(double)tb.numer/tb.denom/1e9;
        if(now-scan_at>1) {
            scan_at=now;
            {   static int last_moves=-1; bool moves=wallpaper_poster_moves();
                allowed=wallpaper_motion_enabled() && moves;
                if(moves!=last_moves) { last_moves=moves; fprintf(stderr,"redwood: wallpaper motion v2: poster %s\n",moves?"is Apple's collection: parallax on":"is a still picture: parallax off"); } }
            CFPreferencesAppSynchronize(CFSTR("com.redwood.motion"));
            amplitude=wallpaper_pref(CFSTR("HomePosterMotionAmplitude"),40,0,60);
            pose.range=wallpaper_pref(CFSTR("HomePosterMotionRange"),15,3,60)*M_PI/180;
            pose.tau=wallpaper_pref(CFSTR("HomePosterMotionRecenter"),3,0,60);
            id found=wallpaper_current_view();
            if(found!=current) {
                wallpaper_motion_apply(current,false);WM_SEND(void,current,"release");
                current=WM_SEND(id,found,"retain");pose.ready=false;x=y=0;
                if(current) fprintf(stderr,"redwood: wallpaper motion v2: found home poster composite %p\n",current);
            }
        }
        uint64_t display=0;notify_get_state(display_token,&display);
        bool wanted=allowed && display!=0 && wallpaper_visible(current);
        if(wanted!=active) {
            active=wanted;pose.ready=false;x=y=0;last_sample=0;
            if(active) WM_SEND1(void,manager,"startDeviceMotionUpdatesUsingReferenceFrame:",unsigned long,1);
            else {WM_SEND(void,manager,"stopDeviceMotionUpdates");wallpaper_motion_apply(current,false);}
            fprintf(stderr,"redwood: wallpaper motion v2: fused updates %s\n",active?"on":"off");
        }
        double dt=last_clock?fmin(.25,fmax(0,now-last_clock)):.05;last_clock=now;
        if(!active) return;
        id motion=WM_SEND(id,manager,"deviceMotion");
        if(!motion) return;
        double timestamp=WM_SEND(double,motion,"timestamp");
        if(timestamp!=last_sample) {
            struct rw_wall_gravity g=WM_SEND(struct rw_wall_gravity,motion,"gravity");
            struct rw_wall_rect b=WM_SEND(struct rw_wall_rect,current,"bounds");
            /* overscan so the moved picture never shows an edge */
            wallpaper_scale=b.w>0?1+2.2*amplitude/fmin(b.w,b.h):1.15;
            if(rw_wall_pose_update(&pose,timestamp,g.x,g.y,g.z,amplitude)) samples++;
            last_sample=timestamp;
        }
        if(!pose.ready) return;
        double ease=1-exp(-dt/.12),nx=x+(pose.x-x)*ease,ny=y+(pose.y-y)*ease;
        /* Rest unless the tilt really moved the picture. Every update is an animation that makes the
         * render server redraw the whole layered poster at display rate, and a hand's tremor kept that
         * going for ever (1 000+ GPU jobs a second on Apple's posters against ~65 at rest, and the phone
         * at 96-99 C). Start moving past 2.5 pt from where the picture is, settle within 0.5 pt. */
        static bool moving;
        static double tx,ty;   /* the target, low-passed (0.4 s): at 40 pt over 15 deg one degree is ~3 pt,
                                * and a hand's tremor and the sensor's noise are a degree or two */
        double lp=1-exp(-dt/.4);
        tx+=(pose.x-tx)*lp; ty+=(pose.y-ty)*lp;
        nx=x+(tx-x)*ease; ny=y+(ty-y)*ease;
        double gap=fabs(tx-x)+fabs(ty-y);
        if(objc_getAssociatedObject(current,wallpaper_effect_key())) {
            if(!moving && gap<2.5) return;
            moving=gap>=.5;
            if(!moving) return;
        }
        x=nx;y=ny;
        wallpaper_motion_apply(current,true);wallpaper_motion_translate(current,x,y);
    });
    dispatch_resume(timer);
    objc_setAssociatedObject((id)app_class,wallpaper_timer_key(),(id)kCFBooleanTrue,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    notify_register_dispatch("com.redwood.wallpaper.motion.status",&status_token,dispatch_get_main_queue(),^(__unused int t){
        id layer=current?WM_SEND(id,current,"layer"):nil;
        id presentation=layer?WM_SEND(id,layer,"presentationLayer"):nil;
        struct rw_wall_point point=presentation?WM_SEND(struct rw_wall_point,presentation,"position"):(struct rw_wall_point){0};
        struct rw_wall_affine transform=presentation?WM_SEND(struct rw_wall_affine,presentation,"affineTransform"):(struct rw_wall_affine){0};
        struct rw_wall_t3d sub=presentation?WM_SEND(struct rw_wall_t3d,presentation,"sublayerTransform"):(struct rw_wall_t3d){{0}};transform.tx=sub.m[12];transform.ty=sub.m[13];
        fprintf(stderr,"redwood: wallpaper motion v2: view=%p active=%d samples=%u offset=(%.2f,%.2f) presentation=(%.2f,%.2f) matrix_translation=(%.2f,%.2f)\n",current,active,samples,x,y,point.x,point.y,transform.tx,transform.ty);
    });
}
#undef WM_SEND
#undef WM_SEND1
#endif

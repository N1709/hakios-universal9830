/* Port-side table pickup wake without an Apple SPU gesture stream.
 * Real CoreMotion samples; same wake event as the port's Tap to Wake.
 * --probe never wakes, --once tests a pickup, --daemon runs the service. */
#import <Foundation/Foundation.h>
#import <CoreMotion/CoreMotion.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <notify.h>
#include <objc/message.h>
#include <stdio.h>
#include <unistd.h>
#include "raise_policy.h"
#include "redwood_input.h"
#include "../../tools/aod/state.h"

static void pump(double seconds)
{
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

static int display_on(id client)
{
    id value=((id(*)(id,SEL,id))objc_msgSend)(client,sel_registerName("copyPropertyForKey:"),@"DisplayOn");
    int on=-1;
    if(value && (CFGetTypeID((CFTypeRef)value)==CFBooleanGetTypeID() ||
        CFGetTypeID((CFTypeRef)value)==CFNumberGetTypeID())) on=[value boolValue];
    [value release];
    return on;
}

/* Low Power Mode suspends raise-to-wake (no sensor subscriptions at all). */
static bool low_power(void)
{
    static int token=-1; uint64_t on=0;
    if(token<0 && notify_register_check("com.apple.system.lowpowermode",&token)!=NOTIFY_STATUS_OK) return false;
    return notify_get_state(token,&on)==NOTIFY_STATUS_OK && on;
}

static bool lift_enabled(void)
{
    if(low_power()) return false;
    CFTypeRef value=CFPreferencesCopyValue(CFSTR("SBSupportLiftToWake"),
        CFSTR("com.apple.springboard"),CFSTR("mobile"),kCFPreferencesAnyHost);
    bool enabled=true; /* SBIdleTimerDefaults' native default on 20F66. */
    if(value) {
        if(CFGetTypeID(value)==CFBooleanGetTypeID()) enabled=CFBooleanGetValue(value);
        else if(CFGetTypeID(value)==CFNumberGetTypeID()) {
            int n=0; enabled=CFNumberGetValue(value,kCFNumberIntType,&n) && n!=0;
        } else enabled=false;
        CFRelease(value);
    }
    return enabled;
}

/* Native AOD (backlight InactiveOn) keeps DisplayOn/displayStatus set, so
 * those alone read "awake". Both the requested and the completed backlight
 * state must be inactive: never press the side button on an awake screen. */
static bool aod_asleep(int aod_token)
{
    uint64_t flags=0;
    return aod_token>=0 && notify_get_state(aod_token,&flags)==NOTIFY_STATUS_OK &&
        rw_aod_idle_suppressed(flags) && (flags&RW_AOD_COMPLETED_VALID) &&
        rw_aod_inactive((long long)((flags>>12)&15));
}

static bool wake_display(id client,int display_token,int blanked_token,int aod_token)
{
    uint64_t display=1,blanked=0;
    if(lift_enabled() && aod_asleep(aod_token)) goto press;
    /* Independent, fresh native state guards against locking an already-woken
     * screen. This only wakes; it never dismisses or unlocks the lock screen. */
    if(display_on(client)!=0 || !lift_enabled() ||
        notify_get_state(display_token,&display)!=NOTIFY_STATUS_OK || display!=0 ||
        notify_get_state(blanked_token,&blanked)!=NOTIFY_STATUS_OK || blanked!=1) return false;
press:;
    int fd=open(REDWOOD_TOUCH_DEVICE,O_WRONLY);
    if(fd<0) { perror("motion wake: input device"); return false; }
    struct redwood_touch event={.type=REDWOOD_BUTTON_DOWN,.finger=REDWOOD_BUTTON_LOCK};
    bool sent=write(fd,&event,sizeof(event))==(ssize_t)sizeof(event);
    /* Release even if down failed; never leave a held side button. */
    event.type=REDWOOD_BUTTON_UP;
    bool released=write(fd,&event,sizeof(event))==(ssize_t)sizeof(event);
    close(fd);
    return sent && released;
}

int main(int argc,char **argv)
{
    bool daemon=argc==2 && !strcmp(argv[1],"--daemon");
    bool once=argc==2 && !strcmp(argv[1],"--once");
    bool probe=argc==2 && !strcmp(argv[1],"--probe");
    if(!daemon && !once && !probe) {
        fprintf(stderr,"usage: %s --probe|--once|--daemon (probes: 120 seconds)\n",argv[0]);
        return 2;
    }
    if(access("/dev/redwood-sensors",F_OK)!=0) return 0;
    @autoreleasepool {
        dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness",RTLD_NOW);
        id client=[[NSClassFromString(@"BrightnessSystemClient") alloc] init];
        if(!client) return 1;
        int display_token=-1,blanked_token=-1,aod_token=-1;
        (void)notify_register_check(RW_AOD_SERVICE_STATUS,&aod_token);
        if(notify_register_check("com.apple.iokit.hid.displayStatus",&display_token)!=NOTIFY_STATUS_OK ||
            notify_register_check("com.apple.springboard.hasBlankedScreen",&blanked_token)!=NOTIFY_STATUS_OK) return 1;
        CMMotionManager *manager=[CMMotionManager new];
        if(!manager.accelerometerAvailable || !manager.gyroAvailable) return 1;
        manager.accelerometerUpdateInterval=.05;
        manager.gyroUpdateInterval=.05;
        struct redwood_raise policy={0};
        double previous=0,report=0,gx=0,gy=0,gz=0;
        double until=[NSProcessInfo processInfo].systemUptime+120;
        unsigned samples=0,detections=0;
        bool active=false,verified=false;
        int last_on=-2;
        while(daemon || [NSProcessInfo processInfo].systemUptime<until) {
            @autoreleasepool {
                /* Pump: otherwise BrightnessSystemClient's display cache stays
                 * at its initial value even after the screen turns off. */
                pump(daemon?.1:.05);
                int on=display_on(client);
                if(on!=0 && aod_asleep(aod_token)) on=0; /* AOD counts as asleep */
                bool enabled=lift_enabled();
                if(on!=last_on) {
                    printf("motion wake: display=%d enabled=%d\n",on,enabled); fflush(stdout);
                    last_on=on;
                }
                bool wanted=!daemon || (on==0 && enabled);
                if(active!=wanted) {
                    if(wanted) { [manager startAccelerometerUpdates]; [manager startGyroUpdates]; }
                    else { [manager stopGyroUpdates]; [manager stopAccelerometerUpdates]; }
                    active=wanted;
                    previous=0;
                    double cooldown=policy.cooldown;
                    policy=(struct redwood_raise){.cooldown=cooldown};
                    gx=gy=gz=0;
                    printf("motion wake: sensor subscriptions %s\n",active?"on":"off"); fflush(stdout);
                }
                if(!active) { pump(.4); continue; }
                CMAccelerometerData *m=manager.accelerometerData;
                CMGyroData *gyro=manager.gyroData;
                if(!m || !gyro || m.timestamp<=previous) continue;
                double dt=previous ? m.timestamp-previous : 0;
                previous=m.timestamp; samples++;
                CMAcceleration raw=m.acceleration;
                double alpha=dt>0 && dt<.75 ? 1-exp(-dt/.2) : 1;
                gx+=alpha*(raw.x-gx); gy+=alpha*(raw.y-gy); gz+=alpha*(raw.z-gz);
                CMRotationRate r=gyro.rotationRate;
                bool fresh=fabs(gyro.timestamp-m.timestamp)<.75;
                double speed=sqrt(r.x*r.x+r.y*r.y+r.z*r.z);
                double acceleration=sqrt(pow(raw.x-gx,2)+pow(raw.y-gy,2)+pow(raw.z-gz,2));
                bool detected=redwood_raise_sample(&policy,m.timestamp,on==0&&enabled&&fresh,
                    gx,gy,gz,speed,acceleration);
                if(detected) {
                    detections++;
                    bool sent=!probe && wake_display(client,display_token,blanked_token,aod_token);
                    printf("motion wake: table pickup; wake event=%s\n",probe?"disabled (probe)":sent?"sent":"not sent"); fflush(stdout);
                    if(sent) {
                        pump(2);
                        verified=display_on(client)==1 && !aod_asleep(aod_token);
                        printf("motion wake: display-on verified=%d\n",verified); fflush(stdout);
                    }
                    if(once) break;
                }
                if((!daemon || getenv("REDWOOD_MOTION_DEBUG")) && m.timestamp-report>=1) {
                    report=m.timestamp;
                    printf("samples=%u display=%d enabled=%d armed=%d gravity=(%.2f,%.2f,%.2f) gyro_rad_s=%.3f\n",
                        samples,on,enabled,policy.armed,gx,gy,gz,speed); fflush(stdout);
                }
            }
        }
        [manager stopGyroUpdates]; [manager stopAccelerometerUpdates];
        [manager release]; [client release];
        notify_cancel(display_token); notify_cancel(blanked_token);
        if(aod_token>=0) notify_cancel(aod_token);
        printf("result: samples=%u pickups=%u wake_verified=%d\n",samples,detections,verified);
        return once ? (verified?0:1) : samples?0:1;
    }
}

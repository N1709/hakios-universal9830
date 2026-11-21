/* Explicit manual display diagnostic. Default/status reads only.
 * --set70 disables ALS-driven auto brightness through the native controller and
 * requests UI70% through the port's documented legacy slider curve. Thermal
 * protection remains active. Does not change camera AE, AF or ISP values. */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <math.h>
static id property(id client,NSString *key) {
    CFTypeRef v=((CFTypeRef(*)(id,SEL,CFStringRef))objc_msgSend)(client,sel_registerName("copyPropertyForKey:"),(__bridge CFStringRef)key);
    return CFBridgingRelease(v);
}
static NSDictionary *parameters(io_service_t bl) {
    id p=CFBridgingRelease(IORegistryEntryCreateCFProperty(bl,CFSTR("IODisplayParameters"),NULL,0));
    if(![p isKindOfClass:NSDictionary.class] || ![p[@"brightness"] isKindOfClass:NSDictionary.class])return nil;
    return p[@"brightness"];
}
static int automatic(id client) {
    id v=property(client,@"DisplayBrightnessAuto");
    return [v isKindOfClass:NSNumber.class]?[v boolValue]:-1;
}
int main(int argc,char **argv) {@autoreleasepool {
    BOOL set=argc==2&&!strcmp(argv[1],"--set70");
    BOOL autoOn=argc==2&&!strcmp(argv[1],"--auto-on");
    BOOL autoOff=argc==2&&!strcmp(argv[1],"--auto-off");
    BOOL sliderOnly=argc==3&&!strcmp(argv[1],"--slider");
    double target=.7;
    if(sliderOnly){char *end=NULL;target=strtod(argv[2],&end);if(!end||end==argv[2]||*end||!isfinite(target)||target<0||target>1)return 2;}
    if(argc>3 || (argc==3&&!sliderOnly) || (argc==2&&!set&&!autoOn&&!autoOff&&strcmp(argv[1],"--status")))return 2;
    dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness",RTLD_NOW);
    id client=[[NSClassFromString(@"BrightnessSystemClient") alloc] init];
    io_service_t bl=IOServiceGetMatchingService(0,IOServiceMatching("AppleARMBacklight"));
    if(!client||!bl)return 1;
    NSDictionary *p=parameters(bl);
    if(![p[@"max"] isKindOfClass:NSNumber.class] || ![p[@"value"] isKindOfClass:NSNumber.class])return 1;
    double max=[p[@"max"] doubleValue];
    if(!isfinite(max)||max<=0||max>UINT32_MAX)return 1;
    if(autoOn||autoOff){
        BOOL accepted=((BOOL(*)(id,SEL,id,id))objc_msgSend)(client,sel_registerName("setProperty:forKey:"),@(autoOn),@"DisplayBrightnessAuto");
        if(!accepted)return 1;
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
        p=parameters(bl);
    }
    if(set||sliderOnly) {
        int before=automatic(client);
        if(before<0)return 1;
        fprintf(stderr,"before: auto=%d raw=%u/%u\n",before,[p[@"value"] unsignedIntValue],(unsigned)max);
        BOOL accepted=!set||((BOOL(*)(id,SEL,id,id))objc_msgSend)(client,sel_registerName("setProperty:forKey:"),@NO,@"DisplayBrightnessAuto");
        if(!accepted){fprintf(stderr,"native auto-off request refused\n");return 1;}
        kern_return_t(*writeProps)(io_registry_entry_t,CFTypeRef)=dlsym(RTLD_DEFAULT,"IORegistryEntrySetCFProperties");
        if(!writeProps)return 1;
        uint32_t raw=(uint32_t)llround((.025+.95*target)*max);
        kern_return_t result=writeProps(bl,(__bridge CFTypeRef)@{@"brightness":@(raw)});
        if(result){fprintf(stderr,"manual brightness write failed: %#x\n",result);return 1;}
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
        p=parameters(bl);
    }
    double raw=[p[@"value"] doubleValue],ui=(raw/max-.025)/.95;
    int state=automatic(client);
    printf("auto=%d driver_raw=%.0f/%.0f requested_ui_percent=%.2f\n",state,raw,max,100*ui);
    IOObjectRelease(bl);
    return ((set&&(state!=0||fabs(ui-target)>.003))||(sliderOnly&&fabs(ui-target)>.003)||(autoOn&&state!=1)||(autoOff&&state!=0))?1:0;
}}

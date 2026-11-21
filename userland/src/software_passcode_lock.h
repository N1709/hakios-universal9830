#ifndef REDWOOD_SOFTWARE_PASSCODE_LOCK_H
#define REDWOOD_SOFTWARE_PASSCODE_LOCK_H
#include <stdbool.h>
#include <stdio.h>
/* SEP-less software-passcode policy: require native authentication after every
 * display-off. The port's normal lock path can leave SpringBoard authenticated
 * after the underlying keybag locks. No credential handling or unlock shortcut.
 */
#include <CoreFoundation/CoreFoundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <string.h>

static void revoke_on_display_off(int token) {
    uint64_t on=1;
    if(notify_get_state(token,&on)!=NOTIFY_STATUS_OK || on) return;
    id app=((id(*)(id,SEL))objc_msgSend)((id)objc_getClass("UIApplication"),sel_registerName("sharedApplication"));
    SEL controller=sel_registerName("authenticationController");
    if(!app || !class_respondsToSelector(object_getClass(app),controller)) return;
    id auth=((id(*)(id,SEL))objc_msgSend)(app,controller);
    SEL revoke=sel_registerName("revokeAuthenticationImmediatelyForPublicReason:");
    if(!auth || !class_respondsToSelector(object_getClass(auth),revoke)) return;
    /* Native implementation checks hasPasscodeSet, locks the keybag, clears
     * its cached lock state and revokes authentication. It never grants it. */
    ((void(*)(id,SEL,id))objc_msgSend)(auth,revoke,(id)CFSTR("Redwood software passcode: display locked"));
    fprintf(stderr,"redwood: software passcode: native authentication revoked on display-off\n");
}
static void software_passcode_lock_start(void) {
    char args[1024]={0};size_t n=sizeof(args)-1;
    if(sysctlbyname("kern.bootargs",args,&n,NULL,0)) return;
    bool enabled=false;char *save=NULL;
    for(char *s=strtok_r(args," ",&save);s;s=strtok_r(NULL," ",&save))
        if(!strcmp(s,"softpasscode=1"))enabled=true;
    if(!enabled)return;
    Class app=objc_getClass("UIApplication");
    const void *key=(const void *)sel_registerName("redwoodSoftwarePasscodePolicy");
    if(!app || objc_getAssociatedObject((id)app,key))return;
    static int token;
    if(notify_register_dispatch("com.apple.iokit.hid.displayStatus",&token,dispatch_get_main_queue(),^(int t){
        revoke_on_display_off(t);
    })!=NOTIFY_STATUS_OK) return;
    objc_setAssociatedObject((id)app,key,(id)kCFBooleanTrue,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    fprintf(stderr,"redwood: software passcode: immediate-lock policy registered (event-driven)\n");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_main_queue(),^{revoke_on_display_off(token);});
}

#endif

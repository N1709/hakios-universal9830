/* Temporary diagnostic alert and Metal availability check; no account UI reads. */
#import <UIKit/UIKit.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
static void check(void) {
        if(UIApplication.sharedApplication.applicationState!=UIApplicationStateActive)return;
        static BOOL done; if(done)return; done=YES;
        void *metal=dlopen("/System/Library/Frameworks/Metal.framework/Metal",RTLD_NOW);
        id (*create)(void)=metal?(void *)dlsym(metal,"MTLCreateSystemDefaultDevice"):NULL;
        id device=create?create():nil;
        NSDictionary *report=@{@"metal_available":@(device!=nil), @"diagnostic_only":@YES};
        NSData *bytes=[NSJSONSerialization dataWithJSONObject:report options:0 error:NULL];
        int fd=open("/var/mobile/Library/RedwoodAppleAccountLab/settings-render-probe.json",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
        if(fd>=0){if(bytes)write(fd,bytes.bytes,bytes.length);close(fd);}
        UIViewController *top=nil;
        for(UIWindowScene *scene in UIApplication.sharedApplication.connectedScenes)
            if([scene isKindOfClass:UIWindowScene.class])for(UIWindow *window in scene.windows)
                if(window.isKeyWindow)top=window.rootViewController;
        while(top.presentedViewController)top=top.presentedViewController;
        if(!top || [top isKindOfClass:UIAlertController.class])return;
        UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Rendering check"
            message:@"This is a display test. Text, buttons and the blurred background should remain readable. No sign-in was attempted."
            preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [top presentViewController:alert animated:YES completion:nil];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,25*NSEC_PER_SEC),dispatch_get_main_queue(),^{
            if(alert.presentingViewController)[alert dismissViewControllerAnimated:YES completion:nil];
        });
}
__attribute__((constructor)) static void start(void) {
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(__unused NSNotification *note){
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{check();});
        }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),dispatch_get_main_queue(),^{check();});
}

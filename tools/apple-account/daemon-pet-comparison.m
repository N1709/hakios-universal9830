/* Observe native akd PET getter without changing return value/admission checks.
 * Optional owner-armed one-shot sends only the native result tuple privately;
 * remote observer waits for the real phone iCloud401 before testing on Mac. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "pet-comparison-trigger.h"
static id (*originalPET)(id,SEL);
static int comparisonFD=-1;
@interface NSObject (DaemonPETComparison)
- (id)appleID;
- (id)dsid;
- (id)altDSID;
- (id)stringValue;
- (id)passwordEquivalentToken;
@end
static void comparisonRecord(NSDictionary *entry) {
    NSMutableDictionary *tagged=entry.mutableCopy;tagged[@"pid"]=@(getpid());
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:tagged options:0 error:NULL];
    if(comparisonFD>=0 && bytes)@synchronized(rw_comparison_lock()) {write(comparisonFD,bytes.bytes,bytes.length);write(comparisonFD,"\n",1);}
}
static id observePET(id self,SEL selector) {
    id token=originalPET(self,selector);
    if(token && access(RWComparisonPath.fileSystemRepresentation,F_OK)==0 &&
       [self respondsToSelector:@selector(appleID)] && [self respondsToSelector:@selector(dsid)] && [self respondsToSelector:@selector(altDSID)] && [token respondsToSelector:@selector(stringValue)]) {
        rw_comparison_capture(@{@"username":[self appleID]?:NSNull.null,@"pet":[token stringValue]?:NSNull.null,@"dsid":[self dsid]?:NSNull.null,@"alt_dsid":[self altDSID]?:NSNull.null});
        rw_comparison_after_failure(comparisonRecord);
    }
    return token;
}
__attribute__((visibility("default"))) int rw_daemon_pet_comparison_install(void) {
    static dispatch_once_t once;static BOOL installed;
    dispatch_once(&once,^{
        Class cls=objc_getClass("AKAuthenticationServerResponse");Method method=class_getInstanceMethod(cls,@selector(passwordEquivalentToken));
        if(!method || strcmp(method_getTypeEncoding(method),"@16@0:8"))return;
        comparisonFD=open("/var/mobile/Library/RedwoodAppleAccountLab/pet-comparison-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
        if(comparisonFD<0)return;
        originalPET=(void *)method_getImplementation(method);method_setImplementation(method,(IMP)observePET);installed=YES;
        comparisonRecord(@{@"event":@"daemon_pet_observer_installed"});
    });return installed;
}
__attribute__((constructor)) static void start(void) {dispatch_async(dispatch_get_main_queue(),^{rw_daemon_pet_comparison_install();});}

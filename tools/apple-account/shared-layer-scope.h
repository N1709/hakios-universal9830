/* Native account clients only; account verification remains native. */
#import <Foundation/Foundation.h>
static inline BOOL rw_shared_account_process(void){
 return [@[@"akd",@"accountsd",@"appleaccountd",@"amsaccountsd",@"appstored",@"itunesstored",@"storeaccountd",@"Preferences",@"AppStore"] containsObject:NSProcessInfo.processInfo.processName];
}
static inline BOOL rw_shared_media_process(void){
 return [@[@"accountsd",@"amsaccountsd",@"appstored",@"itunesstored",@"Preferences",@"AppStore"] containsObject:NSProcessInfo.processInfo.processName];
}

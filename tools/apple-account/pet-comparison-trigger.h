/* One-shot owner-authorized PET comparison, no raw-password/token logging.
 * Fixed local SSH tunnel endpoint; transient reference expires after120s. */
#import <Foundation/Foundation.h>
#include <sys/stat.h>
#include <unistd.h>
static NSObject *rwComparisonLock;
static NSDictionary *rwComparisonEnvelope,*rwComparisonConfig;
static NSDate *rwComparisonExpiry;
static NSString *const RWComparisonPath=@"/var/mobile/Library/RedwoodAppleAccountLab/native-comparison.json";
static NSObject *rw_comparison_lock(void) {
    static dispatch_once_t once;dispatch_once(&once,^{rwComparisonLock=[NSObject new];});return rwComparisonLock;
}
static BOOL rw_comparison_string(id value,NSUInteger limit) {
    return [value isKindOfClass:NSString.class] && [value length]>0 && [value length]<=limit &&
        [value rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location==NSNotFound;
}
static void rw_comparison_capture(NSDictionary *values) {
    @synchronized(rw_comparison_lock()){rwComparisonEnvelope=nil;rwComparisonConfig=nil;rwComparisonExpiry=nil;}
    struct stat st;if(lstat(RWComparisonPath.fileSystemRepresentation,&st) || !S_ISREG(st.st_mode) || st.st_uid!=501 || (st.st_mode&077) || st.st_size>1024)return;
    NSData *bytes=[NSData dataWithContentsOfFile:RWComparisonPath];
    NSDictionary *config=bytes?[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL]:nil;
    if(![config isKindOfClass:NSDictionary.class] || ![config[@"enabled"] isEqual:@YES] || ![config[@"port"] isEqual:@49752] || !rw_comparison_string(config[@"token"],128) || [config[@"token"] length]<32)return;
    id dsid=values[@"dsid"];if([dsid isKindOfClass:NSNumber.class])dsid=[dsid stringValue];
    if(!rw_comparison_string(values[@"username"],320) || !rw_comparison_string(values[@"pet"],16384) || !rw_comparison_string(dsid,20) || !rw_comparison_string(values[@"alt_dsid"],256))return;
    if([dsid rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location!=NSNotFound)return;
    NSString *nonce=[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""];
    NSDictionary *envelope=@{@"version":@1,@"nonce":nonce,@"issued_at":@(NSDate.date.timeIntervalSince1970),@"username":values[@"username"],@"pet":values[@"pet"],@"dsid":dsid,@"alt_dsid":values[@"alt_dsid"]};
    @synchronized(rw_comparison_lock()){rwComparisonEnvelope=envelope;rwComparisonConfig=config;rwComparisonExpiry=[NSDate dateWithTimeIntervalSinceNow:120];}
    unlink(RWComparisonPath.fileSystemRepresentation);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,120*NSEC_PER_SEC),dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        @synchronized(rw_comparison_lock()) {
            if([rwComparisonEnvelope[@"nonce"] isEqual:nonce]){rwComparisonEnvelope=nil;rwComparisonConfig=nil;rwComparisonExpiry=nil;}
        }
    });
}
static void rw_comparison_after_failure(void (*logger)(NSDictionary *)) {
    __block NSDictionary *envelope=nil,*config=nil;
    @synchronized(rw_comparison_lock()) {
        if(rwComparisonExpiry.timeIntervalSinceNow>0){envelope=rwComparisonEnvelope;config=rwComparisonConfig;}
        rwComparisonEnvelope=nil;rwComparisonConfig=nil;rwComparisonExpiry=nil;
    }
    if(!envelope || !config)return;
    logger(@{@"event":@"native_pet_comparison_started"});
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:@"http://127.0.0.1:49752/compare-once"]];
        request.HTTPMethod=@"POST";request.timeoutInterval=110;
        [request setValue:[@"Bearer " stringByAppendingString:config[@"token"]] forHTTPHeaderField:@"Authorization"];
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        request.HTTPBody=[NSJSONSerialization dataWithJSONObject:envelope options:0 error:NULL];
        NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
        [[session dataTaskWithRequest:request completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
            NSInteger transport=[response isKindOfClass:NSHTTPURLResponse.class]?[(NSHTTPURLResponse *)response statusCode]:0;
            id reply=[data isKindOfClass:NSData.class] && data.length>0 && data.length<=8192?[NSJSONSerialization JSONObjectWithData:data options:0 error:NULL]:nil;
            if(error || transport!=200 || ![reply isKindOfClass:NSDictionary.class] || ![reply[@"version"] isEqual:@1] || ![reply[@"nonce"] isEqual:envelope[@"nonce"]] || ![reply[@"account_saved"] isEqual:@NO]) {
                logger(@{@"event":@"native_pet_comparison_unavailable",@"transport_status":@(transport),@"transport_error_code":@(error.code)});
            } else {
                id status=reply[@"http_status"];NSMutableArray *errors=[NSMutableArray array];
                if([reply[@"errors"] isKindOfClass:NSArray.class])for(id item in reply[@"errors"]) {
                    if(errors.count==4)break;
                    if([item isKindOfClass:NSDictionary.class] && [@[@"com.apple.appleaccount",@"com.apple.AppleAccount.Error",@"NSURLErrorDomain",@"NSCocoaErrorDomain",@"other"] containsObject:item[@"domain"]] && [item[@"code"] isKindOfClass:NSNumber.class])
                        [errors addObject:@{@"domain":item[@"domain"],@"code":item[@"code"]}];
                }
                if([status isKindOfClass:NSNumber.class] && [status integerValue]>=0 && [status integerValue]<=599)
                    logger(@{@"event":@"native_pet_comparison_completed",@"mac_http_status":status,@"errors":errors,@"response_present":@([reply[@"response_present"] boolValue]),@"account_saved":@NO});
            }
            [session finishTasksAndInvalidate];
        }] resume];
    });
}

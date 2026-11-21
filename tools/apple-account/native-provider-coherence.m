/* Native machine metadata and empty-fixture signature availability only. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
@interface NSObject (ProviderCoherence)
+ (id)currentDevice;
+ (id)machineUDID;
+ (id)sharedSigner;
+ (id)udid;
- (id)localUserUUID;
- (id)uniqueDeviceIdentifier;
- (id)initWithDSID:(id)dsid;
- (void)addProvisioningInfoToURLRequest:(id)request sendEmptyValues:(BOOL)flag;
- (void)signatureForData:(NSData *)data completion:(void (^)(id,NSError *))completion;
@end
int main(void) { @autoreleasepool {
    dlopen("/System/Library/PrivateFrameworks/AOSKit.framework/AOSKit",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AuthKit.framework/AuthKit",RTLD_NOW);
    dlopen("/System/Library/PrivateFrameworks/AppleAccount.framework/AppleAccount",RTLD_NOW);
    Class cls=objc_getClass("AADeviceProvisioningSession");
    Method method=cls?class_getInstanceMethod(cls,@selector(addProvisioningInfoToURLRequest:sendEmptyValues:)):NULL;
    if(!method || strcmp(method_getTypeEncoding(method),"v28@0:8@16c24"))return 2;
    NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:@"https://gsa.apple.com/grandslam/GsService2"]];
    id session=[[cls alloc] initWithDSID:@"-2"];
    [session addProvisioningInfoToURLRequest:request sendEmptyValues:NO];
    NSMutableDictionary *fields=[NSMutableDictionary dictionary];
    for(NSString *name in @[@"X-Apple-I-MD",@"X-Apple-MD",@"X-Apple-I-MD-M",@"X-Apple-MD-M",
        @"X-Apple-I-MD-RINFO",@"X-Apple-I-MD-LU",@"X-Mme-Nas-Qualify"])
        fields[name]=@([request valueForHTTPHeaderField:name].length>0);
    id device=[objc_getClass("AKDevice") currentDevice];
    id deviceID=[device uniqueDeviceIdentifier];
    id user=[device localUserUUID];
    NSMutableDictionary *report=[@{@"native_provisioning_header_presence":fields,
        @"native_device_id_nonempty":@([deviceID isKindOfClass:NSString.class] && [deviceID length]>0),
        @"native_local_user_id_nonempty":@([user isKindOfClass:NSString.class] && [user length]>0),
        @"aos_and_authkit_device_id_agree":@([deviceID isEqual:[objc_getClass("AOSUtilities") machineUDID]]),
        @"appleaccount_and_authkit_device_id_agree":@([deviceID isEqual:[objc_getClass("AADeviceInfo") udid]]),
        @"request_sent":@NO,@"account_login_attempted":@NO} mutableCopy];
    dispatch_semaphore_t done=dispatch_semaphore_create(0);
    [[objc_getClass("AAAbsintheSigner") sharedSigner] signatureForData:NSData.data completion:^(id signature,NSError *error){
        report[@"empty_fixture_signature_type"]=[signature isKindOfClass:NSData.class]?@"data":
            [signature isKindOfClass:NSDictionary.class]?@"headers":@"other";
        report[@"empty_fixture_signature_nonempty"]=@([signature respondsToSelector:@selector(length)] && [signature length]>0);
        if([signature isKindOfClass:NSDictionary.class])
            report[@"validation_header_nonempty"]=@([signature[@"X-Mme-Nas-Qualify"] isKindOfClass:NSString.class] && [signature[@"X-Mme-Nas-Qualify"] length]>0);
        if(error)report[@"signing_error"]=@{@"domain":error.domain,@"code":@(error.code)};
        dispatch_semaphore_signal(done);
    }];
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:20];
    BOOL completed=NO;
    while(end.timeIntervalSinceNow>0) {
        if(!dispatch_semaphore_wait(done,DISPATCH_TIME_NOW)){completed=YES;break;}
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.02]];
    }
    if(!completed)report=[@{@"timed_out":@YES,@"account_login_attempted":@NO} mutableCopy];
    report[@"signing_completed"]=@(completed);
    NSData *output=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL];
    if(output)fwrite(output.bytes,1,output.length,stdout);
    return output?0:1;
}}

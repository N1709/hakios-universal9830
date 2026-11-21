/* Synthetic request only. No network or account operations. */
#import <Foundation/Foundation.h>
#include "icloud-request-identity.h"
int main(void) { @autoreleasepool {
    NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:@"https://setup.icloud.com/fixture"]];
    request.HTTPMethod=@"POST";request.HTTPBody=[@"opaque-fixture" dataUsingEncoding:NSUTF8StringEncoding];
    [request setValue:@"opaque-fixture" forHTTPHeaderField:@"Authorization"];
    [request setValue:@"native-agent" forHTTPHeaderField:@"User-Agent"];
    [request setValue:@"native-device" forHTTPHeaderField:@"Device-UDID"];
    [request setValue:@"stale-native-proof" forHTTPHeaderField:@"X-Apple-MD"];
    [request setValue:@"stale-native-machine" forHTTPHeaderField:@"X-Apple-MD-M"];
    NSMutableDictionary *identity=[NSMutableDictionary dictionary];
    for(NSString *field in @[@"X-Apple-I-MD",@"X-Apple-I-MD-M",@"X-Apple-I-MD-RINFO",@"X-Apple-I-MD-LU",@"X-Apple-I-Client-Time",@"X-Mme-Device-Id",@"X-MMe-Client-Info",@"X-Apple-I-SRL-NO"])identity[field]=@"synthetic";
    identity[@"X-Mme-Nas-Qualify"]=@"c3ludGhldGljLXByb29m";
    identity[@"User-Agent"]=@"akd/synthetic CFNetwork/synthetic Darwin/synthetic";
    NSURLRequest *output=rw_auth_lab_align_icloud_request(request,identity);
    NSCAssert(output && [output.URL isEqual:request.URL] && [output.HTTPMethod isEqual:@"POST"],@"preserve native route/method");
    NSCAssert([output.HTTPBody isEqual:request.HTTPBody] && [[output valueForHTTPHeaderField:@"Authorization"] isEqual:@"opaque-fixture"],@"preserve opaque native input");
    NSCAssert([[output valueForHTTPHeaderField:@"Device-UDID"] isEqual:@"synthetic"],@"align device alias");
    NSCAssert([[output valueForHTTPHeaderField:@"X-Mme-Nas-Qualify"] isEqual:identity[@"X-Mme-Nas-Qualify"]],@"preserve generated validation metadata");
    NSCAssert([[output valueForHTTPHeaderField:@"User-Agent"] isEqual:@"native-agent"],@"preserve native User-Agent selection");
    NSCAssert([[request valueForHTTPHeaderField:@"Device-UDID"] isEqual:@"native-device"],@"do not mutate input");
    NSCAssert([[output valueForHTTPHeaderField:@"X-Apple-MD"] isEqual:identity[@"X-Apple-I-MD"]],@"align native iCloud OTP header");
    NSCAssert([[output valueForHTTPHeaderField:@"X-Apple-MD-M"] isEqual:identity[@"X-Apple-I-MD-M"]],@"align native iCloud machine header");
    NSCAssert([[request valueForHTTPHeaderField:@"X-Apple-MD"] isEqual:@"stale-native-proof"],@"preserve original native request");
    [identity removeObjectForKey:@"X-Apple-I-MD-LU"];
    NSCAssert(!rw_auth_lab_align_icloud_request(request,identity),@"reject incomplete metadata");
    request.URL=[NSURL URLWithString:@"https://example.invalid/"];
    NSCAssert(rw_auth_lab_align_icloud_request(request,identity)==request,@"scope to iCloud host");
    puts("PASS: selected metadata alignment, native request preservation, scope and validation");
    return 0;
}}

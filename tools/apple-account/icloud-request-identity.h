/* Preserve the native request and credentials; replace only selected lab metadata. */
#import <Foundation/Foundation.h>
static NSURLRequest *rw_auth_lab_align_icloud_request(NSURLRequest *request,NSDictionary *identity) {
    if(![request isKindOfClass:NSURLRequest.class])return nil;
    if(![request.URL.scheme.lowercaseString isEqual:@"https"] ||
        ![request.URL.host.lowercaseString isEqual:@"setup.icloud.com"])return request;
    NSArray *fields=@[@"X-Apple-I-MD",@"X-Apple-I-MD-M",@"X-Apple-I-MD-RINFO",@"X-Apple-I-MD-LU",
        @"X-Apple-I-Client-Time",@"X-Mme-Device-Id",@"X-MMe-Client-Info",@"X-Apple-I-SRL-NO"];
    if(![identity isKindOfClass:NSDictionary.class])return nil;
    for(NSString *field in fields) {
        id value=identity[field];
        if(![value isKindOfClass:NSString.class] || ![value length] ||
            [value rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location!=NSNotFound)return nil;
    }
    NSMutableURLRequest *copy=request.mutableCopy;
    for(NSString *field in fields)[copy setValue:identity[field] forHTTPHeaderField:field];
    [copy setValue:identity[@"X-Mme-Device-Id"] forHTTPHeaderField:@"Device-UDID"];
    /* AppleAccount AADeviceProvisioningSession writes legacy MD names, not
     * AuthKit's I-MD names. Align both members to the same provider snapshot. */
    [copy setValue:identity[@"X-Apple-I-MD"] forHTTPHeaderField:@"X-Apple-MD"];
    [copy setValue:identity[@"X-Apple-I-MD-M"] forHTTPHeaderField:@"X-Apple-MD-M"];
    for(NSString *field in @[@"X-Mme-Nas-Qualify"]) {
        id value=identity[field];
        if(!value)continue;
        if(![value isKindOfClass:NSString.class] || ![value length] || [value length]>16384 ||
            [value rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location!=NSNotFound)return nil;
        [copy setValue:value forHTTPHeaderField:field];
    }
    /* Preserve native User-Agent selection; a nominal AADeviceInfo value is
     * not an explicit header on the native Mac/iOS prototype requests. */
    return copy.copy;
}

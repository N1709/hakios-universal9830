/* Pure dictionary adapter. Inputs are never printed, serialized or persisted. */
#import <Foundation/Foundation.h>

static NSDictionary *rw_auth_lab_align_parameters(NSDictionary *parameters, NSDictionary *identity,
    NSString *cpdKey, NSString *extraKey, NSString *infoKey) {
    if (![parameters isKindOfClass:NSDictionary.class] || ![identity isKindOfClass:NSDictionary.class] ||
        !cpdKey || !extraKey || !infoKey) return nil;
    NSDictionary *cpd=parameters[cpdKey], *extra=parameters[extraKey];
    if (![cpd isKindOfClass:NSDictionary.class] || ![extra isKindOfClass:NSDictionary.class] ||
        ![parameters[infoKey] isKindOfClass:NSString.class]) return nil;
    // Preserve the native proof; only align metadata for its provisioned identity.
    for (NSString *field in @[@"X-Apple-I-MD-M", @"X-Apple-I-MD-RINFO"]) {
        if (![identity[field] isKindOfClass:NSString.class] || ![identity[field] length] ||
            ![cpd[field] isEqual:identity[field]]) return nil;
    }
    NSArray *fields=@[@"X-Mme-Device-Id", @"X-Apple-I-MD-LU", @"X-Apple-I-SRL-NO"];
    for (NSString *field in [fields arrayByAddingObject:@"X-MMe-Client-Info"])
        if (![identity[field] isKindOfClass:NSString.class] || ![identity[field] length]) return nil;
    NSMutableDictionary *alignedCPD=cpd.mutableCopy, *alignedExtra=extra.mutableCopy;
    for (NSString *field in fields) {
        alignedCPD[field]=identity[field];
        alignedExtra[field]=identity[field];
    }
    // The virtual provider supplies neither a logic-board serial nor a ROM ID.
    for (NSString *field in @[@"X-Apple-I-MLB", @"X-Apple-I-ROM"]) {
        [alignedCPD removeObjectForKey:field]; [alignedExtra removeObjectForKey:field];
    }
    if (alignedCPD[@"X-MMe-Client-Info"]) alignedCPD[@"X-MMe-Client-Info"]=identity[@"X-MMe-Client-Info"];
    if (alignedExtra[@"X-MMe-Client-Info"]) alignedExtra[@"X-MMe-Client-Info"]=identity[@"X-MMe-Client-Info"];
    NSMutableDictionary *result=parameters.mutableCopy;
    result[cpdKey]=alignedCPD.copy; result[extraKey]=alignedExtra.copy;
    result[infoKey]=identity[@"X-MMe-Client-Info"];
    return result.copy;
}

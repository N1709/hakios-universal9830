/* Exact native failure labels only. Never return raw NSError text/userInfo. */
#import <Foundation/Foundation.h>
static inline NSString *rw_srp_error_category(NSError *error) {
    if(![error isKindOfClass:NSError.class] || ![error.domain isEqual:@"com.apple.AppleIDAuthSupport"])
        return @"not_auth_support";
    NSDictionary *labels=@{
        @"M2 missing (bad password)":@"server_proof_missing",
        @"M2 wrong size":@"server_proof_size_invalid",
        @"server passed bad M2":@"server_proof_verification_failed",
        @"selected protocol key missing":@"selected_protocol_missing",
        @"password missing in options":@"password_option_missing",
        @"salt missing from server":@"server_salt_missing",
        @"iteration missing from server":@"server_iteration_missing",
        @"B missing from server":@"server_public_value_missing",
        @"client neg2: input dict missing":@"negotiation_input_missing",
        @"client neg3: input dict missing":@"proof_input_missing"};
    for(NSString *key in @[@"NSDescription",NSLocalizedDescriptionKey,NSDebugDescriptionErrorKey]) {
        id text=error.userInfo[key];
        if([text isKindOfClass:NSString.class] && [text length]<=256 && labels[text])return labels[text];
    }
    return @"unclassified";
}

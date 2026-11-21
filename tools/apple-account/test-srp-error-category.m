#import <Foundation/Foundation.h>
#include "srp-error-category.h"
int main(void) { @autoreleasepool {
    NSDictionary *labels=@{@"M2 missing (bad password)":@"server_proof_missing",@"selected protocol key missing":@"selected_protocol_missing",@"server passed bad M2":@"server_proof_verification_failed"};
    for(NSString *text in labels) {
        NSError *error=[NSError errorWithDomain:@"com.apple.AppleIDAuthSupport" code:2 userInfo:@{@"NSDescription":text,@"token":@"synthetic-secret"}];
        if(![rw_srp_error_category(error) isEqual:labels[text]])return 1;
    }
    for(id text in @[@"synthetic-secret M2 missing (bad password)",@42,@"private account text"]) {
        NSError *error=[NSError errorWithDomain:@"com.apple.AppleIDAuthSupport" code:2 userInfo:@{@"NSDescription":text}];
        if(![rw_srp_error_category(error) isEqual:@"unclassified"])return 2;
    }
    NSError *other=[NSError errorWithDomain:@"other" code:2 userInfo:@{@"NSDescription":@"M2 missing (bad password)"}];
    if(![rw_srp_error_category(other) isEqual:@"not_auth_support"])return 3;
    puts("PASS: exact native SRP labels classified; arbitrary text, token fields and unrelated domains excluded");
    return 0;
}}

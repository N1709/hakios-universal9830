/* Synthetic fixtures only. No frameworks, accounts or network requests. */
#import <Foundation/Foundation.h>
#include "payload-identity.h"

int main(void) { @autoreleasepool {
    NSDictionary *identity=@{@"X-Apple-I-MD-M":@"virtual-mid", @"X-Apple-I-MD-RINFO":@"42",
        @"X-Mme-Device-Id":@"virtual-device", @"X-Apple-I-MD-LU":@"virtual-user",
        @"X-Apple-I-SRL-NO":@"virtual-serial", @"X-MMe-Client-Info":@"virtual-client"};
    NSDictionary *cpd=@{@"X-Apple-I-MD-M":@"virtual-mid", @"X-Apple-I-MD-RINFO":@"42",
        @"X-Mme-Device-Id":@"native-device", @"X-Apple-I-SRL-NO":@"native-serial",
        @"X-Apple-I-MD":@"unchanged-proof", @"X-Apple-I-MLB":@"native-board", @"capability":@YES};
    NSObject *opaque=[NSObject new];
    NSDictionary *input=@{@"cpd":cpd, @"extra":@{@"X-Apple-I-ROM":@"native-rom"},
        @"info":@"native-client", @"opaque":opaque};
    NSDictionary *output=rw_auth_lab_align_parameters(input,identity,@"cpd",@"extra",@"info");
    NSCAssert(output && output[@"opaque"]==opaque, @"preserve unrelated envelope entries");
    NSCAssert([output[@"cpd"][@"X-Apple-I-MD"] isEqual:@"unchanged-proof"], @"preserve native proof");
    NSCAssert([output[@"cpd"][@"capability"] isEqual:@YES], @"preserve capabilities");
    for (NSString *field in @[@"X-Mme-Device-Id",@"X-Apple-I-MD-LU",@"X-Apple-I-SRL-NO"]) {
        NSCAssert([output[@"cpd"][field] isEqual:identity[field]], @"align cpd");
        NSCAssert([output[@"extra"][field] isEqual:identity[field]], @"align additional headers");
    }
    NSCAssert([output[@"info"] isEqual:identity[@"X-MMe-Client-Info"]], @"align client info");
    NSCAssert(!output[@"cpd"][@"X-Apple-I-MLB"] && !output[@"extra"][@"X-Apple-I-ROM"], @"omit unsupported IDs");
    NSCAssert([input[@"cpd"][@"X-Mme-Device-Id"] isEqual:@"native-device"], @"immutable source");
    NSMutableDictionary *wrong=identity.mutableCopy; wrong[@"X-Apple-I-MD-M"]=@"wrong-mid";
    NSCAssert(!rw_auth_lab_align_parameters(input,wrong,@"cpd",@"extra",@"info"), @"refuse different machine");
    wrong=identity.mutableCopy; [wrong removeObjectForKey:@"X-Apple-I-MD-LU"];
    NSCAssert(!rw_auth_lab_align_parameters(input,wrong,@"cpd",@"extra",@"info"), @"refuse incomplete identity");
    NSCAssert(!rw_auth_lab_align_parameters(@{},identity,@"cpd",@"extra",@"info"), @"refuse unknown layout");
    puts("PASS: synthetic payload alignment, proof preservation, immutable inputs and fail-closed identity checks");
    return 0;
}}

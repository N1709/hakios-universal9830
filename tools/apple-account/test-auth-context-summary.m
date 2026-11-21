#import <Foundation/Foundation.h>
#include "auth-context-summary.h"
@interface SyntheticContext:NSObject
@end
@implementation SyntheticContext
- (NSUInteger)authenticationType {return 2;}
- (NSInteger)serviceType {return 1;}
- (BOOL)shouldRequestShortLivedToken {return NO;}
- (id)serviceIdentifier {return @"com.apple.mobileme";}
- (id)serviceIdentifiers {return @[@"com.apple.gs.idms.pet",@"synthetic-private-value"];}
- (id)desiredInternalTokens {return @[@"synthetic-secret-token"];}
- (id)appProvidedContext {return @"synthetic-secret-context";}
@end
int main(void) { @autoreleasepool {
    NSDictionary *summary=rw_context_summary([SyntheticContext new]);
    NSData *data=[NSJSONSerialization dataWithJSONObject:summary options:0 error:NULL];
    NSString *text=[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if([text containsString:@"synthetic-"])return 1;
    if(![summary[@"authenticationType"] isEqual:@2] || ![summary[@"serviceIdentifiers"][@"unclassified_count"] isEqual:@1])return 2;
    if(![summary[@"app_provided_context_present"] boolValue])return 3;
    puts("PASS: native context summary reports selected flags/labels, excludes unknown identifiers and context contents");return 0;
}}

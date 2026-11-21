#import <Foundation/Foundation.h>
#include "icloud-response-summary.h"
int main(void){@autoreleasepool{
 NSDictionary *fixture=@{@"status":@0,@"password":@"NEVER-EXPORT",@"tokens":@{@"secret":@"NEVER-EXPORT"},@"delegates":@{@"com.apple.mobileme":@{@"status":@"401",@"errorMessage":@"NEVER-EXPORT"}},@"account":@"NEVER-EXPORT"};
 NSData *bytes=[NSPropertyListSerialization dataWithPropertyList:fixture format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
 NSDictionary *summary=rw_icloud_response_summary(bytes);
 NSData *output=[NSJSONSerialization dataWithJSONObject:summary options:0 error:NULL];
 NSString *text=[[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding];
 if([text containsString:@"NEVER-EXPORT"] || ![summary[@"root"][@"tokens_field_present"] boolValue] || ![summary[@"delegates"][@"com.apple.mobileme"][@"status"] isEqual:@401])return 1;
 puts("PASS: fixed status codes and presence flags only; account/token/text values excluded.");return 0;
}}

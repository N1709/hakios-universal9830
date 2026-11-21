/* Known error fields/categories only. Never return server text or account data. */
#import <Foundation/Foundation.h>
static NSDictionary *rw_media_response_summary(id object){
 if(![object isKindOfClass:NSDictionary.class])return @{@"dictionary":@NO};
 NSMutableDictionary *codes=[NSMutableDictionary dictionary];NSString *category=@"unclassified";
 NSRegularExpression *numeric=[NSRegularExpression regularExpressionWithPattern:@"\\A-?[0-9]{1,12}\\z" options:0 error:NULL];
 for(NSString *key in @[@"status",@"statusCode",@"errorCode",@"error-code",@"failureType",@"errorNumber"]){
  id value=object[key];NSString *text=[value isKindOfClass:NSNumber.class]?[value stringValue]:([value isKindOfClass:NSString.class]?value:nil);
  if(text.length<=13 && [numeric numberOfMatchesInString:text?:@"" options:0 range:NSMakeRange(0,text.length)]==1)codes[key]=@(text.longLongValue);
 }
 for(NSString *key in @[@"customerMessage",@"failureType",@"errorMessage",@"message"]){
  id value=object[key];if(![value isKindOfClass:NSString.class] || [value length]>2048)continue;NSString *lower=[value lowercaseString];
  if([lower containsString:@"never been used"] || [lower containsString:@"not been used"] || [lower containsString:@"review your account"])category=@"store_account_review";
  else if([lower containsString:@"password"] && ([lower containsString:@"incorrect"] || [lower containsString:@"invalid"]))category=@"credentials_rejected";
  else if([lower containsString:@"verification code"])category=@"verification_code_related";
  else if([lower containsString:@"authentication failed"])category=@"authentication_failed";
 }
 return @{@"dictionary":@YES,@"numeric_codes":codes,@"category":category};
}

/* Fixed response status labels only. Never export account, token or raw text. */
#import <Foundation/Foundation.h>
static NSDictionary *rw_icloud_status_node(id node) {
    if(![node isKindOfClass:NSDictionary.class])return @{};
    NSMutableDictionary *out=[NSMutableDictionary dictionary];
    for(NSString *key in @[@"status",@"status-code",@"statusCode",@"errorCode",@"error-code"]){
        id value=node[key];
        if([value isKindOfClass:NSNumber.class])out[key]=value;
        else if([value isKindOfClass:NSString.class] && [value length]<=12 && [value length] &&
            [value rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"-0123456789"].invertedSet].location==NSNotFound)
            out[key]=@([value longLongValue]);
    }
    out[@"error_field_present"]=@(node[@"error"]!=nil || node[@"errorMessage"]!=nil);
    out[@"tokens_field_present"]=@(node[@"tokens"]!=nil);
    return out;
}
static NSDictionary *rw_icloud_response_summary(NSData *bytes) {
    if(![bytes isKindOfClass:NSData.class] || !bytes.length || bytes.length>2097152)return @{@"parsed_dictionary":@NO};
    id root=[NSPropertyListSerialization propertyListWithData:bytes options:0 format:NULL error:NULL];
    if(!root)root=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL];
    if(![root isKindOfClass:NSDictionary.class])return @{@"parsed_dictionary":@NO};
    NSMutableDictionary *out=[@{@"parsed_dictionary":@YES,@"root":rw_icloud_status_node(root),
        @"account_info_present":@(root[@"appleAccountInfo"]!=nil || root[@"dsInfo"]!=nil)} mutableCopy];
    id delegates=root[@"delegates"];
    NSMutableDictionary *services=[NSMutableDictionary dictionary];
    if([delegates isKindOfClass:NSDictionary.class])for(NSString *key in @[@"com.apple.mobileme",@"com.apple.gamecenter",@"com.apple.private.ids"])
        if([delegates[key] isKindOfClass:NSDictionary.class])services[key]=rw_icloud_status_node(delegates[key]);
    out[@"delegates"]=services;
    return out;
}

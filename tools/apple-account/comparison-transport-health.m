/* Loopback-tunnel reachability only; deliberately no bearer/account/token. */
#import <Foundation/Foundation.h>
int main(void) { @autoreleasepool {
    NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:@"http://127.0.0.1:49752/compare-once"]];
    request.HTTPMethod=@"POST";request.HTTPBody=[@"{}" dataUsingEncoding:NSUTF8StringEncoding];request.timeoutInterval=3;
    __block NSInteger status=0;__block BOOL completed=NO;
    NSURLSession *session=[NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    [[session dataTaskWithRequest:request completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){(void)data;(void)error;status=[response isKindOfClass:NSHTTPURLResponse.class]?[(NSHTTPURLResponse *)response statusCode]:0;completed=YES;}] resume];
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:5];while(!completed && end.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
    [session invalidateAndCancel];
    if(completed && status==401){puts("PASS: comparison tunnel reaches private endpoint; unauthorized health request refused, no Apple request");return 0;}
    puts("FAIL: private comparison endpoint not reachable");return 1;
}}

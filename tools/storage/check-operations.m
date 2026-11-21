/* Exercise real FileProvider copy/move operations on a newly created test folder.
 * Usage: check-operations PATH_TO_DOWNLOADS
 * Sign with entitlements from the user's own fileproviderctl executable.
 * On success only this test's UUID folder is removed; failures leave it for diagnosis.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
@interface FPItemManager : NSObject
+ (id)defaultManager;
- (id)itemForURL:(NSURL *)url error:(NSError **)error;
- (void)addOperation:(NSOperation *)operation;
@end
@interface FPTransferOperation : NSOperation
- (id)initWithItems:(NSArray *)items destinationFolder:(id)folder;
- (NSDictionary *)errorsByItem;
@end
static NSString *testRoot;
static FPItemManager *manager;
static NSData *contents;
static void transfer(BOOL copy) {
 NSString *source=[testRoot stringByAppendingPathComponent:copy?@"Source/probe.txt":@"Copied/probe.txt"];
 NSString *target=[testRoot stringByAppendingPathComponent:copy?@"Copied":@"Moved"];
 NSError *error=nil;id item=[manager itemForURL:[NSURL fileURLWithPath:source] error:&error];
 if(!item){NSLog(@"item lookup failed %@",error);exit(1);}
 id folder=[manager itemForURL:[NSURL fileURLWithPath:target isDirectory:YES] error:&error];
 if(!folder){NSLog(@"folder lookup failed %@",error);exit(1);}
 FPTransferOperation *op=[[NSClassFromString(copy?@"FPCopyOperation":@"FPMoveOperation") alloc] initWithItems:@[item] destinationFolder:folder];
 __weak FPTransferOperation *weakOp = op;
 [op setCompletionBlock:^{ dispatch_async(dispatch_get_main_queue(), ^{
  NSString *result=[target stringByAppendingPathComponent:@"probe.txt"];
  NSData *got=[NSData dataWithContentsOfFile:result];
  BOOL retained=[[NSFileManager defaultManager] fileExistsAtPath:source];
  if(![got isEqualToData:contents] || retained!=copy){NSLog(@"%@ FAILED: result=%@ retained=%d errors=%@",copy?@"copy":@"move",result,retained,[weakOp errorsByItem]);exit(1);}
  NSLog(@"%@ PASS through FileProvider: %@",copy?@"copy":@"move",result);
  if(copy){transfer(NO);}else{NSError *cleanupError=nil; if(![[NSFileManager defaultManager] removeItemAtPath:testRoot error:&cleanupError]){NSLog(@"cleanup failed %@",cleanupError);exit(1);} NSLog(@"COPY+MOVE PASS; disposable directory removed");exit(0);}
 });}];
 [manager addOperation:op];
}
int main(int argc, char **argv){@autoreleasepool {
 if(argc!=2){fprintf(stderr,"usage: check-operations PATH_TO_DOWNLOADS\n");return 2;}
 dlopen("/System/Library/Frameworks/FileProvider.framework/FileProvider",RTLD_NOW);
 NSString *base=[NSString stringWithUTF8String:argv[1]]; BOOL directory=NO;
 if(![[NSFileManager defaultManager] fileExistsAtPath:base isDirectory:&directory]||!directory){fprintf(stderr,"Downloads directory must exist\n");return 2;}
 testRoot=[base stringByAppendingPathComponent:[@"Files-check-" stringByAppendingString:[NSUUID UUID].UUIDString]];
 contents=[@"HakiOS FileProvider copy/move check\n" dataUsingEncoding:NSUTF8StringEncoding];
 NSFileManager *fm=[NSFileManager defaultManager];NSError *e=nil;
 for(NSString *name in @[@"Source",@"Copied",@"Moved"]){if(![fm createDirectoryAtPath:[testRoot stringByAppendingPathComponent:name] withIntermediateDirectories:YES attributes:nil error:&e]){NSLog(@"mkdir %@",e);return 1;}}
 if(![contents writeToFile:[testRoot stringByAppendingPathComponent:@"Source/probe.txt"] options:NSDataWritingAtomic error:&e]){NSLog(@"write %@",e);return 1;}
 manager=[NSClassFromString(@"FPItemManager") defaultManager];
 dispatch_async(dispatch_get_main_queue(), ^{transfer(YES);});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,45*NSEC_PER_SEC),dispatch_get_main_queue(),^{NSLog(@"transfer timeout; retained %@",testRoot);exit(1);});
 dispatch_main();
}}

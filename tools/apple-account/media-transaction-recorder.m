/* One coordinated, opt-in plaintext RESPONSE recorder. Request bodies/credentials
 * are never recorded. Response tokens may exist ONLY in protected capture files.
 * Native data/completion/error objects and authentication decisions are unchanged. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "shared-layer-scope.h"
static char stateKey;
static unsigned long long serial;
static NSUInteger processBytes;
static NSString *budgetRun;
static void(*nativeResume)(id,SEL);
static void(*nativeData)(id,SEL,id,id,id);
static void(*nativeFinish)(id,SEL,id);
@interface RWTransaction:NSObject
@property NSString *directory,*identifier;
@property NSTimeInterval began;
@property NSUInteger received,written;
@property BOOL truncated;
@property int fd;
@end
@implementation RWTransaction
- (instancetype)init{if((self=[super init]))_fd=-1;return self;}
- (void)dealloc{if(_fd>=0)close(_fd);}
@end
static BOOL writeAll(int fd,const void *bytes,size_t length){
 const char *p=bytes;while(length){ssize_t n=write(fd,p,length);if(n<0 && errno==EINTR)continue;if(n<=0)return NO;p+=n;length-=(size_t)n;}return YES;
}
static BOOL inScope(NSURLRequest *request){
 if(getenv("RW_TRANSACTION_SELFTEST"))return [request.URL.host isEqual:@"127.0.0.1"];
 NSString *host=request.URL.host.lowercaseString;
 return [@[@"gsa.apple.com",@"gsas.apple.com",@"auth.itunes.apple.com"] containsObject:host] || [host hasSuffix:@"-buy.itunes.apple.com"] || [host isEqual:@"buy.itunes.apple.com"];
}
static NSString *safePath(NSString *path){
 if(![path isKindOfClass:NSString.class] || path.length>256)return @"redacted";
 NSMutableArray *parts=[NSMutableArray array];
 NSCharacterSet *allowed=[NSCharacterSet characterSetWithCharactersInString:@"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-"];
 for(NSString *part in [path componentsSeparatedByString:@"/"]){
  BOOL secret=part.length>48 || [part containsString:@"@"] || [part rangeOfCharacterFromSet:allowed.invertedSet].location!=NSNotFound;
  if(part.length>=6 && [part rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location==NSNotFound)secret=YES;
  [parts addObject:secret?@"redacted":part];
 }return [parts componentsJoinedByString:@"/"];
}
static void event(RWTransaction *state,NSString *kind,NSDictionary *fields){
 NSMutableDictionary *entry=[fields mutableCopy];entry[@"event"]=kind;entry[@"transaction"]=state.identifier;entry[@"pid"]=@(getpid());entry[@"process"]=NSProcessInfo.processInfo.processName;entry[@"time"]=@(NSDate.date.timeIntervalSince1970);
 NSData *bytes=[NSJSONSerialization dataWithJSONObject:entry options:0 error:NULL];
 int fd=open([[state.directory stringByAppendingPathComponent:[NSString stringWithFormat:@"%d.events.jsonl",getpid()]] fileSystemRepresentation],O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
 if(fd>=0){if(bytes){writeAll(fd,bytes.bytes,bytes.length);writeAll(fd,"\n",1);}close(fd);}
}
static RWTransaction *transaction(NSURLSessionTask *task){
 RWTransaction *state=objc_getAssociatedObject(task,&stateKey);if(state)return state;
 NSURLRequest *request=task.currentRequest?:task.originalRequest;if(!inScope(request))return nil;
 NSData *bytes=[NSData dataWithContentsOfFile:@"/var/mobile/Library/RedwoodAppleAccountLab/capture.json"];
 if(!bytes.length || bytes.length>4096)return nil;
 id config=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL];
 if(![config isKindOfClass:NSDictionary.class] || ![config[@"enabled"] isEqual:@YES] || ![config[@"expires_at"] isKindOfClass:NSNumber.class])return nil;
 NSTimeInterval remaining=[config[@"expires_at"] doubleValue]-NSDate.date.timeIntervalSince1970;
 NSString *run=config[@"run"];
 if(remaining<=0 || remaining>900 || ![run isKindOfClass:NSString.class] || run.length!=32 || [run rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"].invertedSet].location!=NSNotFound)return nil;
 @synchronized(RWTransaction.class){
  state=objc_getAssociatedObject(task,&stateKey);if(state)return state;
  if(![budgetRun isEqual:run]){budgetRun=run;processBytes=0;}
  if(processBytes>=8*1024*1024)return nil;
  state=[RWTransaction new];state.directory=[@"/var/mobile/Library/RedwoodAppleAccountLab/private-capture" stringByAppendingPathComponent:run];
  state.identifier=[NSString stringWithFormat:@"%d-%llu",getpid(),__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED)];state.began=NSDate.date.timeIntervalSince1970;
  state.fd=open([[state.directory stringByAppendingPathComponent:[state.identifier stringByAppendingString:@".response"]] fileSystemRepresentation],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
  if(state.fd<0)return nil;
  objc_setAssociatedObject(task,&stateKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
 }
 NSArray *headerNames=[[request.allHTTPHeaderFields allKeys] sortedArrayUsingSelector:@selector(compare:)];
 event(state,@"request_started",@{@"host":request.URL.host?:@"",@"path":safePath(request.URL.path),@"method":request.HTTPMethod?:@"",@"request_header_names":headerNames?:@[],@"request_body_bytes":@(request.HTTPBody.length),@"task_identifier":@(task.taskIdentifier)});
 return state;
}
static void resume(NSURLSessionTask *self,SEL cmd){(void)transaction(self);nativeResume(self,cmd);}
static void data(NSURLSessionTask *self,SEL cmd,id connection,id bytes,id completion){
 RWTransaction *state=transaction(self);
 if(state && [bytes isKindOfClass:NSData.class])@synchronized(state){
  NSUInteger length=[bytes length];state.received+=length;
  @synchronized(RWTransaction.class){
   NSUInteger room=MIN((NSUInteger)(2*1024*1024)-MIN(state.written,(NSUInteger)(2*1024*1024)),(NSUInteger)(8*1024*1024)-MIN(processBytes,(NSUInteger)(8*1024*1024)));
   NSUInteger count=MIN(length,room);
   if(state.fd>=0 && count && writeAll(state.fd,[bytes bytes],count)){state.written+=count;processBytes+=count;}else if(count)state.truncated=YES;
   if(count!=length)state.truncated=YES;
  }
 }
 nativeData(self,cmd,connection,bytes,completion);
}
static void finish(NSURLSessionTask *self,SEL cmd,NSError *error){
 RWTransaction *state=transaction(self);
 if(state)@synchronized(state){
  if(state.fd>=0){close(state.fd);state.fd=-1;}
  NSHTTPURLResponse *response=[self.response isKindOfClass:NSHTTPURLResponse.class]?(id)self.response:nil;
  NSString *contentType=[response valueForHTTPHeaderField:@"Content-Type"];
  if(![contentType isKindOfClass:NSString.class] || contentType.length>128)contentType=@"other";
  event(state,@"request_finished",@{@"http_status":@(response.statusCode),@"response_header_names":[[response.allHeaderFields allKeys] sortedArrayUsingSelector:@selector(compare:)]?:@[],@"content_type":contentType?:@"",@"received_bytes":@(state.received),@"captured_bytes":@(state.written),@"truncated":@(state.truncated),@"elapsed_seconds":@(NSDate.date.timeIntervalSince1970-state.began),@"error_domain":error.domain?:@"",@"error_code":@(error.code)});
 }
 nativeFinish(self,cmd,error);
}
__attribute__((visibility("default"))) BOOL rw_transaction_recorder_install(void){
 static BOOL installed=NO;if(installed)return YES;
 Class cls=NSClassFromString(@"__NSCFLocalSessionTask");
 Method r=class_getInstanceMethod(cls,sel_registerName("_onqueue_resume"));
 Method d=class_getInstanceMethod(cls,sel_registerName("connection:didReceiveData:completion:"));
 Method f=class_getInstanceMethod(cls,sel_registerName("_onqueue_didFinishWithError:"));
 if(!r || strcmp(method_getTypeEncoding(r),"v16@0:8") || !d || strcmp(method_getTypeEncoding(d),"v40@0:8@16@24@?32") || !f || strcmp(method_getTypeEncoding(f),"v24@0:8@16"))return NO;
 nativeResume=(void *)method_getImplementation(r);nativeData=(void *)method_getImplementation(d);nativeFinish=(void *)method_getImplementation(f);
 method_setImplementation(r,(IMP)resume);method_setImplementation(d,(IMP)data);method_setImplementation(f,(IMP)finish);installed=YES;return YES;
}
__attribute__((constructor)) static void start(void){
 if(!rw_shared_account_process())return;
 dispatch_async(dispatch_get_main_queue(),^{
  BOOL installed=rw_transaction_recorder_install();
  int fd=open("/var/mobile/Library/RedwoodAppleAccountLab/request-events.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  if(fd>=0){dprintf(fd,"{\"event\":\"transaction_recorder_installed\",\"installed\":%s,\"pid\":%d}\n",installed?"true":"false",getpid());close(fd);}
 });
}

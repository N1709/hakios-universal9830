/* Root sensor coordinator for the rejection-only preview. No auth provider,
 * keybag writes, passwords, tokens or successful-match IPC exist here. */
#import <Foundation/Foundation.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/un.h>
#define PROC_PIDPATHINFO_MAXSIZE 4096
extern int proc_pidpath(int, void *, uint32_t);
#include <dlfcn.h>
#include <fcntl.h>
#include <notify.h>
#include <unistd.h>
#include "model.h"
#define SOCKET_PATH "/var/run/redwood-fingerprint-ui/socket"
static struct rw_fp_ui state;
static NSArray<NSString *> *queue;
static NSString *running;
static pid_t owner;
static double lease, commandDeadline, sessionDeadline;
static uint64_t logTotal;
static BOOL baseline, draining, enabled;
static NSMutableData *partial;
static NSString *failure=@"Open Settings to begin";
static int (*bagState)(CFDictionaryRef);
static double now(void){return NSProcessInfo.processInfo.systemUptime;}
static NSString *tee(void){char b[2048]={0};size_t n=sizeof(b)-1;
    if(sysctlbyname("kern.redwood.tee",b,&n,NULL,0))return nil;return @(b);}
static BOOL idle(NSString *s){return [s containsString:@"fingerprint ui: reject-v1 (no authentication)"] &&
    [s containsString:@"exchange-v2 (research only), active 0"] && [s containsString:@"TEEGRIS: up"];}
static BOOL sendCommand(NSString *s){const char *b=s.UTF8String;return sysctlbyname("kern.redwood.tee",NULL,NULL,(void*)b,strlen(b))==0;}
static void stop(BOOL error){
    /* Only stop work owned by this daemon; never cancel another research agent. */
    if(running)sendCommand(@"fp stop");
    draining=running!=nil; queue=nil; state.phase=error?RW_ERROR:RW_IDLE;
    state.event++; owner=0;
}
static void fail(NSString *message){failure=message;stop(YES);}
static NSData *snapshot(void){size_t n=0;if(sysctlbyname("kern.redwood_dmesg",NULL,&n,NULL,0)||n<8||n>1024*1024)return nil;
    NSMutableData *d=[NSMutableData dataWithLength:n];if(sysctlbyname("kern.redwood_dmesg",d.mutableBytes,&n,NULL,0))return nil;
    d.length=n;return d;}
static void readEvents(void){
    NSData *d=snapshot();if(d.length<8){fail(@"Sensor events unavailable");return;}
    uint64_t total;memcpy(&total,d.bytes,8);size_t bytes=d.length-8;
    if(!baseline){baseline=YES;logTotal=total;partial=[NSMutableData data];return;}
    if(total<logTotal || total-logTotal>bytes){logTotal=total;[partial setLength:0];fail(@"Sensor events interrupted; try again");return;}
    size_t delta=(size_t)(total-logTotal);logTotal=total;
    if(!delta)return;
    [partial appendBytes:(const char*)d.bytes+d.length-delta length:delta];
    if(partial.length>256*1024){fail(@"Sensor event overflow");[partial setLength:0];return;}
    const char *b=partial.bytes;size_t begin=0;
    for(size_t i=0;i<partial.length;i++)if(b[i]=='\n'){
        NSString *line=[[NSString alloc]initWithBytes:b+begin length:i-begin encoding:NSISOLatin1StringEncoding];
        rw_fp_line(&state,line.UTF8String);begin=i+1;
    }
    if(begin)[partial replaceBytesInRange:NSMakeRange(0,begin) withBytes:NULL length:0];
}
static BOOL privateTemplate(void){struct stat st;
    return !lstat("/var/root/fp-template.bin",&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&
        (st.st_mode&0777)==0600&&st.st_nlink==1&&st.st_size>0&&st.st_size<=0x64000;}
static BOOL backup(void){
    int fd=open("/var/root/fp-template.bin",O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return errno==ENOENT;
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid||st.st_nlink!=1||(st.st_mode&0777)!=0600||st.st_size<=0||st.st_size>0x64000){close(fd);return NO;}
    char path[160];snprintf(path,sizeof(path),"/var/root/fp-template.before-ui.%llu.bin",(unsigned long long)(now()*1000000));
    int out=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(out<0){close(fd);return NO;}
    char b[4096];ssize_t n;BOOL ok=YES;
    while((n=read(fd,b,sizeof(b)))>0){ssize_t at=0,k;while(at<n){k=write(out,b+at,(size_t)(n-at));if(k<=0){ok=NO;break;}at+=k;}if(!ok)break;}
    if(n<0||fsync(out))ok=NO;memset(b,0,sizeof(b));close(fd);close(out);if(!ok)unlink(path);return ok;
}
static BOOL completedOK(NSString *cmd,NSString *s){
    NSString *last=[[s componentsSeparatedByString:@"\nlast: "] lastObject];
    if([cmd isEqual:@"fp power"])return [last isEqual:@"fp power: configured, error 0"] || [last isEqual:@"fp power: configured, error 0\n"];
    if([cmd isEqual:@"fp cbgeefs"])return [last containsString:@"2 factory slots read, load error 0"];
    if([cmd isEqual:@"fp idinit"])return [last hasPrefix:@"fp idinit: error 0 op "];
    if([cmd isEqual:@"fp einit 1"])return [last hasPrefix:@"fp einit 1: result 0 error 0 op "];
    if([cmd hasPrefix:@"fp run "])return [last hasPrefix:@"fp run "];
    return [last containsString:@": result 0 origin "]&&[last containsString:@" error 0 timeout "];
}
static void tick(void){
    if(!running && !queue.count)return;
    NSString *s=tee();if(!s){fail(@"Sensor service unavailable");return;}
    if(!draining && (now()>lease || now()>sessionDeadline || (state.phase==RW_REGISTER && (!bagState||bagState(NULL)!=0))))stop(NO);
    if(running) {
        if(!draining)readEvents();
        if(state.phase==RW_ERROR && !draining){stop(YES);}
        if(!idle(s)){if(now()>commandDeadline)fail(@"Sensor timed out; wait for it to stop");return;}
        if(!draining && !completedOK(running,s)){running=nil;fail(@"Sensor setup failed");return;}
        BOOL wasRun=[running hasPrefix:@"fp run "];running=nil;
        if(draining){draining=NO;return;}
        if(state.phase==RW_ERROR){queue=nil;return;}
        if(wasRun){
            if(state.phase==RW_REGISTER){queue=@[@"fp einit 1",@"fp run 3 1 15 4"];}
            else {queue=nil;owner=0; if(state.phase==RW_COMPARE){state.phase=RW_IDLE;state.event++;}}
        }
        if(state.phase==RW_SAVED){queue=nil;owner=0;}
    }
    if(queue.count && !running){
        if(!idle(s)){fail(@"Sensor is busy in another session");return;}
        NSString *next=queue[0];queue=[queue subarrayWithRange:NSMakeRange(1,queue.count-1)];
        if(!sendCommand(next)){fail(@"Sensor command refused");return;}
        running=next;commandDeadline=now()+([next hasPrefix:@"fp run "]?25:20);
    }
}
static NSDictionary *handle(const char *command,pid_t pid,BOOL preferences){
    BOOL ours=owner==pid;
    if(!strcmp(command,"lease")&&ours)lease=now()+3;
    if(!strcmp(command,"cancel")&&ours)stop(NO);
    if((!strcmp(command,"enable")||!strcmp(command,"disable")) && preferences && bagState && bagState(NULL)==0) {
        enabled=!strcmp(command,"enable");if(!enabled)stop(NO);
    }
    BOOL enrolling=!strcmp(command,"enroll"),matching=!strcmp(command,"compare");
    if(enrolling||matching){
        if(running||queue.count){if(!ours)failure=@"Sensor busy";}
        else if(!idle(tee())){failure=@"Sensor backend is not prepared";state.phase=RW_ERROR;}
        else if(enrolling&&(!preferences||!bagState||bagState(NULL)!=0)){failure=@"Unlock with your passcode before setup";state.phase=RW_ERROR;}
        else if(!enrolling&&!privateTemplate()){failure=@"Set up a fingerprint first";state.phase=RW_ERROR;}
        else if(enrolling&&!backup()){failure=@"Existing fingerprint could not be backed up";state.phase=RW_ERROR;}
        else {
            state=(struct rw_fp_ui){.phase=enrolling?RW_REGISTER:RW_COMPARE,.saved=privateTemplate(),.event=state.event+1};
            owner=pid;lease=now()+3;sessionDeadline=now()+(enrolling?300:45);baseline=NO;readEvents();failure=@"";
            if(state.phase==RW_ERROR)return @{@"version":@1,@"phase":@"unavailable",@"authentication":@"unavailable"};
            queue=@[@"fp power",@"fp 1:2",@"fp cbgeefs",@"fp 10:1",@"fp 1:2",enrolling?@"fp einit 1":@"fp idinit",
                enrolling?@"fp run 3 1 15 4":@"fp run 6 1 15 0"];
            tick();
        }
    }
    return @{@"version":@1,@"phase":@(rw_fp_phase_name(state.phase)),@"progress":@(state.progress),
        @"saved":@(state.saved||privateTemplate()),@"enabled":@(enabled),@"event":@(state.event),@"busy":@(running!=nil||queue.count>0),
        @"owns":@(owner==pid),@"message":failure,@"authentication":@"unavailable"};
}
static void serve(int server){while(1){@autoreleasepool{
    int fd=accept(server,NULL,NULL);if(fd<0){if(errno==EINTR)continue;break;}
    struct timeval timeout={.tv_sec=1};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));int one=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
    uid_t uid;gid_t gid;pid_t pid=0;socklen_t size=sizeof(pid);char path[PROC_PIDPATHINFO_MAXSIZE]={0};
    BOOL valid=!getpeereid(fd,&uid,&gid)&&uid==501&&!getsockopt(fd,SOL_LOCAL,LOCAL_PEERPID,&pid,&size)&&
        proc_pidpath(pid,path,sizeof(path))>0;
    BOOL prefs=!strcmp(path,"/Applications/Preferences.app/Preferences");
    valid=valid&&(prefs||!strcmp(path,"/System/Library/CoreServices/SpringBoard.app/SpringBoard"));
    char command[16]={0};ssize_t count=valid?recv(fd,command,sizeof(command)-1,0):-1;
    if(count>0 && !memchr(command,0,(size_t)count) &&
        (!strcmp(command,"status")||!strcmp(command,"lease")||!strcmp(command,"enroll")||!strcmp(command,"compare")||!strcmp(command,"cancel")||!strcmp(command,"enable")||!strcmp(command,"disable"))){
        NSString *action=@(command);
        __block NSData *reply;
        dispatch_sync(dispatch_get_main_queue(),^{reply=[NSJSONSerialization dataWithJSONObject:handle(action.UTF8String,pid,prefs) options:0 error:NULL];});
        if(reply.length)send(fd,reply.bytes,reply.length,0);
    }
    close(fd);
}}}
int main(void){@autoreleasepool{
    if(getuid()!=0)return 1;umask(0077);
    /* Root-owned parent prevents mobile clients from substituting the endpoint. */
    if(mkdir("/var/run/redwood-fingerprint-ui",0755)&&errno!=EEXIST)return 1;
    struct stat st;if(lstat("/var/run/redwood-fingerprint-ui",&st)||!S_ISDIR(st.st_mode)||st.st_uid||(st.st_mode&0022))return 1;
    if(chmod("/var/run/redwood-fingerprint-ui",0755))return 1;
    int lock=open("/var/run/redwood-fingerprint-ui/lock",O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 1;
    int server=socket(AF_UNIX,SOCK_STREAM,0);if(server<0)return 1;
    unlink(SOCKET_PATH);struct sockaddr_un addr={.sun_family=AF_UNIX};strlcpy(addr.sun_path,SOCKET_PATH,sizeof(addr.sun_path));
    if(bind(server,(struct sockaddr*)&addr,sizeof(addr))||chmod(SOCKET_PATH,0666)||listen(server,4))return 1;
    void *mkb=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_NOW);
    bagState=mkb?dlsym(mkb,"MKBGetDeviceLockState"):NULL;
    static int displayToken;
    notify_register_dispatch("com.apple.iokit.hid.displayStatus",&displayToken,dispatch_get_main_queue(),^(int t){
        uint64_t on=0;if(notify_get_state(t,&on)!=NOTIFY_STATUS_OK||!on)stop(NO);
    });
    [NSTimer scheduledTimerWithTimeInterval:.25 repeats:YES block:^(NSTimer *timer){(void)timer;tick();}];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{serve(server);});
    [[NSRunLoop mainRunLoop]run];
}return 0;}

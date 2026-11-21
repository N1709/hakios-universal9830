/* 20F66-only bring-up bridge, loaded for one locationd launch by the operator.
 * Root-only local socket; live checksum-valid RMC+GGA epochs only. No simulated
 * CLLocation, authorization override, saved-position replay or authentication hook.
 * Uses Apple's accessory NMEA parser on its own silo. Not a shipped provider.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <mach-o/dyld.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/select.h>

static void (*original_begin)(id,SEL);
static void (*native_nmea)(void *,CFStringRef);
static id (*original_universe)(id,SEL);
static Method universe_method;
static const char *socket_path="/var/tmp/redwood-gnss-nmea.sock";
static unsigned epochs,rejected;
static void status(const char *phase)
{
    int fd=open("/var/tmp/gnss-native-nmea-status.txt",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd>=0){dprintf(fd,"%s epochs=%u rejected=%u\n",phase,epochs,rejected);close(fd);}
}
#include "nmea_lab_gate.h"
static id universe(id self,SEL cmd)
{
    id result=original_universe(self,cmd);
    static bool requested;
    if(result && __sync_bool_compare_and_swap(&requested,false,true)){
        method_setImplementation(universe_method,(IMP)original_universe);
        /* Ask the real service vendor to instantiate the accessory provider.
         * This queries its real connection state; it never reports a connection
         * or match that does not exist. The normal provider begin hook follows. */
        dispatch_async(dispatch_get_main_queue(),^{
            @try {
                id vendor=((id(*)(id,SEL))objc_msgSend)(result,@selector(vendor));
                if(!vendor){status("vendor-unavailable");return;}
                int fd=open("/var/tmp/gnss-vendor-abi.txt",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
                if(fd>=0){
                    Class cls=object_getClass(vendor);unsigned count=0;
                    dprintf(fd,"class %s\n",class_getName(cls));
                    Method *methods=class_copyMethodList(cls,&count);
                    for(unsigned i=0;i<count;i++)dprintf(fd,"%s %s\n",sel_getName(method_getName(methods[i])),method_getTypeEncoding(methods[i]));
                    free(methods);close(fd);
                }
                status("vendor-inspected");
            } @catch(NSException *exception){(void)exception;status("provider-request-failed");}
        });
    }
    return result;
}
static void serve(id adapter,id silo)
{
    @autoreleasepool {
        int server=socket(AF_UNIX,SOCK_STREAM,0);if(server<0)return;
        struct sockaddr_un address={0};address.sun_family=AF_UNIX;
        strlcpy(address.sun_path,socket_path,sizeof(address.sun_path));
        mode_t old=umask(077);
        int result=bind(server,(struct sockaddr *)&address,sizeof(address));umask(old);
        if(result || listen(server,1)){close(server);status("socket-unavailable");return;}
        struct timeval timeout={1,0};setsockopt(server,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        status("listening");time_t end=time(NULL)+600;
        while(time(NULL)<end){
            fd_set readset;FD_ZERO(&readset);FD_SET(server,&readset);
            struct timeval wait={1,0};if(select(server+1,&readset,NULL,NULL,&wait)<=0)continue;
            int client=accept(server,NULL,NULL);if(client<0)continue;
            uid_t uid;gid_t gid;
            if(getpeereid(client,&uid,&gid) || uid!=0){close(client);rejected++;continue;}
            setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
            char line[257],gga[257]={0};size_t used=0;bool overflow=false;
            while(time(NULL)<end){
                char c;ssize_t n=read(client,&c,1);if(n<=0)break;
                if(c=='\r')continue;
                if(c!='\n'){if(used<256)line[used++]=c;else overflow=true;continue;}
                line[used]=0;used=0;
                if(overflow || !gnss_nmea_checksum(line)){overflow=false;gga[0]=0;rejected++;continue;}
                if(!strncmp(line,"$GPGGA,",7) || !strncmp(line,"$GNGGA,",7))strlcpy(gga,line,sizeof(gga));
                if(strncmp(line,"$GPRMC,",7) && strncmp(line,"$GNRMC,",7))continue;
                if(!gga[0] || !gnss_nmea_live_epoch(line,gga,time(NULL))){rejected++;gga[0]=0;continue;}
                NSString *first=[[NSString alloc] initWithUTF8String:gga];
                NSString *second=[[NSString alloc] initWithUTF8String:line];
                /* Retrieving adaptee inside the silo avoids retaining a C++
                 * provider pointer past endService. Normal client permissions
                 * and the native parser's validity/accuracy policy still apply. */
                ((void(*)(id,SEL,id))objc_msgSend)(silo,@selector(async:),^{
                    void *provider=((void *(*)(id,SEL))objc_msgSend)(adapter,@selector(adaptee));
                    if(provider){native_nmea(provider,(__bridge CFStringRef)first);native_nmea(provider,(__bridge CFStringRef)second);}
                });
                [first release];[second release];gga[0]=0;epochs++;status("forwarding");
            }
            close(client);
        }
        close(server);unlink(socket_path);status("stopped");
    }
}
static void begin(id self,SEL cmd)
{
    original_begin(self,cmd);static bool started;if(started)return;started=true;
    status("begin");
    void *provider=((void *(*)(id,SEL))objc_msgSend)(self,@selector(adaptee));
    if(!provider)return;
    id context=*(id *)((char *)provider+0x20);
    id silo=((id(*)(id,SEL))objc_msgSend)(context,@selector(silo));
    if(![silo respondsToSelector:@selector(async:)])return;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT,0),^{serve(self,silo);});
}
__attribute__((constructor)) static void install(void)
{
    Class cls=objc_getClass("CLAccessoryLocationProviderAdapter");
    Method m=cls?class_getInstanceMethod(cls,@selector(beginService)):NULL;
    const unsigned char start[]={0xf4,0x4f,0xbe,0xa9,0xfd,0x7b,0x01,0xa9,0xfd,0x43,0x00,0x91,0xf3,0x03,0x00,0xaa};
    const unsigned char callback[]={0xff,0x43,0x02,0xd1,0xf8,0x5f,0x05,0xa9,0xf6,0x57,0x06,0xa9,0xf4,0x4f,0x07,0xa9};
    /* Anchor to the checked method in locationd, rather than assuming dyld
     * image index zero identifies the executable on this port. */
    if(!m)return;
    native_nmea=(void(*)(void *,CFStringRef))((uintptr_t)method_getImplementation(m)+(0x10037a428-0x10037873c));
    bool method_ok=m && !strcmp(method_getTypeEncoding(m),"v16@0:8") && !memcmp((void *)method_getImplementation(m),start,sizeof(start));
    bool callback_ok=!memcmp((void *)native_nmea,callback,sizeof(callback));
    if(!method_ok || !callback_ok){
        status(!method_ok?"incompatible-method":"incompatible-callback");
        return;
    }
    original_begin=(void(*)(id,SEL))method_getImplementation(m);
    method_setImplementation(m,(IMP)begin);
    status("armed");
    Class service=objc_getClass("CLIntersiloService");
    universe_method=class_getInstanceMethod(service,@selector(universe));
    if(universe_method && !strcmp(method_getTypeEncoding(universe_method),"@16@0:8")){
        original_universe=(id(*)(id,SEL))method_getImplementation(universe_method);
        method_setImplementation(universe_method,(IMP)universe);
    }
}

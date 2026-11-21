/* Small bounded UI-only IPC client. Call off the main queue. */
#import <Foundation/Foundation.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#define RW_FP_SOCKET "/var/run/redwood-fingerprint-ui/socket"
static NSDictionary *rw_fp_request(const char *command) {
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return nil;
    struct timeval timeout={.tv_sec=1};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    int one=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    strlcpy(addr.sun_path,RW_FP_SOCKET,sizeof(addr.sun_path));
    NSDictionary *result=nil;
    if(!connect(fd,(struct sockaddr*)&addr,sizeof(addr))) {
        size_t n=strlen(command);
        if(send(fd,command,n,0)==(ssize_t)n) {
            shutdown(fd,SHUT_WR);
            char bytes[1024];size_t used=0;ssize_t count;
            while(used<sizeof(bytes) && (count=recv(fd,bytes+used,sizeof(bytes)-used,0))>0)used+=(size_t)count;
            if(used && used<sizeof(bytes)) {
                id object=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:bytes length:used] options:0 error:NULL];
                if([object isKindOfClass:NSDictionary.class])result=object;
            }
        }
    }
    close(fd);return result;
}

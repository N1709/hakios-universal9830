/* Root-side stdin connector for the temporary locationd NMEA lab socket. */
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
int main(void)
{
    if(geteuid()!=0)return 1;
    signal(SIGPIPE,SIG_IGN);
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return 1;
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;
    strlcpy(address.sun_path,"/var/tmp/redwood-gnss-nmea.sock",sizeof(address.sun_path));
    if(connect(fd,(struct sockaddr *)&address,sizeof(address))){perror("NMEA lab socket");close(fd);return 1;}
    char bytes[4096];ssize_t n;
    while((n=read(STDIN_FILENO,bytes,sizeof(bytes)))>0){
        for(ssize_t at=0;at<n;){ssize_t k=write(fd,bytes+at,(size_t)(n-at));
            if(k<0 && errno==EINTR)continue;
            if(k<=0){close(fd);return 1;}at+=k;}
    }
    close(fd);return n<0?1:0;
}

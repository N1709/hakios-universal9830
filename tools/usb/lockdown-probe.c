/* Bounded QueryType probe for the user's stock lockdownd; no pairing/state change. */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
static int receive(int fd,void *buffer,size_t size){char *p=buffer;while(size){ssize_t n=read(fd,p,size);if(n==0){errno=ECONNRESET;return -1;}if(n<0){if(errno==EINTR)continue;return -1;}p+=n;size-=(size_t)n;}return 0;}
int main(int argc,char **argv){
 if(argc!=2 || (strcmp(argv[1],"unix") && strcmp(argv[1],"loopback") && strcmp(argv[1],"usb"))){fprintf(stderr,"usage: lockdown-probe unix|loopback|usb\n");return 2;}
 signal(SIGPIPE,SIG_IGN);
 int local=!strcmp(argv[1],"unix"),fd=socket(local?AF_UNIX:AF_INET,SOCK_STREAM,0);if(fd<0){perror("socket");return 1;}
 struct timeval timeout={5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
 int result;
 if(local){struct sockaddr_un a={.sun_len=sizeof(a),.sun_family=AF_UNIX};snprintf(a.sun_path,sizeof(a.sun_path),"/var/run/lockdown.sock");result=connect(fd,(struct sockaddr*)&a,sizeof(a));}
 else{struct sockaddr_in a={.sin_len=sizeof(a),.sin_family=AF_INET,.sin_port=htons(62078)};inet_pton(AF_INET,!strcmp(argv[1],"loopback")?"127.0.0.1":"10.77.0.2",&a.sin_addr);result=connect(fd,(struct sockaddr*)&a,sizeof(a));}
 if(result){perror("connect");close(fd);return 1;}
 const char request[]="<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict><key>Label</key><string>Jesvi transport probe</string><key>Request</key><string>QueryType</string></dict></plist>";
 uint32_t length=htonl(sizeof(request)-1);if(write(fd,&length,sizeof(length))!=sizeof(length)||write(fd,request,sizeof(request)-1)!=sizeof(request)-1){perror("write");close(fd);return 1;}
 if(receive(fd,&length,sizeof(length))){perror("reply header");close(fd);return 1;}
 length=ntohl(length);char response[16385];if(length>sizeof(response)-1){fprintf(stderr,"Reply exceeds probe limit\n");close(fd);return 1;}
 if(receive(fd,response,length)){perror("reply body");close(fd);return 1;}response[length]=0;printf("%s: %s\n",argv[1],response);close(fd);return 0;
}

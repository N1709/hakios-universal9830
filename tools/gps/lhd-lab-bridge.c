/* Linux-lab-only FIFO peer for stock Samsung gpsd. No GPS computation or fixes.
 * Typed FIFO framing recovered from a stock daemon capture. USB relay protocol
 * is private to tools/gps/gnss-relay.c. No independent SPI/BBD owner is created.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stop;
static void signal_stop(int n){(void)n;stop=1;}
static uint32_t u32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint16_t u16(const uint8_t *p){return p[0]|(uint16_t)p[1]<<8;}
static void put32(uint8_t *p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static int write_all(int fd,const uint8_t *p,size_t n){while(n){ssize_t k=write(fd,p,n);if(k<0 && errno==EINTR)continue;if(k<=0)return -1;p+=k;n-=(size_t)k;}return 0;}
int main(int argc,char **argv)
{
    unsigned duration=115;
    if(argc>2){fprintf(stderr,"usage: %s [seconds:10..600]\n",argv[0]);return 2;}
    if(argc==2){char *tail=NULL;errno=0;long v=strtol(argv[1],&tail,10);
        if(errno || tail==argv[1] || *tail || v<10 || v>600){fprintf(stderr,"invalid test duration\n");return 2;}duration=(unsigned)v;}

    signal(SIGINT,signal_stop);signal(SIGTERM,signal_stop);signal(SIGPIPE,SIG_IGN);
    const char *server="/share/android/data/vendor/gps/.pipe.gpsd_to_lhd.to_server";
    const char *client="/share/android/data/vendor/gps/.pipe.gpsd_to_lhd.to_client";
    int in=open(server,O_RDONLY|O_NONBLOCK),out=-1;
    if(in<0){perror("FIFO server");return 1;}
    fprintf(stderr,"FIFO server ready\n");fflush(stderr);
    double end=now()+10;
    while(now()<end){out=open(client,O_WRONLY|O_NONBLOCK);if(out>=0)break;usleep(10000);}
    if(out<0){perror("FIFO client");close(in);return 1;}
    int flags=fcntl(out,F_GETFL);fcntl(out,F_SETFL,flags&~O_NONBLOCK);
    int sock=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons(31338);inet_pton(AF_INET,"192.168.64.1",&a.sin_addr);
    if(connect(sock,(void*)&a,sizeof(a))){perror("USB relay connect");return 1;}
    int yes=1;setsockopt(sock,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
    uint8_t fifo[16384],net[8192];size_t f_used=0,n_used=0;unsigned commands=0,replies=0;end=now()+duration;
    while(!stop && now()<end){
        struct pollfd fds[]={{in,POLLIN,0},{sock,POLLIN,0}};
        int got=poll(fds,2,100);if(got<0 && errno==EINTR)continue;if(got<0)break;
        if(fds[0].revents&POLLIN){ssize_t n=read(in,fifo+f_used,sizeof(fifo)-f_used);if(n<=0)break;f_used+=(size_t)n;}
        while(f_used>=4){
            uint32_t size=u32(fifo);if(size<40 || size>8192){fprintf(stderr,"bad FIFO size%u\n",size);goto done;}if(f_used<size)break;
            uint32_t cmd=u32(fifo+16),n=u32(fifo+28);
            if(u32(fifo+4)!=0x5245ff02 || u32(fifo+8)!=0 || u32(fifo+12)!=0x5245ff02 || u32(fifo+20)!=4 || u32(fifo+24)!=0 || u32(fifo+32)!=n || u32(fifo+36)!=0 || n>8192 || 40+n>size || cmd>255){fprintf(stderr,"bad FIFO format\n");goto done;}
            if(cmd==6 && n==0x1288){
                printf("GLL fix status valid=%u\n",fifo[40+0x7b0]);fflush(stdout);
                int cap=open("/share/gll-fix-status.bin",O_WRONLY|O_CREAT|O_APPEND,0600);
                if(cap>=0){(void)write_all(cap,fifo+40,n);close(cap);}
                memmove(fifo,fifo+size,f_used-size);f_used-=size;continue;
            }
            if(cmd==7){printf("GNSS host-control acknowledgement bytes%u\n",n);fflush(stdout);memmove(fifo,fifo+size,f_used-size);f_used-=size;continue;}
            uint8_t message[8195]={(uint8_t)cmd,(uint8_t)n,(uint8_t)(n>>8)};memcpy(message+3,fifo+40,n);
            if(write_all(sock,message,n+3))goto done;
            printf("host command%u bytes%u\n",cmd,n);fflush(stdout);commands++;
            memmove(fifo,fifo+size,f_used-size);f_used-=size;
        }
        if(fds[1].revents&POLLIN){ssize_t n=read(sock,net+n_used,sizeof(net)-n_used);if(n<=0)break;n_used+=(size_t)n;}
        while(n_used>=3 && n_used>=3u+u16(net+1)){
            uint16_t n=u16(net+1);uint8_t cmd=net[0];if(n>4096 || (cmd!=8 && cmd!=9)){fprintf(stderr,"bad relay reply\n");goto done;}
            uint8_t packet[4140]={0};size_t size=(40+n+3u)&~3u;
            put32(packet,(uint32_t)size);put32(packet+4,0x5245ff02);put32(packet+12,0x5245ff02);put32(packet+16,cmd);put32(packet+20,4);put32(packet+28,n);put32(packet+32,n);memcpy(packet+40,net+3,n);
            if(write_all(out,packet,size))goto done;replies++;
            memmove(net,net+3+n,n_used-3-n);n_used-=3+n;
        }
        if(f_used==sizeof(fifo) || n_used==sizeof(net))break;
        if((fds[1].revents&(POLLHUP|POLLERR)) || (fds[0].revents&POLLHUP))break;
    }
done:
    fprintf(stderr,"bridge ended commands=%u replies=%u\n",commands,replies);close(sock);close(in);close(out);return 0;
}

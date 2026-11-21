/* Temporary USB-only bridge for the owner's stock GLL in an isolated Linux lab.
 * One client, bounded deadline (default two minutes), existing kernel SPI owner, paired nav cleanup.
 * Never resets/powers down the shared hub or accepts memory-write RPCs.
 */
#include "gnss_protocol.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static int dev=-1,peer=-1;
static uint8_t records[70000];static size_t records_used;
static bool forwarding=true,s20_lna;
static volatile sig_atomic_t stopped;static bool fatal,opened;static unsigned sent,received;
static struct gnss_meif_decoder meif;
static unsigned meif_packets;
static void meif_packet(void *ctx,const uint8_t *p,size_t n)
{
    (void)ctx;(void)p;(void)n;meif_packets++;
}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void on_signal(int sig){(void)sig;stopped=1;}
static bool output(uint8_t op,const uint8_t *data,size_t n)
{
    if(!forwarding)return true;
    if(n>4096)return false;uint8_t bytes[4099]={op,0,0};gnss_put16(bytes+1,(uint16_t)n);memcpy(bytes+3,data,n);
    for(size_t at=0;at<n+3;){ssize_t k=send(peer,bytes+at,n+3-at,0);if(k<0 && errno==EINTR)continue;if(k<=0)return false;at+=(size_t)k;}return true;
}
/* Returns1 for requested successful completion, -1 on fault,0 if none. */
static int drain(uint16_t expected)
{
    ssize_t k=read(dev,records+records_used,sizeof(records)-records_used);
    if(k>0)records_used+=(size_t)k;
    else if(k<0 && errno!=EAGAIN && errno!=EINTR)return -1;
    int ack=0;
    while(records_used>=6 && records_used>=6u+gnss_u16(records+4)) {
        uint8_t kind=records[0],flags=records[1];uint16_t id=gnss_u16(records+2),n=gnss_u16(records+4);uint8_t *d=records+6;
        if(kind=='R' && (id==0x17 || id==0x18)){fprintf(stderr,"hub exception RPC%#x\n",id);fatal=true;return -1;}
        if(kind=='A' && id==expected){if(!flags)return -1;ack=1;}
        if(kind=='R' && id==0x1f){
            if(n<2 || gnss_u16(d)!=n-2)return -1;
            gnss_meif_feed(&meif,d+2,n-2,meif_packet,NULL);
            if(meif.malformed){fprintf(stderr,"malformed MEIF stream; closing navigation\n");return -1;}
            if(!output(8,d+2,n-2))return -1;received++;}
        if(kind=='R' && id==0x38){if(n<3 || gnss_u16(d)>n-2 || !output(9,d+2,gnss_u16(d)))return -1;}
        memmove(records,records+6+n,records_used-6-n);records_used-=6+n;
    }
    if(records_used==sizeof(records))return -1;return ack;
}
static bool rpc(uint16_t id,const uint8_t *data,size_t n)
{
    if(n>1000 || fatal)return false;uint8_t q[1003]={'R',0,0};gnss_put16(q+1,id);memcpy(q+3,data,n);
    double end=now()+4;
    while(write(dev,q,n+3)!=(ssize_t)n+3){if(errno!=EBUSY || now()>end)return false;usleep(2000);}
    while(now()<end){int a=drain(id);if(a)return a==1;usleep(2000);}return false;
}
static bool command(uint8_t op,const uint8_t *p,size_t n)
{
    printf("LHD command=%u bytes=%zu\n",op,n);fflush(stdout);
    if(op==4)return n==0; /* physical hub already runs; no shared-MCU power change */
    if(op==1){uint8_t on=1;if(n || opened)return false;
        /* Stock x1slte lhd.conf: LhePinMux=P17=M1 (external GNSS LNA).
         * RpcGlobalRequestEncoder::SetPinMuxing encodes two u8s in RPC34.
         * This is explicit lab configuration, not a shared-hub reset. */
        if(s20_lna){const uint8_t mux[]={17,1};
            if(!rpc(0x34,mux,sizeof(mux)))return false;
            fprintf(stderr,"S20 external-LNA pin mux acknowledged\n");}
        opened=true;return rpc(0x1c,&on,1);}
    if(op==2 || op==5){if(n)return false;if(opened){uint8_t off=0;if(!rpc(0x1c,&off,1))return false;opened=false;}return true;}
    if(op==3 && opened && n && n<=4096){
        while(n){size_t k=n>998?998:n;uint8_t b[1000];gnss_put16(b,(uint16_t)k);memcpy(b+2,p,k);if(!rpc(0x1e,b,k+2))return false;p+=k;n-=k;sent++;}return true;
    }
    /* Unknown requests fail visibly; never fabricate a GNSS or authentication result. */
    fprintf(stderr,"unimplemented LHD command%u\n",op);return false;
}
int main(int argc,char **argv)
{
    unsigned duration=120;
    if(argc==3 && !strcmp(argv[2],"--s20-lna"))s20_lna=true;
    if(argc>3 || (argc==3 && !s20_lna)){fprintf(stderr,"usage: %s [seconds:10..600 [--s20-lna]]\n",argv[0]);return 2;}
    if(argc>=2){char *tail=NULL;errno=0;long v=strtol(argv[1],&tail,10);
        if(errno || tail==argv[1] || *tail || v<10 || v>600){fprintf(stderr,"invalid test duration\n");return 2;}duration=(unsigned)v;}

    signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGPIPE,SIG_IGN);
    int server=socket(AF_INET,SOCK_STREAM,0),yes=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    struct sockaddr_in addr={0};addr.sin_len=sizeof(addr);addr.sin_family=AF_INET;addr.sin_port=htons(31337);inet_pton(AF_INET,"10.77.0.2",&addr.sin_addr);
    if(bind(server,(void*)&addr,sizeof(addr)) || listen(server,1)){perror("USB listener");return 1;}
    printf("GNSS relay USB10.77.0.2:31337; single client,%us max\n",duration);fflush(stdout);
    fd_set set;FD_ZERO(&set);FD_SET(server,&set);struct timeval wait={60,0};
    if(select(server+1,&set,NULL,NULL,&wait)<=0){close(server);return 1;}
    peer=accept(server,NULL,NULL);close(server);if(peer<0){perror("accept");return 1;}
    setsockopt(peer,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
    struct timeval send_timeout={3,0};setsockopt(peer,SOL_SOCKET,SO_SNDTIMEO,&send_timeout,sizeof(send_timeout));
    dev=open("/dev/redwood-gnss",O_RDWR|O_NONBLOCK);if(dev<0){perror("GNSS");close(peer);return 1;}
    uint8_t input[8192];size_t used=0;double end=now()+duration;bool ok=true;
    while(!stopped && !fatal && now()<end){
        if(drain(0xffff)<0){ok=false;break;}
        FD_ZERO(&set);FD_SET(peer,&set);wait=(struct timeval){0,10000};int ready=select(peer+1,&set,NULL,NULL,&wait);
        if(ready<0 && errno==EINTR)continue;if(ready<0){ok=false;break;}
        if(ready>0){ssize_t n=recv(peer,input+used,sizeof(input)-used,0);if(n<=0)break;used+=(size_t)n;}
        while(used>=3 && used>=3u+gnss_u16(input+1)){
            uint16_t n=gnss_u16(input+1);if(n>4096 || !command(input[0],input+3,n)){ok=false;goto done;}
            memmove(input,input+3+n,used-3-n);used-=3+n;
        }
        if(used==sizeof(input)){ok=false;break;}
    }
done:
    forwarding=false;
    if(opened && !fatal){uint8_t off=0;bool closed=rpc(0x1c,&off,1);printf("cleanup nav_stop=%d\n",closed);ok=ok&&closed;}
    printf("MEIF packets=%u malformed=%u\n",meif_packets,meif.malformed);
    printf("relay ended sent=%u received=%u fault=%d ok=%d\n",sent,received,fatal,ok);close(dev);close(peer);return ok?0:1;
}

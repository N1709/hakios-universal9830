/* Bounded S20 navigation/pass-through handshake, not a positioning service.
 * Recovered stock messages only; root diagnostic through existing SPI owner.
 * Metadata on stdout, optional private raw capture. Always attempts channel stop.
 */
#include "gnss_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int fd=-1,capture=-1;
static uint8_t rx[70000];
static size_t used;
static volatile sig_atomic_t interrupted;
static bool fault,state10;
static unsigned nav_packets;
static struct gnss_meif_decoder decoder;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void stop_signal(int sig){(void)sig;interrupted=1;}
static void packet(void *ctx,const uint8_t *p,size_t n)
{
    (void)ctx;uint16_t id=gnss_u16(p);
    printf("MEIF id=%#x bytes=%zu checksum=valid\n",id,n);
    if(id==4 && n>=12 && gnss_u16(p+8)==10)state10=true;
    if(id==1 || id==2)fault=true;
}
static int record(double end,uint8_t wanted,uint16_t wanted_id)
{
    while(now()<end && !interrupted) {
        while(used>=6 && used>=6u+gnss_u16(rx+4)) {
            uint8_t kind=rx[0],flags=rx[1];uint16_t id=gnss_u16(rx+2),n=gnss_u16(rx+4);
            bool match=kind==wanted && id==wanted_id;
            if(capture>=0){size_t at=0;while(at<6u+n){ssize_t k=write(capture,rx+at,6u+n-at);if(k<0 && errno==EINTR)continue;if(k<=0){fault=true;break;}at+=(size_t)k;}}
            if(kind=='R') {
                if(id==0x17 || id==0x18){fprintf(stderr,"firmware exception RPC%#x; stop\n",id);fault=true;}
                else if(id==0x1f) {
                    nav_packets++;
                    if(n<2 || gnss_u16(rx+6)!=n-2){fprintf(stderr,"invalid navigation length\n");fault=true;}
                    else {
                        gnss_meif_feed(&decoder,rx+8,n-2,packet,NULL);
                        if(decoder.malformed)fault=true;
                    }
                } else printf("RPC id=%#x bytes=%u\n",id,n);
            }
            memmove(rx,rx+6+n,used-6-n);used-=6+n;
            if(fault)return -1;
            if(match)return flags?1:0;
        }
        if(used==sizeof(rx)){errno=EOVERFLOW;return -1;}
        ssize_t k=read(fd,rx+used,sizeof(rx)-used);
        if(k>0)used+=(size_t)k;
        else if(k<0 && errno!=EAGAIN && errno!=EINTR)return -1;
        else usleep(2000);
    }
    errno=ETIMEDOUT;return -1;
}
static bool send_rpc(uint16_t id,const uint8_t *body,size_t n)
{
    if(n>64 || fd<0 || fault)return false;
    uint8_t q[67]={'R',0,0};gnss_put16(q+1,id);memcpy(q+3,body,n);
    double end=now()+3;
    while(write(fd,q,n+3)!=(ssize_t)n+3){if(errno!=EBUSY || now()>=end)return false;usleep(2000);}
    int result=record(end,'A',id);
    printf("send RPC%#x bytes=%zu acknowledged=%d\n",id,n,result==1);
    fflush(stdout);return result==1;
}
/* Paired Samsung SSP sensor29 operation; period1000ms, no batching. */
static bool sensor29(bool enable)
{
    uint8_t q[20]={ 'S',0,1,0,0,0,0,0,0,0,29,0xe8,3,0,0,0,0,0,0,0 };
    q[1]=enable?0xa1:0xa2;gnss_put16(q+8,enable?10:5);
    size_t n=enable?20:15;double end=now()+3;
    while(write(fd,q,n)!=(ssize_t)n){if(errno!=EBUSY || now()>=end)return false;usleep(2000);}
    int result=record(end,'S',q[1]);
    printf("sensor29 %s transport_complete=%d\n",enable?"start":"stop",result==1);fflush(stdout);
    return result==1;
}
int main(int argc,char **argv)
{
    if(argc<2 || argc>3 || (strcmp(argv[1],"passthrough") && strcmp(argv[1],"sensor29"))){
        fprintf(stderr,"usage: gnss-session passthrough|sensor29 [PRIVATE_RECORD_CAPTURE]\n");return 2;}
    signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);
    if(argc==3){capture=open(argv[2],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(capture<0){perror("private capture");return 1;}}
    fd=open("/dev/redwood-gnss",O_RDWR|O_NONBLOCK);if(fd<0){perror("GNSS");if(capture>=0)close(capture);return 1;}
    const uint8_t on=1,off=0;
    /* Exact stock gpsd SendPassthroughCtrlMsg literal. */
    const uint8_t control[]={0xa5,0x93,3,0,8,0,3,0,10,0,24,0,0xa5,0x68};
    uint8_t body[16];gnss_navigation_body(body,sizeof(body),control,sizeof(control));
    bool opened=send_rpc(0x1c,&on,1),sent=false;
    bool sensor_mode=!strcmp(argv[1],"sensor29");
    if(opened)sent=sensor_mode?sensor29(true):send_rpc(0x1e,body,sizeof(body));
    if(sent && !fault){double end=now()+5;while(now()<end && !fault && !interrupted){(void)record(end,0,0);}}
    /* Do not transmit into a firmware exception. A reboot is then the recovery. */
    interrupted=0;
    bool sensor_closed=!sensor_mode || (!fault && sensor29(false));
    bool closed=!fault && send_rpc(0x1c,&off,1);
    close(fd);if(capture>=0){fsync(capture);close(capture);}
    printf("summary open=%d sent=%d navigation_packets=%u state10=%d closed=%d fault=%d malformed=%u\n",opened,sent,nav_packets,state10,closed,fault,decoder.malformed);
    return opened && sent && (sensor_mode || state10) && sensor_closed && closed && !fault?0:1;
}

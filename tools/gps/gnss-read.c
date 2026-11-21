/* Read-only Broadcom GNSS firmware capture through the existing SPI owner.
 * RPC7/8 field layout recovered from the owner's stock S20 lhd. No resets,
 * memory writes or SSP changes. Output files are private and exclusive.
 * Local read-record framing follows tools/sensors/gnsstool.c (Scion Project).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int dev;
static uint8_t buffer[70000];
static size_t used;
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p+2)<<16; }
static void put16(uint8_t *p,uint16_t v) { p[0]=v;p[1]=v>>8; }
static void put32(uint8_t *p,uint32_t v) { put16(p,v);put16(p+2,v>>16); }
static double seconds(void) { struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+ts.tv_nsec/1e9; }

/* Completes BOTH the transport outcome and the independently delivered reply.
 * The hub can send RPC8 before the kernel reports completion with 'A'. */
static int read_memory(uint32_t addr,uint16_t size,uint8_t *out)
{
    /* Only the 16-byte read has been validated on this firmware. Larger
     * transfers caused host_com_rx.cpp assertions: never infer a limit. */
    if(size==0 || size>16){errno=EINVAL;return -1;}
    uint8_t q[10]={'R',7,0,0};
    put32(q+4,addr);put16(q+8,size);
    double end=seconds()+5;
    while(write(dev,q,sizeof(q))!=(ssize_t)sizeof(q)) {
        if(errno!=EBUSY || seconds()>end)return -1;
        usleep(2000);
    }
    int ack=0,reply=0;
    while(seconds()<end) {
        while(used>=6 && used>=6u+le16(buffer+4)) {
            uint16_t n=le16(buffer+4),id=le16(buffer+2);
            if(buffer[0]=='R' && (id==0x17 || id==0x18)) {
                fprintf(stderr,"hub exception RPC %#x; stop immediately\n",id);
                errno=ECANCELED;return -1;
            }
            if(buffer[0]=='A' && id==7) {
                if(!buffer[1]) {errno=EIO;return -1;} ack=1;
            }
            if(buffer[0]=='R' && id==8 && n>=6 && le32(buffer+6)==addr) {
                if(le16(buffer+10)!=size || n!=size+6u){errno=EPROTO;return -1;}
                memcpy(out,buffer+12,size);reply=1;
            }
            memmove(buffer,buffer+6+n,used-6-n);used-=6+n;
        }
        if(ack && reply)return 0;
        if(used==sizeof(buffer)){errno=EOVERFLOW;return -1;}
        ssize_t got=read(dev,buffer+used,sizeof(buffer)-used);
        if(got>0)used+=(size_t)got;
        else if(got<0 && errno!=EAGAIN && errno!=EINTR)return -1;
        else usleep(2000);
    }
    fprintf(stderr,"RPC7 timeout ack=%d reply=%d requested=%u\n",ack,reply,size);errno=ETIMEDOUT;return -1;
}

int main(int argc,char **argv)
{
    if(argc!=3 || (strcmp(argv[1],"rom") && strcmp(argv[1],"patch") && strcmp(argv[1],"codec"))) {
        fprintf(stderr,"usage: gnss-read rom|patch|codec PRIVATE_OUTPUT_FILE\n");return 2;
    }
    uint32_t base=!strcmp(argv[1],"rom")?0:(!strcmp(argv[1],"codec")?0x20000:0x800000);
    uint32_t length=base==0x800000?0x56394:(base==0x20000?0x8000:0x100000);
    int output=open(argv[2],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(output<0){perror("private output");return 1;}
    dev=open("/dev/redwood-gnss",O_RDWR|O_NONBLOCK);
    if(dev<0){perror("GNSS device");close(output);return 1;}
    uint8_t data[16];double start=seconds();
    for(uint32_t at=0;at<length;) {
        uint16_t n=length-at>sizeof(data)?sizeof(data):(uint16_t)(length-at);
        if(read_memory(base+at,n,data)) {perror("firmware read");fprintf(stderr,"stopped at %#x; partial output retained\n",base+at);close(output);close(dev);return 1;}
        size_t done=0;
        while(done<n){ssize_t k=write(output,data+done,n-done);if(k<0 && errno==EINTR)continue;if(k<=0){perror("output write");close(output);close(dev);return 1;}done+=(size_t)k;}
        at+=n;
        if(at%(64*16)==0 || at==length)fprintf(stderr,"firmware: %u/%u bytes %.1fs\n",at,length,seconds()-start);
    }
    int result=fsync(output);close(output);close(dev);
    if(result){perror("output sync");return 1;}return 0;
}

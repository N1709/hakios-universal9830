/* Bounded S20 driver check. Explicit binned modes; optional private RAW dump.
 * usage: probe SENSOR [RAW_PATH [EXPOSURE_LINES GAIN_REGISTER [REAR_MODE]]]
 * REAR_MODE: 2 boot default, 3 native4032x3024, 4 explicit2016x1512 (new kernel).
 * Run only with Camera closed and no other reader. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "redwood_camera.h"
int main(int argc, char **argv) {
    unsigned sensor = argc >= 2 ? (unsigned)atoi(argv[1]) : 1;
    if (sensor != 1 && sensor != 2 && sensor != 3 && sensor != 4) return 2;
    if (argc != 2 && argc != 3 && argc != 5 && argc != 6) return 2;
    unsigned lines=argc>=5?(unsigned)strtoul(argv[3],NULL,10):1001;
    unsigned gain=argc>=5?(unsigned)strtoul(argv[4],NULL,10):32;
    if (lines<16 || lines>22000 || gain<32 || gain>512) return 2;
    unsigned mode=argc==6?(unsigned)strtoul(argv[5],NULL,10):2;
    if ((sensor==4&&lines>5150)||mode<2 || mode>4 || (sensor!=1 && mode!=2) || (mode==3 && lines>12000) || (sensor==3 && lines>6636)) return 2;
    struct timespec began;clock_gettime(CLOCK_MONOTONIC,&began);
    int fd = open(REDWOOD_CAMERA_DEVICE, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct redwood_camera_command c = {.command=REDWOOD_CAMERA_SENSOR,.a=sensor==4?1:sensor,.b=sensor==4?6:sensor==3?5:sensor==1?mode:1};
    if (write(fd,&c,sizeof(c)) != sizeof(c)) { perror("sensor"); close(fd); return 1; }
    c=(struct redwood_camera_command){.command=REDWOOD_CAMERA_EXPOSURE,.a=lines,.b=gain};
    int bad=0;
    for (int i=0;i<36;i++) {
        struct redwood_camera_frame f;
        ssize_t n;
        int retry=0;
        do { n=read(fd,&f,sizeof(f)); } while(n<0 && errno==EAGAIN && ++retry<4);
        if(n!=sizeof(f) || memcmp(f.magic,REDWOOD_CAMERA_MAGIC,8)) { perror("frame"); bad=1; break; }
        if(i==0){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);fprintf(stderr,"first_frame_ms=%.1f\n",(now.tv_sec-began.tv_sec)*1000.0+(now.tv_nsec-began.tv_nsec)/1000000.0);}
        if (i==0 && write(fd,&c,sizeof(c)) != sizeof(c)) { perror("exposure"); bad=1; break; }
        printf("%d sensor=%u %ux%u seq=%llu exposure=%u gain=%u line_ns=%u frame=%u\n",i,f.sensor,f.width,f.height,(unsigned long long)f.sequence,f.exposure_lines,f.gain,f.line_ns,f.frame_lines);
        if(f.sensor!=sensor || (sensor==4&&(f.width!=4864||f.height!=3648||f.line_ns!=6415||f.frame_lines!=5166||f.focus==REDWOOD_CAMERA_NO_FOCUS||(i>4&&(f.exposure_lines!=lines||f.gain!=gain)))) || (sensor==3 && (f.width!=4032 || f.height!=3024 || f.line_ns!=5008 || f.frame_lines!=6652 || (i>4 && (f.exposure_lines!=lines || f.gain!=gain)))) || (sensor==1 && (f.width!=(mode==3?4032u:2016u) || f.height!=(mode==3?3024u:1512u) || f.line_ns!=(mode==3?2740u:1468u) || (i>4 && f.exposure_lines!=(lines&~1u))))) bad=1;
    }
    if (!bad && argc >= 3) {
        struct redwood_camera_frame f;
        if (read(fd,&f,sizeof(f)) != sizeof(f)) { perror("dump header"); bad=1; }
        else {
            size_t bytes=sizeof(f)+(size_t)f.stride*f.height;
            void *frame=bytes<=32*1024*1024?malloc(bytes):NULL;
            if (!frame || read(fd,frame,bytes)!=(ssize_t)bytes) { perror("dump frame"); bad=1; }
            else {
                FILE *out=fopen(argv[2],"wb");
                if (!out) { perror("dump file"); bad=1; }
                else { if (fwrite(frame,1,bytes,out)!=bytes) bad=1; if(fclose(out)) bad=1; }
            }
            free(frame);
        }
    }
    close(fd);
    return bad;
}

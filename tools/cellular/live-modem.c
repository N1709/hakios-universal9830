/* MIT. One bounded S5123 BOOT attempt, followed by verified CP-off cleanup.
 * Reuses the live hardware-proven SPI adapter and tested power lifecycle.
 * No DMA, shared-RAM writes, NV, MAIN, calls or network activity. */
#define LIVE_SPI_ADAPTER_ONLY 1
#include "live-spi.c"
#include "s5123-boot.h"
#include "s5123-live-link.h"
static int link_requested;
static int link_delay(uint32_t);

static const struct {uint64_t base;unsigned pin;} outputs[]={
    {0x15c30120,0},{0x15c30280,0},{0x15c30320,0},{0x107300c0,3},{0x10730060,6}
};
static const unsigned controller_regs[]={S5123_CH,S5123_MODE,S5123_CS,S5123_IRQ,
    S5123_PACKET,S5123_SWAP,S5123_FEEDBACK,S5123_USI_CON};
struct modem {
    struct spi_state spi;
    struct s5123_link link;
    uint32_t con[5],dat[5],changed[5];
    uint32_t mux,divider,mode,pins,pulls,pad_dat,saved[8];
    int clock_changed,mode_changed,controller_saved,pads_changed;
};
/* Debug log volume (2026-11-13: the log grew 38 KB a minute, ~55 MB a day, mostly one line per doorbell and per IPC
 * message -- disk and CPU for nothing). Default quiet; /var/root/cell-verbose (or "verbose on" on the pipe) brings the
 * per-message lines back. Milestones, errors, health and watchdog lines are always written. */
#include <sys/stat.h>
static int cell_verbose=-1;
static int verbose(void)
{
    if (cell_verbose<0) {struct stat vs;cell_verbose=!stat("/var/root/cell-verbose",&vs);}
    return cell_verbose;
}
static void phase(const char *s) {printf("cellular live BOOT: %s\n",s);fflush(stdout);}
static int link_read(uint64_t address,uint32_t *value)
{
    printf("cellular live BOOT: PCIe before read %llx\n",(unsigned long long)address);fflush(stdout);
    return read_word(address,value);
}
static int masked_write(uint64_t address,uint32_t mask,uint32_t value,int verify)
{
    if (link_requested && address==0x14ffe004) {
        /* The sole endpoint write is COMMAND with BME clear. Never echo W1C
         * status bits or enable bus mastering; no arbitrary config writes. */
        if (mask!=0xffff || (value&~0xffffu) || (value&4)) return -1;
        char command[128],response[8192];unsigned long long got;unsigned old,next,back;int used=0;
        snprintf(command,sizeof(command),"w 0x14ffe004 0x%x",value);
        if (request(command,response) || sscanf(response,"%llx: %x -> %x, reads %x %n",
            &got,&old,&next,&back,&used)!=4 || response[used] || got!=address || next!=value ||
            (back&0xffff)!=value) return -1;
        return 0;
    }
    int allowed=address==0x10730068 || (link_requested && s5123_link_allowed(address));
    for (unsigned i=0;i<5;i++)
        if (address==outputs[i].base || address==outputs[i].base+4) allowed=1;
    if (!allowed || (value&~mask)) return -1;
    char command[128],response[8192];unsigned long long got;unsigned old,next,back;int used=0;
    snprintf(command,sizeof(command),"m 0x%llx 0x%x 0x%x",(unsigned long long)address,~mask,value);
    if (request(command,response) || sscanf(response,"%llx: %x -> %x, reads %x %n",
        &got,&old,&next,&back,&used)!=4 || response[used] || got!=address ||
        next!=((old&~mask)|value)) {
        fprintf(stderr,"cellular live BOOT: masked I/O/parse failure %llx\n",(unsigned long long)address);return -1;
    }
    if (verify && (back&mask)!=value) {
        fprintf(stderr,"cellular live BOOT: readback mismatch %llx mask %08x old %08x requested %08x read %08x\n",
            (unsigned long long)address,mask,old,next,back);return -1;
    }
    return 0;
}
static int masked(uint64_t address,uint32_t mask,uint32_t value)
{
    return masked_write(address,mask,value,1);
}
static int restore_mask(uint64_t address,uint32_t mask,uint32_t value)
{
    for (unsigned i=0;i<3;i++) if (!masked(address,mask,value&mask)) return 0;
    return -1;
}
static int ready(void *context)
{
    struct modem *m=context;uint32_t power,active,isolation,g0,g1;
    phase("read CP-off, owner and clock guards");
    if (read_word(0x15860720,&isolation) || read_word(0x15c30284,&power) ||
        read_word(0x15c30384,&active) || read_word(0x10700690,&m->mux) ||
        read_word(0x10701820,&m->divider) || read_word(0x10721048,&m->mode) ||
        read_word(0x10730060,&m->pins) || read_word(0x10730064,&m->pad_dat) ||
        read_word(0x10730068,&m->pulls) || read_word(0x107020fc,&g0) || read_word(0x1070208c,&g1)) return 0;
    if ((isolation&1) || (power&1) || (active&1) || m->mode || (m->pins&0xffff0000u) ||
        (m->mux&(1u<<16)) || (m->divider&(1u<<16)) || !(g0&(1u<<21)) || !(g1&(1u<<21))) return 0;
    for (unsigned i=0;i<5;i++) {
        if (read_word(outputs[i].base,&m->con[i]) || read_word(outputs[i].base+4,&m->dat[i])) return 0;
        unsigned function=(m->con[i]>>(4*outputs[i].pin))&15;
        /* Critical controls may already be held output-low by a prior clean attempt. */
        if (function && !(i<2 && function==1 && !(m->dat[i]&(1u<<outputs[i].pin)))) return 0;
    }
    if (link_requested) {
        m->link.io=(struct s5123_link_io){link_read,masked_write,link_delay,phase};
        if (s5123_link_guard(&m->link)) return 0;
    }
    return 1;
}
static int prepare(void *context)
{
    struct modem *m=context;phase("prepare SPI clocks/controller; pads stay inputs");
    m->clock_changed=1;
    if (write_word(0x10701820,(m->divider&~15u)|3,0) || !clock_wait(0x10701820,(1u<<16)|15,3) ||
        write_word(0x10700690,m->mux&~16u,0) || !clock_wait(0x10700690,(1u<<16)|16,0)) return -1;
    m->mode_changed=1;
    if (write_word(0x10721048,2,1)) return -1;
    for (unsigned i=0;i<8;i++) if (read_word(0x10960000+controller_regs[i],&m->saved[i])) return -1;
    m->controller_saved=1;
    if ((m->saved[0]&3) || m->saved[3] || m->saved[4]) return -1;
    if (write_word(0x10960000,0,1) || write_word(0x10960020,0,1) ||
        write_word(0x10960010,0,1) || write_word(0x1096000c,1,1) || write_word(0x109600c4,0,1)) return -1;
    return 0;
}
static int output(void *context,enum s5123_ap_output signal,int level)
{
    struct modem *m=context;
    if ((unsigned)signal>=5 || (unsigned)level>1) return -1;
    uint64_t base=outputs[signal].base;unsigned pin=outputs[signal].pin;
    m->changed[signal]=1;
    int error=1;
    for (unsigned attempt=0;attempt<(cleanup?3u:1u);attempt++) {
        uint32_t dat;
        /* DAT reads the pad while it is input. Preload the latch without
         * demanding a pad level until CON has selected output mode. */
        if (!masked_write(base+4,1u<<pin,(unsigned)level<<pin,0) &&
            !masked(base,15u<<(4*pin),1u<<(4*pin)) &&
            !read_word(base+4,&dat) && ((dat>>pin)&1)==(unsigned)level) {error=0;break;}
    }
    printf("cellular live BOOT: output %u level %d verified %d\n",signal,level,!error);fflush(stdout);
    return error?-1:0;
}
static int delay(void *context,uint32_t us)
{
    (void)context;
    if (stopped && !cleanup) return -1;
    struct timespec remaining={us/1000000,(long)(us%1000000)*1000};
    while (nanosleep(&remaining,&remaining)) {
        if (errno!=EINTR || (stopped && !cleanup)) return -1;
    }
    return 0;
}
static int link_delay(uint32_t us) {return delay(NULL,us);}
static int route(void *context)
{
    struct modem *m=context;uint32_t con,dat;
    phase("route external SPI16 pads; internal loopback disabled");
    if (read_word(0x10730060,&con) || read_word(0x10730064,&dat) ||
        (con&0xffff0000u)!=0x01000000u || (dat&(1u<<6))) return -1;
    m->pads_changed=1;
    return masked(0x10730068,0xffff0000u,0) || masked(0x10730060,0xffff0000u,0x22220000u) ? -1:0;
}
/* wall time HH:MM:SS.mmm, as redwood-ims.log writes it (A, 2026-11-15: to line up the two logs) */
static const char *ts_ms(void)
{
    static char b[16];struct timeval tv;gettimeofday(&tv,NULL);struct tm tm;localtime_r(&tv.tv_sec,&tm);
    snprintf(b,sizeof(b),"%02d:%02d:%02d.%03d",tm.tm_hour,tm.tm_min,tm.tm_sec,(int)(tv.tv_usec/1000));
    return b;
}
static int inputs(uint32_t *value)
{
    uint32_t active,wake,hold;
    if (read_word(0x15c30384,&active) || read_word(0x15c303c4,&wake) || read_word(0x15c303e4,&hold)) return -1;
    *value=(active&1)|((wake&1)<<1)|((hold&1)<<2);return 0;
}

/* ---- firmware download (A, 2026-11-12): legacy RAW queues in the reserved IPC memory, Samsung's
 * link_start_normal_boot + xmit_to_legacy_link + cbd's SIPC UDL. Only with --download TOC. ---- */
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#include <time.h>
#include "../../xnu/files/bsd/sys/redwood_cp_ipc.h"
#include "s5123-udl.h"
static const char *download_toc;
static int ipc_mapped;
#define RAW_TX_HEAD 0x18u
#define RAW_TX_TAIL 0x1cu
#define RAW_RX_HEAD 0x20u
#define RAW_RX_TAIL 0x24u
#define RAW_TX_BUF  0x3000u
#define RAW_TX_SIZE 0x1fd000u
#define RAW_RX_BUF  0x200000u
#define RAW_RX_SIZE 0x200000u
static int ipc_op(unsigned op,uint32_t offset,const void *in,uint32_t length,void *out)
{
    struct cp_ipc_request q={.version=CP_IPC_API,.op=op,.offset=offset,.length=length};
    struct cp_ipc_reply r={0};size_t size=sizeof(r),qs=CP_IPC_HEADER;
    if ((op==CP_IPC_WRITE || op==CP_IPC_STORE32) && in) {memcpy(q.data,in,length);qs+=length;}
    if (op<=CP_IPC_UNMAP) q.length=0;
    if (sysctlbyname("kern.redwood.cp_ipc",&r,&size,&q,qs)) return -1;
    if (out && (op==CP_IPC_READ || op==CP_IPC_LOAD32)) memcpy(out,r.data,length);
    return 0;
}
static int ipc_load(uint32_t off,uint32_t *v) {return ipc_op(CP_IPC_LOAD32,off,NULL,4,v);}
static int ipc_store(uint32_t off,uint32_t v) {return ipc_op(CP_IPC_STORE32,off,&v,4,NULL);}
static int ipc_write(uint32_t off,const uint8_t *d,uint32_t n)
{
    while (n) {uint32_t c=n>CP_IPC_CHUNK?CP_IPC_CHUNK:n;if (ipc_op(CP_IPC_WRITE,off,d,c,NULL)) return -1;off+=c;d+=c;n-=c;}
    return 0;
}
static int ipc_read(uint32_t off,uint8_t *d,uint32_t n)
{
    while (n) {uint32_t c=n>CP_IPC_CHUNK?CP_IPC_CHUNK:n;if (ipc_op(CP_IPC_READ,off,NULL,c,d)) return -1;off+=c;d+=c;n-=c;}
    return 0;
}
/* init_legacy_link + link_start_normal_boot: indices zero, access 1, magic BOOT */
static int pktproc_info(void);
static int ipc_boot_init(void)
{
    if (ipc_op(CP_IPC_MAP,0,NULL,0,NULL)) {fprintf(stderr,"cellular live DL: IPC map refused\n");return -1;}
    ipc_mapped=1;
    {   /* the whole IPC area zeroed first: it holds whatever RAM held (SR info, kernel time at 0x424 ...) */
        static const uint8_t zero[2048];uint64_t t0=now_us(NULL);
        for (uint32_t o=0;o<0x800000;o+=sizeof(zero)) if (ipc_write(o,zero,sizeof(zero))) return -1;
        printf("cellular live DL: IPC 8 MB zeroed in %llu ms\n",(unsigned long long)((now_us(NULL)-t0)/1000));fflush(stdout);
    }
    for (uint32_t o=0;o<0x28;o+=4) if (ipc_store(o,0)) return -1;
    for (uint32_t o=0x400;o<0x410;o+=4) if (ipc_store(o,0)) return -1;
    if (pktproc_info()) return -1;   /* as pktproc_create() at probe: before the CP boots */
    if (ipc_store(0x408,2u<<14)) return -1;     /* init_control_messages: ap2cp_united_status ds_det=2 (dual SIM) */
    if (ipc_store(4,1) || ipc_store(0,0x424f4f54u)) return -1;
    uint32_t m=0,a=0;if (ipc_load(0,&m) || ipc_load(4,&a) || m!=0x424f4f54u || a!=1) return -1;
    phase("IPC: queues zeroed, access 1, magic BOOT");return 0;
}
static int raw_send(const uint8_t *frame,uint32_t n)
{
    uint32_t in,out;
    if (ipc_load(RAW_TX_HEAD,&in) || ipc_load(RAW_TX_TAIL,&out) || in>=RAW_TX_SIZE || out>=RAW_TX_SIZE) return -1;
    uint32_t space=(out>in?out-in:RAW_TX_SIZE-in+out)-1;if (space<n) return -2;
    uint32_t first=RAW_TX_SIZE-in<n?RAW_TX_SIZE-in:n;
    if (ipc_write(RAW_TX_BUF+in,frame,first) || (n>first && ipc_write(RAW_TX_BUF,frame+first,n-first))) return -1;
    __sync_synchronize();
    return ipc_store(RAW_TX_HEAD,(in+n)%RAW_TX_SIZE);
}
/* one SIPC5 frame from RAW RX, waiting up to ms; returns length (0 = nothing) */
static int raw_recv(uint8_t *buf,uint32_t cap,unsigned ms)
{
    uint64_t deadline=now_us(NULL)+(uint64_t)ms*1000;      /* (usleep(2000) sleeps far longer here: a clock, not a count) */
    for (;;) {
        if (now_us(NULL)>deadline || stopped) return 0;
        uint32_t in,out;
        if (ipc_load(RAW_RX_HEAD,&in) || ipc_load(RAW_RX_TAIL,&out) || in>=RAW_RX_SIZE || out>=RAW_RX_SIZE) return -1;
        if (in!=out) {
            uint8_t h[4];if (ipc_read(RAW_RX_BUF+out,h,4)) return -1;
            uint32_t len=h[2]|(h[3]<<8),total=(len+7)&~7u;
            if (len<4 || total>cap) {fprintf(stderr,"cellular live DL: RX head %x tail %x bad frame %02x %02x len %u\n",in,out,h[0],h[1],len);return -1;}
            uint32_t first=RAW_RX_SIZE-out<total?RAW_RX_SIZE-out:total;
            if (ipc_read(RAW_RX_BUF+out,buf,first) || (total>first && ipc_read(RAW_RX_BUF,buf+first,total-first))) return -1;
            if (ipc_store(RAW_RX_TAIL,(out+total)%RAW_RX_SIZE)) return -1;
            return (int)len;
        }
        usleep(2000);
    }
    return 0;
}
static int udl_step(unsigned stage,enum s5123_udl_op op,const uint8_t *frame,size_t n)
{
    uint8_t reply[64];
    int e=raw_send(frame,(uint32_t)n);
    if (e) {printf("cellular live DL: stage %u op %u: send error %d\n",stage,op,e);fflush(stdout);return -1;}
    int got=raw_recv(reply,sizeof(reply),stage==1 && op==S5123_UDL_START?10000:3000);
    {
        uint32_t w[10];for (unsigned i=0;i<10;i++) ipc_load(i*4,&w[i]);
        printf("cellular live DL: IPC header after:");for (unsigned i=0;i<10;i++) printf(" %08x",w[i]);printf("\n");fflush(stdout);
    }
    uint32_t th=0,tt=0,rh=0,rt=0;ipc_load(RAW_TX_HEAD,&th);ipc_load(RAW_TX_TAIL,&tt);ipc_load(RAW_RX_HEAD,&rh);ipc_load(RAW_RX_TAIL,&rt);
    printf("cellular live DL: stage %u op %u: reply %d bytes; TX head %x tail %x (tail moves = CP consumed); RX head %x tail %x\n",
        stage,op,got,th,tt,rh,rt);
    if (got>0) {printf("cellular live DL: reply");for (int i=0;i<got && i<16;i++) printf(" %02x",reply[i]);printf("\n");}
    fflush(stdout);
    if (got<=0) return -1;
    enum s5123_udl_ack a=s5123_udl_response(reply,(size_t)((got+7)&~7),stage,op);
    printf("cellular live DL: stage %u op %u: %s\n",stage,op,a==S5123_UDL_ACK?"ACK":a==S5123_UDL_PEER_FAIL?"CP reports FAIL":"unexpected reply");
    fflush(stdout);
    return a==S5123_UDL_ACK?0:-1;
}


/* ---- after FIN: Samsung's shmem_cmd_handler, polled (A, 2026-11-13). cp2ap_msg +404, ap2cp_msg +400,
 * united status +408/+40c (DT, DRAM_V1 at the IPC base). int = 0x80|0x40|cmd. AP->CP doorbell =
 * write 0x10000+int_num into EP BAR0+0x60000 (pci_db_addr), through iATU outbound region 1. ---- */
static int doorbell_ready;
static int doorbell(unsigned num)
{
    char r[8192],c[96];
    if (!doorbell_ready) {
        /* iATU region1 (DBI+300000+0x200): MEM, 14d00000..14dfffff -> PCI 14d00000 (EP BAR0) */
        if (request("w 0x13b00200 0x0",r) || request("w 0x13b00208 0x14d00000",r) || request("w 0x13b0020c 0x0",r) ||
            request("w 0x13b00210 0x14dfffff",r) || request("w 0x13b00214 0x14d00000",r) || request("w 0x13b00218 0x0",r) ||
            request("w 0x13b00204 0x80000000",r)) return -1;
        doorbell_ready=1;
    }
    snprintf(c,sizeof(c),"w 0x14d60000 0x%x",0x10000u+num);
    int e=request(c,r);if (e || verbose()) {printf("cellular live DL: doorbell %#x: %s",0x10000u+num,e?"failed\n":r);fflush(stdout);}return e;
}
static int ap2cp(unsigned cmd)
{
    if (ipc_store(0x400,0xc0u|cmd)) return -1;__sync_synchronize();
    printf("cellular live DL: AP -> CP cmd %#x (msg %#x)\n",cmd,0xc0u|cmd);fflush(stdout);
    return doorbell(0);
}
static void fmt_dump(void)
{
    uint32_t rh,rt;if (ipc_load(0x10,&rh) || ipc_load(0x14,&rt)) return;
    while (rh!=rt && rt<0x1000 && rh<0x1000) {
        uint8_t h[4],b[64];if (ipc_read(0x2000+rt,h,4)) return;
        uint32_t len=h[2]|(h[3]<<8),total=(len+3)&~3u;if (len<4 || len>0x1000) {printf("cellular live DL: FMT RX bad len %u at %x\n",len,rt);return;}
        uint32_t show=len<64?len:64,first=0x1000-rt<show?0x1000-rt:show;ipc_read(0x2000+rt,b,first);if (show>first) ipc_read(0x2000,b+first,show-first);
        printf("cellular live DL: FMT RX frame (%u bytes):",len);for (unsigned i=0;i<show;i++) printf(" %02x",b[i]);printf("\n");
        rt=(rt+total)%0x1000;ipc_store(0x14,rt);
    }
    fflush(stdout);
}

/* ---- SBD rings (link_device_memory_sbd.c create_sbd_mem_map + init_sbd_link), this board's DT iodevs in
 * order, minus non-IPC/PS channels and the kor_skt/usa_spr option-region ones; MULTI_RAW -> QoS ch 10,11 ---- */
struct sbd_ch {unsigned ch,ul_len,ul_size,dl_len,dl_size;};
static const struct sbd_ch sbd_chs[]={
    {35,16,0x800,16,0x800},{235,16,0x1000,32,0x1000},{236,16,0x1000,32,0x1000},{245,16,0x800,512,0x800},
    {1,32,0x800,64,0x800},{25,16,0x800,16,0x800},{28,16,0x800,128,0x800},{244,16,0x800,32,0x800},
    {10,1024,0x800,256,0x800},{11,512,0x800,1024,0x800}};
#define SBD_N (sizeof(sbd_chs)/sizeof(sbd_chs[0]))
static uint32_t sbd_rpwp,sbd_pos[SBD_N][2],sbd_lenarr[SBD_N][2],sbd_buf[SBD_N][2];
static uint32_t sbd_rp(unsigned id,unsigned dir) {return sbd_rpwp+2*(id+SBD_N*(dir*2));}
static uint32_t sbd_wp(unsigned id,unsigned dir) {return sbd_rpwp+2*(id+SBD_N*(dir*2+1));}
static int ipc_load16(uint32_t off,uint16_t *v) {uint32_t w;if (ipc_load(off&~3u,&w)) return -1;*v=(uint16_t)(w>>((off&2)*8));return 0;}
static int ipc_store16(uint32_t off,uint16_t v)
{uint32_t w;if (ipc_load(off&~3u,&w)) return -1;unsigned sh=(off&2)*8;w=(w&~(0xffffu<<sh))|((uint32_t)v<<sh);return ipc_store(off&~3u,w);}
static int sbd_init(void)
{
    uint8_t zero[2048]={0};
    for (uint32_t o=0x800;o<0x10000;o+=sizeof(zero)) if (ipc_write(o,zero,sizeof(zero))) return -1;
    uint32_t g=0x800,desc=g+12+32*2*8+32*2*20,buff=0x10000;
    sbd_rpwp=desc;desc+=2*2*2*SBD_N;
    for (unsigned i=0;i<SBD_N;i++) for (unsigned dir=0;dir<2;dir++) {
        unsigned len=dir?sbd_chs[i].dl_len:sbd_chs[i].ul_len,size=dir?sbd_chs[i].dl_size:sbd_chs[i].ul_size;
        sbd_pos[i][dir]=desc;desc+=len*4;sbd_lenarr[i][dir]=desc;desc+=len*4;
        sbd_buf[i][dir]=buff;buff+=len*size;
    }
    if (desc>0x10000 || buff>0x800000) {printf("cellular live DL: SBD layout overflow desc %x buff %x\n",desc,buff);return -1;}
    if (ipc_store(g,1) || ipc_store(g+4,SBD_N) || ipc_store(g+8,sbd_rpwp)) return -1;
    for (unsigned i=0;i<SBD_N;i++) for (unsigned dir=0;dir<2;dir++) {
        unsigned len=dir?sbd_chs[i].dl_len:sbd_chs[i].ul_len,size=dir?sbd_chs[i].dl_size:sbd_chs[i].ul_size;
        for (unsigned k=0;k<len;k++) if (ipc_store(sbd_pos[i][dir]+4*k,sbd_buf[i][dir]+k*size)) return -1;
        uint32_t chd=g+12+(i*2+dir)*8,rbd=g+12+32*2*8+(i*2+dir)*20;
        uint8_t d[20];memset(d,0,sizeof(d));
        d[0]=sbd_chs[i].ch;d[4]=dir;d[6]=1;d[8]=0x81;d[12]=len&0xff;d[13]=len>>8;d[14]=i;d[16]=size&0xff;d[17]=size>>8;
        if (ipc_write(rbd,d,20) || ipc_store(chd,rbd) || ipc_store(chd+4,sbd_pos[i][dir])) return -1;
    }
    printf("cellular live DL: SBD: %u channels, rpwp %x, descriptors end %x, buffers end %x\n",(unsigned)SBD_N,sbd_rpwp,desc,buff);fflush(stdout);
    return 0;
}
/* one SIPC5 frame into channel id's UL ring, then MASK_SEND_DATA to the CP */
static int sbd_send(unsigned id,const uint8_t *frame,uint32_t n)
{
    uint16_t rp,wp;unsigned len=sbd_chs[id].ul_len;
    if (ipc_load16(sbd_rp(id,0),&rp) || ipc_load16(sbd_wp(id,0),&wp) || (unsigned)(wp+1)%len==rp) return -1;
    if (n>sbd_chs[id].ul_size || ipc_write(sbd_buf[id][0]+wp*sbd_chs[id].ul_size,frame,n) ||
        ipc_store(sbd_lenarr[id][0]+4*wp,n)) return -1;
    __sync_synchronize();
    if (ipc_store16(sbd_wp(id,0),(uint16_t)((wp+1)%len))) return -1;
    if (ipc_store(0x400,0x81)) return -1;
    return doorbell(0);
}
static int stay_mode;
static int restart_wanted;     /* watchdog: shut the CP down cleanly, then exec ourselves again */
static char **saved_argv;
static int data_off;          /* user switched mobile data off (Settings / Control Center / nodata) */
#define IMS_CID 3
#define IMS_CH (10+IMS_CID-1)     /* PS channels: cid 2 (internet) arrives on ch 11 */
static struct {int up,npcscf,tries;uint8_t ip6[16],pcscf[2][16];char imsi[16],imei[20];uint64_t tried;} ims;
static void ims_write(void);
static unsigned long long ims_dl_n, ims_dl_sbd;
static void ims_deliver(const uint8_t *p,unsigned n);
static int dm_on;
static void cell_ipc(const uint8_t *m,unsigned len);
static void pdn_ipconfig(const uint8_t *d,unsigned n);
static int sbd_send(unsigned id,const uint8_t *frame,uint32_t n);

/* ---- RFS server (umts_rfs0, ch 245, SBD id 3), after mifmon.c (postmarketOS Note 8, decoded from libsec-ril):
 * a byte stream of frames {u32 len (whole frame), u8 cmd, u8 id, data}, carried in SIPC5 frames of up to
 * 2044 payload bytes. NV read/write go to the phone's private NV copy /var/root/cell-nv.bin (never the EFS
 * partition); file requests: OPEN/INFO -> ENOENT, MKDIR -> 0, CLOSE -> 0, others -> ENOSYS. ---- */
#define RFS_MAX (512*1024)
static uint8_t *rfs_buf;static size_t rfs_have;
static void rfs_put32(uint8_t *p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static uint32_t rfs_get32(const uint8_t *p) {return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
static void rfs_send(uint8_t *r,size_t n,uint8_t cmd,uint8_t id)
{
    rfs_put32(r,(uint32_t)n);r[4]=cmd;r[5]=id;
    for (size_t off=0;off<n;) {
        uint8_t f[2048];size_t c=n-off>2044?2044:n-off;
        f[0]=0xf8;f[1]=245;f[2]=(uint8_t)(c+4);f[3]=(uint8_t)((c+4)>>8);memcpy(f+4,r+off,c);
        for (unsigned t=0;t<200 && sbd_send(3,f,(uint32_t)(c+4));t++) usleep(5000);
        off+=c;
    }
}
static void rfs_ret(uint8_t cmd,uint8_t id,int ret,int err)
{
    uint8_t r[14]={0};rfs_put32(r+6,(uint32_t)ret);rfs_put32(r+10,(uint32_t)(ret<0?err:0));rfs_send(r,14,cmd,id);
}
static void rfs_handle(const uint8_t *q,size_t n)
{
    uint8_t cmd=q[4],id=q[5];
    if (cmd==1 || cmd==2) {
        uint32_t off=n>=14?rfs_get32(q+6):0,len=n>=14?rfs_get32(q+10):0;ssize_t done=-1;
        if (len>RFS_MAX) len=RFS_MAX;
        uint8_t *r=calloc(1,15+len);int fd=open("/var/root/cell-nv.bin",O_RDWR);struct stat st;
        if (r && fd>=0 && !fstat(fd,&st) && (uint64_t)off+len<=(uint64_t)st.st_size) {
            if (cmd==1) done=pread(fd,r+15,len,off);
            else if (n>=14+(size_t)len) {done=pwrite(fd,q+14,len,off);fsync(fd);}
        }
        if (fd>=0) close(fd);
        printf("cellular: RFS NV_%s off 0x%x len %u -> %zd\n",cmd==1?"READ":"WRITE",off,len,done);fflush(stdout);
        if (!r) return;
        r[6]=done==(ssize_t)len;rfs_put32(r+7,off);rfs_put32(r+11,done>0?(uint32_t)done:0);
        rfs_send(r,15+(cmd==1 && done>0?(size_t)done:0),cmd,id);free(r);return;
    }
    char path[200]="";
    if (cmd==0x11 && n>=14) {uint32_t pl=rfs_get32(q+10);if (pl>n-14) pl=(uint32_t)(n-14);if (pl>=sizeof(path)) pl=sizeof(path)-1;memcpy(path,q+14,pl);path[pl]=0;}
    if ((cmd==0x0a||cmd==0x0b||cmd==0x0c||cmd==0x14) && n>=10) {uint32_t pl=rfs_get32(q+6);if (pl>n-10) pl=(uint32_t)(n-10);if (pl>=sizeof(path)) pl=sizeof(path)-1;memcpy(path,q+10,pl);path[pl]=0;}
    for (char *c=path;*c;c++) if (*c<32||*c>126) *c='.';
    printf("cellular: RFS cmd 0x%02x id %u %s\n",cmd,id,path);fflush(stdout);
    switch (cmd) {
    case 0x11: rfs_ret(cmd,id,-1,2);break;                       /* OPEN: ENOENT */
    case 0x06: rfs_ret(cmd,id,0,0);break;                        /* CLOSE */
    case 0x0c: rfs_ret(cmd,id,0,0);break;                        /* MKDIR */
    case 0x0a: case 0x13: {uint8_t r[32]={0};rfs_put32(r+6,(uint32_t)-1);rfs_put32(r+28,2);rfs_send(r,32,cmd,id);break;}
    default: rfs_ret(cmd,id,-1,78);break;                        /* ENOSYS */
    }
}
static void rfs_rx(const uint8_t *p,size_t n)
{
    if (!rfs_buf && !(rfs_buf=malloc(RFS_MAX))) return;
    if (rfs_have+n>RFS_MAX) {printf("cellular: RFS overflow, stream reset\n");rfs_have=0;return;}
    memcpy(rfs_buf+rfs_have,p,n);rfs_have+=n;
    while (rfs_have>=6) {
        uint32_t want=rfs_get32(rfs_buf);
        static uint32_t said;
        if (want!=said) {said=want;printf("cellular: RFS request: %u bytes, cmd 0x%02x id %u (have %zu)\n",want,rfs_buf[4],rfs_buf[5],rfs_have);fflush(stdout);}
        if (want<6 || want>RFS_MAX) {printf("cellular: RFS bad length %u, stream reset\n",want);fflush(stdout);rfs_have=0;return;}
        if (rfs_have<want) return;
        rfs_handle(rfs_buf,want);
        memmove(rfs_buf,rfs_buf+want,rfs_have-want);rfs_have-=want;
    }
}
static void sbd_dump_dl(void)
{
    static int said;
    if (!said++) {      /* what the CP sees: g_desc and the ipc0/dm0 descriptors, as written */
        uint8_t g[48];ipc_read(0x800,g,12);
        printf("cellular live DL: g_desc:");for (unsigned k=0;k<12;k++) printf(" %02x",g[k]);printf("\n");
    }
    for (unsigned id=0;id<SBD_N;id++) {
        uint16_t rp,wp;unsigned len=sbd_chs[id].dl_len;
        if (ipc_load16(sbd_rp(id,1),&rp) || ipc_load16(sbd_wp(id,1),&wp)) return;
        if (rp>=len || wp>=len) continue;      /* (not a valid index: reported by the rp/wp dump) */
        for (unsigned guard=0;rp!=wp && guard<len;guard++) {
            uint32_t n=0;uint8_t b[96];ipc_load(sbd_lenarr[id][1]+4*rp,&n);
            uint8_t big[2100];uint32_t show=sbd_chs[id].dl_size<sizeof(big)?sbd_chs[id].dl_size:sizeof(big);(void)b;(void)n;
            ipc_read(sbd_buf[id][1]+rp*sbd_chs[id].dl_size,big,show);
            uint32_t flen=big[2]|(big[3]<<8);if (big[0]>=0xf8 && flen>=4 && flen<show) show=flen;
            if (sbd_chs[id].ch==235 && show>=11) cell_ipc(big,show);
            if (sbd_chs[id].ch==235 && show>=11 && big[8]==0x0d && big[9]==0x09) pdn_ipconfig(big+11,show-11);
            if (sbd_chs[id].ch==245 && show>4) {rfs_rx(big+4,show-4);show=0;}
            /* the call's voice (A, 2026-11-13): the IMS dedicated bearer's RTP/RTCP comes on the SBD channels 10/11 as
             * bare IPv6, not on pktproc: for our IMS address -> the IMS interface (was only logged: redwood-ims saw none) */
            if ((sbd_chs[id].ch==10 || sbd_chs[id].ch==11) && show>=40 && (big[0]>>4)==6 && ims.up && !memcmp(big+24,ims.ip6,16)) {
                unsigned plen=40u+((unsigned)big[4]<<8|big[5]);
                if (plen<=show) {ims_deliver(big,plen);ims_dl_sbd++;}
                show=0;
            }
            if (sbd_chs[id].ch==25 && show>4) {
                printf("cellular: AT <- ");for (unsigned k=4;k<show;k++) putchar(big[k]=='\r'?' ':big[k]=='\n'?'|':(big[k]>=32&&big[k]<127?big[k]:'.'));printf("\n");fflush(stdout);
                show=0;
            }
            if (sbd_chs[id].ch==235 && show>=14 && big[8]==0x16 && big[9]==0x0b) {
                /* (A, 2026-11-15) IMS 16/0b: the dedicated (voice) bearer. Payload: [0] ?, [1] QCI, [2] 1 up / 3 down,
                 * 4 x u32 bit rates, [19] EPS bearer id, then the TFT: [24..25] our RTP port (LE), [28..43] the far
                 * end's IPv6, [44..45] its port. One line with the wall time, to line up with redwood-ims.log. */
                const uint8_t *q=big+11;unsigned n=show-11;
                if (q[2]==1 && n>=46) {
                    char a6[INET6_ADDRSTRLEN]="";inet_ntop(AF_INET6,q+28,a6,sizeof(a6));
                    printf("%s cellular: IMS voice bearer UP: QCI %u, bearer %u, port %u <-> [%s]:%u\n",ts_ms(),q[1],q[19],
                        q[24]|q[25]<<8,a6,q[44]|q[45]<<8);
                } else printf("%s cellular: IMS voice bearer %s: QCI %u (state %u)\n",ts_ms(),q[2]==3?"DOWN":"change",q[1],q[2]);
                fflush(stdout);
            }
            if (sbd_chs[id].ch==235 && show>=11 && !verbose()) show=0;  /* (quiet: the per-message dump only when verbose) */
            if (sbd_chs[id].ch==235 && show>=11) {
                printf("%s cellular live DL: IPC <- group %02x index %02x type %02x (mseq %u aseq %u, %u bytes):",
                    ts_ms(),big[8],big[9],big[10],big[6],big[7],show-11);
                /* SIM APDU answers, UICC auth, IMSI and IMEI stay out of the log (they go to root-only files) */
                int secret=(big[8]==0x05 && (big[9]==0x06 || big[9]==0x09)) || (big[8]==0x0a && (big[9]==0x02 || big[9]==0x03));
                if (secret) printf(" <not logged>\n");
                else {
                for (unsigned k=11;k<show;k++) printf(" %02x",big[k]);
                printf("  |");for (unsigned k=11;k<show;k++) putchar(big[k]>=32&&big[k]<127?big[k]:'.');printf("|\n");
                }
            } else if (show && sbd_chs[id].ch==28 && stay_mode) {
                if (dm_on) {          /* printable runs of 6+ characters */
                    char run[256];unsigned rl=0;
                    for (unsigned k=0;k<=show;k++) {
                        int c=k<show?big[k]:0;
                        if (c>=32 && c<127 && rl<sizeof(run)-1) run[rl++]=(char)c;
                        else {if (rl>=6) {run[rl]=0;printf("cellular: CP dm: %s\n",run);}rl=0;}
                    }
                    fflush(stdout);
                }
            } else if (show && (!stay_mode || sbd_chs[id].ch!=28)) {     /* (dm0 = the modem's own debug log) */
                printf("cellular live DL: SBD RX ch %u (%u bytes):",sbd_chs[id].ch,show);for (unsigned k=0;k<show && k<48;k++) printf(" %02x",big[k]);printf("\n");
            }
            rp=(uint16_t)((rp+1)%len);ipc_store16(sbd_rp(id,1),rp);
        }
    }
    fflush(stdout);
}

/* pktproc v2 info (link_rx_pktproc.c pktproc_create, CONFIG_CP_PKTPROC_V2): SHMEM_PKTPROC = cp_rmem_2
 * d0000000+64MB, CP view 0x28000000. info 0x100: {num_queues 1, mode 1 SKTBUF, max_packet 2048} +
 * q_info[0] {desc_base cp+0x100, num_desc (0x3f00000/2560)*25/100, data_base cp+0x100000, fore 0, rear 0} */
#define PP_NDESC 256u
#define PP_CELL 2560u
/* the pktproc area: through the kernel's second IPC window (CP_IPC_PP_FLAG, one call per packet) when the
 * kernel has it, else the debug peek ("r"/"w", 64 words a call, page mappings of its own) */
static int poke_read(uint32_t pa,uint32_t *w,unsigned n);
static int poke_write(uint32_t pa,uint32_t v);
static int pp_win=-1;
static int pp_window(void)
{
    if (pp_win<0) {uint32_t v;pp_win=ipc_op(CP_IPC_LOAD32,CP_IPC_PP_FLAG|0x14,NULL,4,&v)==0;
        printf("cellular live DL: pktproc access through %s\n",pp_win?"the kernel's packet window":"the debug peek (old kernel)");fflush(stdout);}
    return pp_win;
}
static int pp_read(uint32_t pa,uint32_t *w,unsigned n)
{return pp_window()?ipc_read(CP_IPC_PP_FLAG|(pa-0xd0000000u),(uint8_t *)w,4*n):poke_read(pa,w,n);}
static int pp_writes(uint32_t pa,const uint32_t *w,unsigned n)
{
    if (pp_window()) return ipc_write(CP_IPC_PP_FLAG|(pa-0xd0000000u),(const uint8_t *)w,4*n);
    for (unsigned k=0;k<n;k++) if (poke_write(pa+4*k,w[k])) return -1;
    return 0;
}
static int pktproc_info(void)
{
    static uint32_t z[64],ring[PP_NDESC*4];
    if (pp_writes(0xd0000000u,z,64)) return -1;
    /* the descriptor ring (16 B each at +0x100): every entry but the last a free RX buffer, as
     * pktproc_fill_data_addr(): data_addr = CP data base + cell offset + NET_SKB_PAD(64); HEAD on the first */
    for (unsigned i=0;i<PP_NDESC;i++) {
        ring[4*i]=i<PP_NDESC-1?0x28100000u+i*PP_CELL+64u:0;ring[4*i+1]=(i==0?0x80u:0)<<8;ring[4*i+2]=ring[4*i+3]=0;
    }
    if (pp_writes(0xd0000100u,ring,PP_NDESC*4)) return -1;
    /* info: {num_queues 1, mode 1 SKTBUF, max_packet 2048} + q_info[0] {desc_base, num_desc, data_base, fore, rear} */
    const uint32_t w[]={1u|(1u<<4)|(2048u<<8),0x28000100u,PP_NDESC,0x28100000u,PP_NDESC-1,0};
    if (pp_writes(0xd0000000u,w,6)) return -1;
    printf("cellular live DL: pktproc v2 at d0000000: %u descriptors, %u free RX buffers (fore %u)\n",PP_NDESC,PP_NDESC-1,PP_NDESC-1);fflush(stdout);
    return 0;
}
/* ---- stay mode (--stay): the CP stays up; state for iOS in /var/tmp/redwood-cell/state ---- */
static int stay_mode;
static struct {int rssi_dbm,bars,reg_raw,have_rssi;char plmn[8];uint64_t at;} cell;
static int data_is_up(void);
static void cell_publish(void)
{
    char tmp[]="/var/tmp/redwood-cell/state.tmp";
    mkdir("/var/tmp/redwood-cell",0755);
    FILE *f=fopen(tmp,"w");if (!f) return;
    const char *op=strncmp(cell.plmn,"4058",4)==0||strncmp(cell.plmn,"40586",5)==0?"Jio":cell.plmn[0]?cell.plmn:"";
    /* ios_reg/ios_data: SystemStatusServer's STTelephonyRegistrationStatus (2 = registered, 3 = notRegistered)
     * and STTelephonyDataConnectionType (7 = lteConnection), as its debug-name functions list them */
    fprintf(f,"data_on %d\n",!data_off);
    fprintf(f,"data_up %d\n",data_is_up());
    fprintf(f,"bars %d\nmax 4\ndbm %d\noperator %s\nplmn %s\nreg %d\nrat LTE\nios_reg %d\nios_data %d\nupdated %llu\n",
        cell.bars,cell.rssi_dbm,op,cell.plmn,cell.reg_raw,(cell.plmn[0] && strcmp(cell.plmn,"000000"))?2:3,7,(unsigned long long)time(NULL));
    fclose(f);chmod(tmp,0644);rename(tmp,"/var/tmp/redwood-cell/state");
}
/* ---- SMS (IPC group 04, from stock libsec-ril IpcProtocol41Sms): 04 07 NOTI device ready -> answer SET 04 07;
 * 04 02 NOTI incoming {msg_type, type, u16 sim_index, tpid, len} + PDU (SMSC first) -> EXEC 04 06 {msg_type, u16 0, tpid, 0} */
static void ctl_line(char *l);
#include "sms-pdu.h"
static void sms_ipc(unsigned i,const uint8_t *d,unsigned n,unsigned type)
{
    char cmd[64];
    static int ready_done;
    if (i==0x07 && type==3) {if (ready_done++) return;ctl_line((char[]){"fmt 04 07 03"});printf("cellular: SMS: device ready answered\n");fflush(stdout);return;}
    if (i!=0x02 || n<6) return;
    unsigned len=d[5];if (6+len>n) len=n-6;
    char from[64]="?",text[1600]="",when[32]="";
    int ok=!sms_decode(d+6,len,from,sizeof(from),text,sizeof(text),when,sizeof(when));
    printf("cellular: SMS from %s at %s: %s%s\n",from,when,ok?text:"(undecoded PDU)",ok?"":"");fflush(stdout);
    FILE *f=fopen("/var/root/cell-sms.txt","a");
    if (f) {fprintf(f,"%llu\t%s\t%s\t",(unsigned long long)time(NULL),from,when);
        if (ok) {for (char *c=text;*c;c++) fputc(*c=='\n'||*c=='\t'?' ':*c,f);}
        else {fputs("pdu:",f);for (unsigned k=0;k<len;k++) fprintf(f,"%02x",d[6+k]);}
        fputc('\n',f);fclose(f);}
    snprintf(cmd,sizeof(cmd),"fmt 04 06 01 %02x 00 00 %02x 00",d[0],d[4]);ctl_line(cmd);
}
static void cell_ipc(const uint8_t *m,unsigned len)
{
    if (len<11) return;
    unsigned g=m[8],i=m[9];const uint8_t *d=m+11;unsigned n=len-11;int changed=0;
    if (g==0x07 && i==0x06 && n>=1) {               /* DISP_RSSI_INFO: first byte = -dBm */
        int dbm=-(int)d[0],bars=dbm>=-90?4:dbm>=-100?3:dbm>=-108?2:dbm>=-115?1:0;
        if (!cell.have_rssi || dbm!=cell.rssi_dbm) {cell.rssi_dbm=dbm;cell.bars=bars;cell.have_rssi=1;changed=1;}
    }
    if (g==0x08 && i==0x03 && n>=9) {               /* NET_CURRENT_PLMN: "...!" then 6 digits */
        char p[8]={0};for (unsigned k=0;k<6 && 3+k<n;k++) p[k]=(char)d[3+k];
        if (p[0]>='0' && p[0]<='9' && strcmp(p,cell.plmn)) {strcpy(cell.plmn,p);changed=1;}
    }
    if (g==0x08 && i==0x05 && n>=2) {if (cell.reg_raw!=d[1]) {cell.reg_raw=d[1];changed=1;}}
    if (g==0x04) sms_ipc(i,d,n,m[10]);
    if (g==0x0a && i==0x03 && m[10]==2 && n>=2 && d[0]==1 && d[1]<20 && 2u+d[1]<=n) {memcpy(ims.imei,d+2,d[1]);ims.imei[d[1]]=0;ims_write();}
    if (g==0x0a && i==0x02 && m[10]==2 && n>=1 && d[0]<16 && 1u+d[0]<=n) {memcpy(ims.imsi,d+1,d[0]);ims.imsi[d[0]]=0;ims_write();}
    if (g==0x05 && i==0x06) {
        FILE *f=fopen("/var/tmp/redwood-cell/apdu.tmp","w");
        if (f) {fchmod(fileno(f),0600);for (unsigned k=0;k<n;k++) fprintf(f,"%02x",d[k]);fputc('\n',f);fclose(f);rename("/var/tmp/redwood-cell/apdu.tmp","/var/tmp/redwood-cell/apdu");}
    }
    if (g==0x05 && i==0x09) {     /* UICC auth answer: written raw (hex) for redwood-ims */
        FILE *f=fopen("/var/tmp/redwood-cell/auth.tmp","w");
        if (f) {fchmod(fileno(f),0600);fprintf(f,"type %u\n",m[10]);for (unsigned k=0;k<n;k++) fprintf(f,"%02x",d[k]);fputc('\n',f);fclose(f);rename("/var/tmp/redwood-cell/auth.tmp","/var/tmp/redwood-cell/auth");}
        printf("cellular: SIM auth answer (%u bytes)\n",n);fflush(stdout);
    }
    if (changed) cell_publish();
}

/* ---- control FIFO (stay mode): /var/tmp/redwood-cell/cmd, one command per line:
 *   at <AT command>                  -> umts_router (ch 25, SBD id 5), "\r" appended
 *   fmt <group> <index> <type> [hex] -> umts_ipc0 Samsung IPC (ch 235, SBD id 1)
 * replies are logged ("AT <-" lines for ch 25, "IPC <-" for ch 235) ---- */

/* ---- packets (A, 2026-11-13): data call cid 2 = rmnet1 = PS ch 11. DL through the packet processor's
 * descriptors (rear_ptr written by the CP), UL as raw IP in the multipdp SBD ring (id 9), buff_len|ch<<16 ---- */
static struct {int up;uint8_t ip6[16],dns6[16];unsigned mtu;} pdn;
static unsigned ul_ch=11,ul_ring=8;     /* QoS high-priority multipdp ring carries UL (ring 9 is consumed but not sent) */
static unsigned pp_done;
static uint64_t pp_rx,pp_tx;
static int poke_read1(uint32_t pa,uint32_t *w,unsigned n);
static int poke_read(uint32_t pa,uint32_t *w,unsigned n)
{   /* the kernel peek returns at most 64 words per call */
    for (unsigned o=0;o<n;o+=64) if (poke_read1(pa+4*o,w+o,n-o>64?64:n-o)) return -1;
    return 0;
}
static int poke_read1(uint32_t pa,uint32_t *w,unsigned n)
{
    char c[64],r[8192];snprintf(c,sizeof(c),"r 0x%x %u",pa,n);if (request(c,r)) return -1;
    unsigned got=0;char *save=NULL;
    for (char *t=strtok_r(r," \n",&save);t && got<n;t=strtok_r(NULL," \n",&save)) {
        size_t L=strlen(t);if (L && t[L-1]==':') continue;          /* "0xADDR:" line prefixes */
        w[got++]=(uint32_t)strtoul(t,NULL,16);
    }
    return got==n?0:-1;
}
static int poke_write(uint32_t pa,uint32_t v) {char c[64],r[8192];snprintf(c,sizeof(c),"w 0x%x 0x%x",pa,v);return request(c,r);}
static void pdn_ipconfig(const uint8_t *d,unsigned n)
{
    if (n>=0x53 && d[0]==IMS_CID) {
        memcpy(ims.ip6,d+0x23,16);ims.npcscf=0;
        for (int k=0;k<2;k++) {static const uint8_t z[16];if (memcmp(d+0x33+16*k,z,16)) memcpy(ims.pcscf[ims.npcscf++],d+0x33+16*k,16);}
        ims.up=1;printf("cellular: IMS PDN up, %d P-CSCF\n",ims.npcscf);fflush(stdout);ims_write();
    }
    if (n>=1 && d[0]!=2) {   /* other PDNs (IMS on cid 3): dump everything, P-CSCF is in here somewhere */
        printf("cellular: PDN cid %u ipconfig (%u bytes):",d[0],n);for (unsigned k=0;k<n;k++) printf("%s%02x",k%32?" ":"\n  ",d[k]);printf("\n");
        for (unsigned k=0;k+16<=n;k++) if (d[k]==0x24 && d[k+1]==0x05) {char t[64];inet_ntop(AF_INET6,d+k,t,sizeof(t));printf("cellular: cid %u IPv6-looking field at 0x%x: %s\n",d[0],k,t);}
        fflush(stdout);return;
    }
    if (n<0x9a) return;
    memcpy(pdn.ip6,d+0x23,16);memcpy(pdn.dns6,d+0x63,16);pdn.mtu=d[0x98]|(d[0x99]<<8);pdn.up=1;
    printf("cellular: data PDN cid 2 up: IPv6 %02x%02x:%02x%02x:..., DNS %02x%02x:%02x%02x:..., MTU %u\n",
        pdn.ip6[0],pdn.ip6[1],pdn.ip6[2],pdn.ip6[3],pdn.dns6[0],pdn.dns6[1],pdn.dns6[2],pdn.dns6[3],pdn.mtu);fflush(stdout);
}
static void pkt_rx(const uint8_t *p,unsigned n);
static void ims_deliver(const uint8_t *p,unsigned n);
static uint32_t pp_fore=PP_NDESC-1;   /* AP-owned: the CP only reads it */
static unsigned pp_lost;
static uint32_t pp_last_rear;
static void pp_give_back(void)
{   /* fore follows done: every descriptor up to the one before pp_done is the CP's to fill again. Kept as a
     * function of pp_done, not a running sum: a read that failed half-way through pp_poll used to return before
     * the handed count reached fore, the CP's free count shrank for good, and the downlink stopped when it hit
     * zero -- the "downlink silent" modem restarts of 2026-11-13 (suspected; the health line shows it). */
    uint32_t want=(pp_done+PP_NDESC-1)%PP_NDESC;
    if (want!=pp_fore && !pp_writes(0xd0000010,&want,1)) pp_fore=want;
}
static void pp_poll(void)
{
    uint32_t rear=0;if (pp_read(0xd0000014,&rear,1) || rear>=PP_NDESC) {pp_give_back();return;}
    pp_last_rear=rear;
    unsigned handed=0;
    while (pp_done!=rear) {
        /* the new descriptors up to the ring's end (or rear) in one read */
        unsigned cnt=rear>pp_done?rear-pp_done:PP_NDESC-pp_done;if (cnt>64) cnt=64;
        static uint32_t dw[64*4];uint32_t a0=0xd0000100u+16*pp_done;
        if (pp_read(a0,dw,4*cnt)) {pp_give_back();return;}
        for (unsigned k=0;k<cnt;k++) {
            uint32_t *d=dw+4*k;unsigned idx=pp_done+k;
            unsigned len=d[2]&0xffff,ch=(d[3]>>16)&0xff;uint32_t addr=d[0];
            if (len && len<=2048 && addr>=0x28100000u && addr<0x2c000000u) {
                uint32_t words[520];unsigned nw=(len+3)/4;
                if (!pp_read(addr-0x28000000u+0xd0000000u,words,nw)) {pp_rx++;if (ch==IMS_CH) ims_deliver((const uint8_t *)words,len);else pkt_rx((const uint8_t *)words,len);}
                else if (pp_lost++<20) {printf("cellular: DL desc %u: buffer %#x unreadable, packet lost\n",idx,addr);fflush(stdout);}
            }
            if (pp_rx<20) {printf("cellular: DL desc %u len %u ch %u status %02x\n",idx,len,ch,(d[1]>>16)&0xff);fflush(stdout);}
            /* hand the buffer back: same address, status cleared */
            d[0]=0x28100000u+idx*PP_CELL+64u;d[1]=idx==0?0x8000u:0;d[2]=d[3]=0;
        }
        if (pp_writes(a0,dw,4*cnt)) {pp_give_back();return;}
        pp_done=(pp_done+cnt)%PP_NDESC;handed+=cnt;
    }
    (void)handed;pp_give_back();
}
static int pkt_tx_ch(const uint8_t *p,unsigned n,unsigned ch);
static int pkt_tx(const uint8_t *p,unsigned n) {return pkt_tx_ch(p,n,ul_ch);}
static const char *tcp_info(const uint8_t *p,unsigned n)
{   /* debug: "sport>dport flags dst-tail" for TCP */
    static char b[96];if (n<60 || (p[0]>>4)!=6 || p[6]!=6) return "";
    const uint8_t *t=p+40;unsigned f=t[13];
    snprintf(b,sizeof(b)," tcp %u>%u %s%s%s%s%s src ..%02x%02x dst ..%02x%02x",(t[0]<<8)|t[1],(t[2]<<8)|t[3],
        f&2?"S":"",f&16?"A":"",f&1?"F":"",f&4?"R":"",f&8?"P":"",p[22],p[23],p[38],p[39]);
    return b;
}
static const char *l4_check(const uint8_t *p,unsigned n)
{   /* debug: the TCP/UDP checksum over the IPv6 pseudo-header */
    if (n<48 || (p[0]>>4)!=6 || (p[6]!=6 && p[6]!=17)) return "-";
    unsigned pl=(p[4]<<8)|p[5];if (40+pl>n) return "SHORT";
    uint32_t sum=0;for (int k=8;k<40;k+=2) sum+=(p[k]<<8)|p[k+1];
    sum+=pl+p[6];
    for (unsigned k=40;k+1<40+pl;k+=2) sum+=(p[k]<<8)|p[k+1];
    if (pl&1) sum+=p[40+pl-1]<<8;
    while (sum>>16) sum=(sum&0xffff)+(sum>>16);
    return sum==0xffff?"csum ok":"CSUM BAD";
}
static int pkt_tx_ch2(const uint8_t *p,unsigned n,unsigned ch);
static int ul_debug;
static int pkt_tx_ch(const uint8_t *p,unsigned n,unsigned ch)
{
    int e=pkt_tx_ch2(p,n,ch);
    if (ul_debug>0) {ul_debug--;
        const char *ck=l4_check(p,n);
        printf("cellular: UL %u bytes next %u ch %u ring %u -> %s, %s%s\n",n,n>6?p[6]:0,ch,ul_ring,e?"FAILED":"ok",ck,tcp_info(p,n));fflush(stdout);}
    return e;
}
static int pkt_tx_ch2(const uint8_t *p,unsigned n,unsigned ch)
{
    unsigned id=ul_ring;uint16_t rp,wp;unsigned len=sbd_chs[id].ul_len;
    if (ipc_load16(sbd_rp(id,0),&rp) || ipc_load16(sbd_wp(id,0),&wp) || (unsigned)(wp+1)%len==rp) return -1;
    if (n>sbd_chs[id].ul_size || ipc_write(sbd_buf[id][0]+wp*sbd_chs[id].ul_size,p,n) ||
        ipc_store(sbd_lenarr[id][0]+4*wp,n|(ch<<16))) return -1;
    __sync_synchronize();
    if (ipc_store16(sbd_wp(id,0),(uint16_t)((wp+1)%len))) return -1;
    pp_tx++;ipc_store(0x400,0x81);return doorbell(0);
}
static uint16_t csum16(const uint8_t *p,size_t n,uint32_t sum)
{
    for (size_t i=0;i+1<n;i+=2) sum+=(p[i]<<8)|p[i+1];if (n&1) sum+=p[n-1]<<8;
    while (sum>>16) sum=(sum&0xffff)+(sum>>16);return (uint16_t)~sum;
}
static void ping6(const char *dst)
{
    uint8_t ip[16];if (!pdn.up || inet_pton(AF_INET6,dst,ip)!=1) {printf("cellular: ping6: no PDN or bad address\n");return;}
    uint8_t pk[48+16];memset(pk,0,sizeof(pk));
    pk[0]=0x60;pk[4]=0;pk[5]=8+16;pk[6]=58;pk[7]=64;memcpy(pk+8,pdn.ip6,16);memcpy(pk+24,ip,16);
    uint8_t *ic=pk+40;ic[0]=128;ic[4]=0x12;ic[5]=0x34;ic[7]=1;memcpy(ic+8,"redwood-s20-lte!",16);
    uint8_t ph[40];memset(ph,0,sizeof(ph));memcpy(ph,pk+8,32);ph[35]=24;ph[39]=58;
    uint32_t s0=0;for (int i=0;i<40;i+=2) s0+=(ph[i]<<8)|ph[i+1];
    uint16_t c=csum16(ic,24,s0);ic[2]=(uint8_t)(c>>8);ic[3]=(uint8_t)c;
    int e=pkt_tx(pk,64);printf("cellular: ping6 %s sent (%s)\n",dst,e?"UL ring refused":"ok");fflush(stdout);
}
static void utun_deliver(const uint8_t *p,unsigned n);
static void pkt_rx(const uint8_t *p,unsigned n)
{
    /* (A, 2026-11-14) a packet for the IMS address on the data PDN's channel (a dedicated bearer can land here) */
    if (n>=40 && (p[0]>>4)==6 && ims.up && !memcmp(p+24,ims.ip6,16)) {ims_deliver(p,n);return;}
    if (ul_debug>0) {printf("cellular: DL %u bytes next %u -> %s%s\n",n,n>6?p[6]:0,l4_check(p,n),tcp_info(p,n));fflush(stdout);}
    utun_deliver(p,n);
    if (n>=40 && (p[0]>>4)==6 && p[6]==58 && n>=48 && p[40]==129)
        {printf("cellular: ping6 REPLY from %02x%02x:%02x%02x:... (%u bytes)\n",p[8],p[9],p[10],p[11],n);fflush(stdout);}
    else if (pp_rx<20) {printf("cellular: DL packet %u bytes, IP v%u next %u\n",n,p[0]>>4,p[6]);fflush(stdout);}
}

/* ---- utun: the data PDN as an iOS network interface (IPv6) ---- */
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_dl.h>
/* (not in the iOS SDK; the kernel ABI is macOS's: sys/kern_control.h, sys/sys_domain.h, net/route.h) */
#define AF_SYSTEM 32
#define PF_SYSTEM AF_SYSTEM
#define AF_SYS_CONTROL 2
#define SYSPROTO_CONTROL 2
struct ctl_info {uint32_t ctl_id;char ctl_name[96];};
#define CTLIOCGINFO _IOWR('N',3,struct ctl_info)
struct sockaddr_ctl {u_char sc_len,sc_family;uint16_t ss_sysaddr;uint32_t sc_id,sc_unit,sc_reserved[5];};
struct rt_metrics {uint32_t rmx_locks,rmx_mtu,rmx_hopcount;int32_t rmx_expire;uint32_t rmx_recvpipe,rmx_sendpipe,rmx_ssthresh,rmx_rtt,rmx_rttvar,rmx_pksent,rmx_state,rmx_filler[3];};
struct rt_msghdr {u_short rtm_msglen;u_char rtm_version,rtm_type;u_short rtm_index;int rtm_flags,rtm_addrs;pid_t rtm_pid;int rtm_seq,rtm_errno,rtm_use;uint32_t rtm_inits;struct rt_metrics rtm_rmx;};
#define RTM_VERSION 5
#define RTM_ADD 0x1
#define RTF_UP 0x1
#define RTF_STATIC 0x800
#define RTA_DST 0x1
#define RTA_GATEWAY 0x2
#define RTA_NETMASK 0x4
#include <netinet/in.h>
#ifndef UTUN_OPT_IFNAME
#define UTUN_OPT_IFNAME 2
#endif
struct rw_in6_addrlifetime {time_t ia6t_expire,ia6t_preferred;uint32_t ia6t_vltime,ia6t_pltime;};
struct rw_in6_aliasreq {char ifra_name[IFNAMSIZ];struct sockaddr_in6 ifra_addr,ifra_dstaddr,ifra_prefixmask;int ifra_flags;struct rw_in6_addrlifetime ifra_lifetime;};
#define RW_SIOCAIFADDR_IN6 _IOW('i',26,struct rw_in6_aliasreq)
static int utun_fd=-1;static char utun_name[IFNAMSIZ];
static int data_is_up(void) {return pdn.up && utun_fd>=0 && !data_off;}
/* configd: publish the cellular service (State:/Network/Service/redwood-cell/{IPv6,DNS}) so IPMonitor
 * installs the DNS resolver and ranks the interface like any other service. Loaded at runtime. */
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
static void sc_publish(void)
{
    void *sc=dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration",RTLD_NOW);
    if (!sc) {printf("cellular: configd: no SystemConfiguration\n");return;}
    void *(*create)(CFAllocatorRef,CFStringRef,void *,void *)=(void *(*)(CFAllocatorRef,CFStringRef,void *,void *))dlsym(sc,"SCDynamicStoreCreate");
    Boolean (*setm)(void *,CFDictionaryRef,CFArrayRef,CFArrayRef)=(Boolean (*)(void *,CFDictionaryRef,CFArrayRef,CFArrayRef))dlsym(sc,"SCDynamicStoreSetMultiple");
    if (!create || !setm) return;
    static void *store;if (!store) store=create(NULL,CFSTR("redwood-cellulard"),NULL,NULL);
    if (!store) {printf("cellular: configd: no store\n");return;}
    char a[64],d[64];inet_ntop(AF_INET6,pdn.ip6,a,sizeof(a));inet_ntop(AF_INET6,pdn.dns6,d,sizeof(d));
    CFStringRef ifn=CFStringCreateWithCString(NULL,utun_name,kCFStringEncodingUTF8);
    CFStringRef sa=CFStringCreateWithCString(NULL,a,kCFStringEncodingUTF8),sd=CFStringCreateWithCString(NULL,d,kCFStringEncodingUTF8);
    int pl=64;CFNumberRef pn=CFNumberCreate(NULL,kCFNumberIntType,&pl);
    const void *av[]={sa},*pv[]={pn},*dv[]={sd};
    CFArrayRef aa=CFArrayCreate(NULL,av,1,&kCFTypeArrayCallBacks),pa=CFArrayCreate(NULL,pv,1,&kCFTypeArrayCallBacks),da=CFArrayCreate(NULL,dv,1,&kCFTypeArrayCallBacks);
    const void *k6[]={CFSTR("Addresses"),CFSTR("PrefixLength"),CFSTR("InterfaceName"),CFSTR("Router")};
    const void *v6[]={aa,pa,ifn,sa};   /* point-to-point: router = our own address (as VPN services do) */
    CFDictionaryRef ip6=CFDictionaryCreate(NULL,k6,v6,4,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    const void *kd[]={CFSTR("ServerAddresses"),CFSTR("InterfaceName")},*vd[]={da,ifn};
    CFDictionaryRef dns=CFDictionaryCreate(NULL,kd,vd,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    const void *ks[]={CFSTR("State:/Network/Service/redwood-cell/IPv6"),CFSTR("State:/Network/Service/redwood-cell/DNS")},*vs[]={ip6,dns};
    CFDictionaryRef all=CFDictionaryCreate(NULL,ks,vs,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    Boolean ok=setm(store,all,NULL,NULL);
    printf("cellular: configd service redwood-cell %s (IPv6 %s, DNS %s on %s)\n",ok?"published":"REFUSED",a,d,utun_name);fflush(stdout);
    CFRelease(all);CFRelease(dns);CFRelease(ip6);CFRelease(aa);CFRelease(pa);CFRelease(da);CFRelease(pn);CFRelease(sa);CFRelease(sd);CFRelease(ifn);
}
static int tun_open(const uint8_t ip6[16],unsigned mtu,char *utun_name,int autoconf)
{
    int fd=socket(PF_SYSTEM,SOCK_DGRAM,SYSPROTO_CONTROL);if (fd<0) {perror("cellular: utun socket");return -1;}
    struct ctl_info ci;memset(&ci,0,sizeof(ci));strlcpy(ci.ctl_name,"com.apple.net.utun_control",sizeof(ci.ctl_name));
    if (ioctl(fd,CTLIOCGINFO,&ci)<0) {perror("cellular: CTLIOCGINFO");close(fd);return -1;}
    struct sockaddr_ctl sc;memset(&sc,0,sizeof(sc));sc.sc_len=sizeof(sc);sc.sc_family=AF_SYSTEM;sc.ss_sysaddr=AF_SYS_CONTROL;sc.sc_id=ci.ctl_id;sc.sc_unit=0;
    if (connect(fd,(struct sockaddr *)&sc,sizeof(sc))<0) {perror("cellular: utun connect");close(fd);return -1;}
    socklen_t nl=sizeof(utun_name);getsockopt(fd,SYSPROTO_CONTROL,UTUN_OPT_IFNAME,utun_name,&nl);
    fcntl(fd,F_SETFL,O_NONBLOCK);
    /* (A, 2026-11-19) close-on-exec: the IMS client (posix_spawn) and the exec restart inherited the tunnel, so an old
     * utun outlived the service with the default route on it (no data after a restart until a reboot) */
    fcntl(fd,F_SETFD,FD_CLOEXEC);
    int s6=socket(AF_INET6,SOCK_DGRAM,0);
    struct rw_in6_aliasreq ar;memset(&ar,0,sizeof(ar));strlcpy(ar.ifra_name,utun_name,IFNAMSIZ);
    ar.ifra_addr.sin6_len=sizeof(struct sockaddr_in6);ar.ifra_addr.sin6_family=AF_INET6;memcpy(&ar.ifra_addr.sin6_addr,ip6,16);
    ar.ifra_prefixmask.sin6_len=sizeof(struct sockaddr_in6);ar.ifra_prefixmask.sin6_family=AF_INET6;memset(&ar.ifra_prefixmask.sin6_addr,0xff,8);
    ar.ifra_lifetime.ia6t_vltime=ar.ifra_lifetime.ia6t_pltime=0xffffffff;
    /* (A, 2026-11-19) the data PDN's address as autoconf (IN6_IFF_AUTOCONF): its /64 prefix is then autonomous, which
     * iOS's CLAT46 needs (in6ctl_clat46start) for IPv4 over IPv6-only Jio, for apps and the hotspot */
    if (autoconf) ar.ifra_flags=0x40;
    if (ioctl(s6,RW_SIOCAIFADDR_IN6,&ar)<0) perror("cellular: SIOCAIFADDR_IN6");
    struct ifreq ifr;memset(&ifr,0,sizeof(ifr));strlcpy(ifr.ifr_name,utun_name,IFNAMSIZ);ifr.ifr_mtu=(int)(mtu?mtu:1500);
    if (ioctl(s6,SIOCSIFMTU,&ifr)<0) perror("cellular: SIOCSIFMTU");
    memset(&ifr,0,sizeof(ifr));strlcpy(ifr.ifr_name,utun_name,IFNAMSIZ);
    if (!ioctl(s6,SIOCGIFFLAGS,&ifr)) {ifr.ifr_flags|=IFF_UP|IFF_RUNNING;ioctl(s6,SIOCSIFFLAGS,&ifr);}
    close(s6);
    return fd;
}
static void utun_up(void)
{
    if (utun_fd>=0 || !pdn.up) return;
    int fd=tun_open(pdn.ip6,pdn.mtu,utun_name,1);if (fd<0) return;
    /* IPv6 default route through the interface (fails harmlessly if Wi-Fi already holds one) */
    int rs=socket(PF_ROUTE,SOCK_RAW,0);
    if (rs>=0) {
        struct {struct rt_msghdr h;struct sockaddr_in6 dst;struct sockaddr_dl gw;struct sockaddr_in6 mask;} m;memset(&m,0,sizeof(m));
        m.h.rtm_msglen=sizeof(m);m.h.rtm_version=RTM_VERSION;m.h.rtm_type=RTM_ADD;m.h.rtm_flags=RTF_UP|RTF_STATIC;
        m.h.rtm_addrs=RTA_DST|RTA_GATEWAY|RTA_NETMASK;m.h.rtm_seq=1;
        m.dst.sin6_len=sizeof(m.dst);m.dst.sin6_family=AF_INET6;
        m.gw.sdl_len=sizeof(m.gw);m.gw.sdl_family=AF_LINK;m.gw.sdl_index=(u_short)if_nametoindex(utun_name);
        m.mask.sin6_len=sizeof(m.mask);m.mask.sin6_family=AF_INET6;
        if (write(rs,&m,sizeof(m))<0) perror("cellular: default route");
        close(rs);
    }
    utun_fd=fd;
    printf("cellular: %s up (index %u), IPv6 from the modem, MTU %u\n",utun_name,if_nametoindex(utun_name),pdn.mtu);fflush(stdout);
    sc_publish();
    cell_publish();
}
static int ims_fd=-1;static char ims_name[IFNAMSIZ];
static void ims_write(void)
{   /* for the SIP client (redwood-ims): root-only, the IMSI is in here */
    char t[64];FILE *f=fopen("/var/tmp/redwood-cell/ims.tmp","w");if (!f) return;fchmod(fileno(f),0600);
    fprintf(f,"up %d\nifname %s\n",ims.up && ims_fd>=0,ims_name);
    inet_ntop(AF_INET6,ims.ip6,t,sizeof(t));fprintf(f,"ip6 %s\n",t);
    for (int k=0;k<ims.npcscf;k++) {inet_ntop(AF_INET6,ims.pcscf[k],t,sizeof(t));fprintf(f,"pcscf %s\n",t);}
    if (ims.imsi[0]) fprintf(f,"imsi %s\n",ims.imsi);
    if (ims.imei[0]) fprintf(f,"imei %s\n",ims.imei);
    fclose(f);rename("/var/tmp/redwood-cell/ims.tmp","/var/tmp/redwood-cell/ims");
}
static void tun_route6(const char *ifn,const uint8_t *dst,int host)
{   /* dst/128 (host) or ::/0 through the interface */
    int rs=socket(PF_ROUTE,SOCK_RAW,0);if (rs<0) return;
    struct {struct rt_msghdr h;struct sockaddr_in6 dst;struct sockaddr_dl gw;struct sockaddr_in6 mask;} m;memset(&m,0,sizeof(m));
    m.h.rtm_msglen=sizeof(m);m.h.rtm_version=RTM_VERSION;m.h.rtm_type=RTM_ADD;m.h.rtm_flags=RTF_UP|RTF_STATIC|(host?0x4:0);  /* RTF_HOST */
    m.h.rtm_addrs=RTA_DST|RTA_GATEWAY|RTA_NETMASK;m.h.rtm_seq=2;
    m.dst.sin6_len=sizeof(m.dst);m.dst.sin6_family=AF_INET6;if (dst) memcpy(&m.dst.sin6_addr,dst,16);
    m.gw.sdl_len=sizeof(m.gw);m.gw.sdl_family=AF_LINK;m.gw.sdl_index=(u_short)if_nametoindex(ifn);
    m.mask.sin6_len=sizeof(m.mask);m.mask.sin6_family=AF_INET6;if (host) memset(&m.mask.sin6_addr,0xff,16);
    if (write(rs,&m,sizeof(m))<0) perror("cellular: route");
    close(rs);
}
static void ims_up(void)
{
    if (ims_fd>=0 || !ims.up) return;
    ims_fd=tun_open(ims.ip6,1500,ims_name,0);if (ims_fd<0) return;
    for (int k=0;k<ims.npcscf;k++) tun_route6(ims_name,ims.pcscf[k],1);
    printf("cellular: IMS %s up (cid %d, ch %d, %d P-CSCF)\n",ims_name,IMS_CID,IMS_CH,ims.npcscf);fflush(stdout);ims_write();
}
static void ims_poll(void)
{
    if (ims_fd<0) return;
    uint8_t b[2100];
    for (int k=0;k<32;k++) {ssize_t n=read(ims_fd,b,sizeof(b));if (n<=4) break;if (pkt_tx_ch(b+4,(unsigned)(n-4),IMS_CH)) break;}
}
static void ims_deliver(const uint8_t *p,unsigned n)
{
    ims_dl_n++;
    static unsigned shown;if (shown++<10) {printf("cellular: IMS DL %u bytes\n",n);fflush(stdout);}
    if (ims_fd<0 || n>2048) return;
    uint8_t b[2100];uint32_t af=htonl((p[0]>>4)==6?AF_INET6:AF_INET);memcpy(b,&af,4);memcpy(b+4,p,n);
    (void)write(ims_fd,b,n+4);
}
static void utun_down(void)
{
    if (utun_fd<0) return;
    close(utun_fd);utun_fd=-1;     /* the interface, its address and route go with the socket */
    void *sc=dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration",RTLD_NOW);
    void *(*create)(CFAllocatorRef,CFStringRef,void *,void *)=sc?(void *(*)(CFAllocatorRef,CFStringRef,void *,void *))dlsym(sc,"SCDynamicStoreCreate"):NULL;
    Boolean (*setm)(void *,CFDictionaryRef,CFArrayRef,CFArrayRef)=sc?(Boolean (*)(void *,CFDictionaryRef,CFArrayRef,CFArrayRef))dlsym(sc,"SCDynamicStoreSetMultiple"):NULL;
    void *st=create?create(NULL,CFSTR("redwood-cellulard-off"),NULL,NULL):NULL;
    if (st && setm) {const void *k[]={CFSTR("State:/Network/Service/redwood-cell/IPv6"),CFSTR("State:/Network/Service/redwood-cell/DNS")};
        CFArrayRef rm=CFArrayCreate(NULL,k,2,&kCFTypeArrayCallBacks);setm(st,NULL,rm,NULL);CFRelease(rm);CFRelease(st);}
    printf("cellular: mobile data off (%s closed, configd service removed)\n",utun_name);fflush(stdout);
    cell_publish();
}
static void utun_poll(void)
{
    if (utun_fd<0) return;
    uint8_t b[2100];
    for (int k=0;k<32;k++) {
        ssize_t n=read(utun_fd,b,sizeof(b));if (n<=4) break;
        if (pkt_tx(b+4,(unsigned)(n-4))) break;
    }
}
static void utun_deliver(const uint8_t *p,unsigned n)
{
    if (utun_fd<0 || n>2048) return;
    uint8_t b[2100];uint32_t af=htonl((p[0]>>4)==6?AF_INET6:AF_INET);memcpy(b,&af,4);memcpy(b+4,p,n);
    ssize_t w=write(utun_fd,b,n+4);
    static unsigned errs;if (w!=(ssize_t)(n+4) && errs++<20) {printf("cellular: utun write %u -> %zd (%s)\n",n+4,w,strerror(errno));fflush(stdout);}
}
static int ctl_fd=-1;static uint8_t ipc_mseq=0x40;
static int sbd_send(unsigned id,const uint8_t *frame,uint32_t n);
#define HEXOF(buf,n) do{for(size_t _k=0;_k<(n);_k++) snprintf(hex+2*_k,3,"%02x",(buf)[_k]);}while(0)
static void ctl_line(char *l)
{
    uint8_t f[2048];
    while (*l==' ') l++;
    if (!strncmp(l,"at ",3)) {
        size_t n=strlen(l+3);if (n>1000) return;
        f[0]=0xf8;f[1]=25;memcpy(f+4,l+3,n);f[4+n]='\r';n+=5;f[2]=(uint8_t)n;f[3]=(uint8_t)(n>>8);
        int e=sbd_send(5,f,(uint32_t)n);printf("cellular: AT -> %s %s\n",l+3,e?"(send failed)":"");fflush(stdout);
    } else if (!strcmp(l,"ims")) {        /* IMS PDN on cid 3, P-CSCF via PCO (d[0x7e]: 1 v4, 2 v6, 3 v4v6); never deactivated (crashes the CP) */
        char cmd[1400],hex[1100];uint8_t d[330];
        if (!ims.imsi[0]) ctl_line((char[]){"fmt 0a 02 02"});
        if (!ims.imei[0]) ctl_line((char[]){"fmt 0a 03 02 01"});
        memset(d,0,sizeof(d));d[0]=1;d[1]=IMS_CID;d[2]=6;memcpy(d+3,"ims",3);d[0x7e]=3;HEXOF(d,142);snprintf(cmd,sizeof(cmd),"fmt 0d 01 03 %s",hex);ctl_line(cmd);
        memset(d,0,sizeof(d));d[0]=1;d[1]=IMS_CID;HEXOF(d,241);snprintf(cmd,sizeof(cmd),"fmt 0d 04 03 %s",hex);ctl_line(cmd);
    } else if (!strncmp(l,"apdu ",5)) {     /* apdu HEX: raw APDU to the SIM (05 06 {u16 BE len, APDU}); answer -> /var/tmp/redwood-cell/apdu */
        char hx[600];size_t hl=0;for (const char *c=l+5;*c && hl<sizeof(hx)-1;c++) if (*c!=' ') hx[hl++]=*c;hx[hl]=0;
        if (hl<8 || hl%2 || hl>500) return;
        char cmd[700];snprintf(cmd,sizeof(cmd),"fmt 05 06 02 %02zx %02zx %s",(hl/2)>>8,(hl/2)&0xff,hx);unlink("/var/tmp/redwood-cell/apdu");ctl_line(cmd);
    } else if (!strncmp(l,"auth ",5)) {     /* auth TYPE RAND AUTN (hex): UICC authentication, answer in /var/tmp/redwood-cell/auth */
        unsigned ty=0;char r[80]={0},a[80]={0};if (sscanf(l+5,"%x %79s %79s",&ty,r,a)!=3 || strlen(r)!=32 || strlen(a)!=32) return;
        char cmd[256];snprintf(cmd,sizeof(cmd),"fmt 05 09 02 %02x 10 %s 10 %s",ty,r,a);unlink("/var/tmp/redwood-cell/auth");ctl_line(cmd);
    } else if (!strncmp(l,"pdn ",4)) {      /* pdn CID APN: define + activate one more PDN (no attach-profile change) */
        unsigned cid=0;char apn[100]={0};if (sscanf(l+4,"%u %99s",&cid,apn)!=2 || cid<3 || cid>8) return;
        char cmd[1400],hex[1100];size_t al=strlen(apn);uint8_t d[330];
        memset(d,0,sizeof(d));d[0]=1;d[1]=(uint8_t)cid;d[2]=6;memcpy(d+3,apn,al);HEXOF(d,142);snprintf(cmd,sizeof(cmd),"fmt 0d 01 03 %s",hex);ctl_line(cmd);
        memset(d,0,sizeof(d));d[0]=1;d[1]=(uint8_t)cid;HEXOF(d,241);snprintf(cmd,sizeof(cmd),"fmt 0d 04 03 %s",hex);ctl_line(cmd);
        printf("cellular: PDN cid %u requested on APN %s\n",cid,apn);fflush(stdout);
    } else if (!strncmp(l,"verbose ",8)) {  /* verbose on|off: the per-message debug lines (not kept across restarts: /var/root/cell-verbose is) */
        cell_verbose=!strcmp(l+8,"on");printf("cellular: verbose log %s\n",cell_verbose?"on":"off");fflush(stdout);
    } else if (!strcmp(l,"restart")) {      /* restart: a clean modem restart, as the watchdogs do (exec of /var/root's binary) */
        printf("cellular: restart asked on the command pipe\n");fflush(stdout);restart_wanted=1;stopped=1;
    } else if (!strcmp(l,"airplane")) {     /* (B, 2026-11-19) airplane mode on (SpringBoard, cell_sb.h): park the modem and
                                             * exit without a retry; airplane mode off starts a fresh bring-up */
        printf("cellular: airplane mode: modem off\n");fflush(stdout);unlink("/var/tmp/redwood-cell/state");stopped=1;
    } else if (!strncmp(l,"nodata",6)) {
        data_off=1;close(open("/var/root/cell-nodata",O_CREAT|O_WRONLY,0644));utun_down();cell_publish();
    } else if (!strncmp(l,"data",4) && data_off) {
        data_off=0;unlink("/var/root/cell-nodata");cell_publish();
        printf("cellular: mobile data on\n");fflush(stdout);
        if (!pdn.up) ctl_line((char[]){"data"});   /* PDN still up: utun_up() follows in the main loop */
    } else if (!strncmp(l,"data",4)) {
        /* (B, 2026-11-20) the carrier's data APN: "data <apn>" on the pipe, else /var/root/cell-apn (one line: other
         * carriers and regions, docs/devices/x1slte/cellular/provisioning.md), else Jio's */
        static char apn_file[100];
        if (!apn_file[0]) {FILE *af=fopen("/var/root/cell-apn","r");if (af) {if (fscanf(af,"%99s",apn_file)!=1) apn_file[0]=0;fclose(af);}}
        const char *apn=l[4]==' '?l+5:apn_file[0]?apn_file:"jionet";char cmd[1400],hex[1100];size_t al=strlen(apn);if (al>99) return;
        uint8_t d[330];
        memset(d,0,sizeof(d));d[0]=1;d[1]=1;d[2]=6;memcpy(d+3,apn,al);d[0x148]=6;HEXOF(d,330);snprintf(cmd,sizeof(cmd),"fmt 0d 14 03 %s",hex);ctl_line(cmd);
        ctl_line("fmt 0d 03 03 010000");
        memset(d,0,sizeof(d));d[0]=1;d[1]=2;d[2]=6;memcpy(d+3,apn,al);HEXOF(d,142);snprintf(cmd,sizeof(cmd),"fmt 0d 01 03 %s",hex);ctl_line(cmd);
        memset(d,0,sizeof(d));d[0]=1;d[1]=2;HEXOF(d,241);snprintf(cmd,sizeof(cmd),"fmt 0d 04 03 %s",hex);ctl_line(cmd);
        printf("cellular: data call requested on APN %s\n",apn);fflush(stdout);
    } else if (!strncmp(l,"uldebug",7)) {
        ul_debug=40;
    } else if (!strncmp(l,"dm",2)) {
        dm_on=!dm_on;printf("cellular: modem debug strings %s\n",dm_on?"on":"off");fflush(stdout);
    } else if (!strncmp(l,"pch ",4)) {
        unsigned c2=0,r2=ul_ring;sscanf(l+4,"%u %u",&c2,&r2);if (c2) ul_ch=c2;if (r2==8||r2==9) ul_ring=r2;
        printf("cellular: UL channel %u ring %u\n",ul_ch,ul_ring);fflush(stdout);
    } else if (!strncmp(l,"rs",2)) {
        /* ICMPv6 router solicitation from our address to ff02::2 */
        if (!pdn.up) return;
        uint8_t pk[48];memset(pk,0,sizeof(pk));pk[0]=0x60;pk[5]=8;pk[6]=58;pk[7]=255;memcpy(pk+8,pdn.ip6,16);
        pk[24]=0xff;pk[25]=0x02;pk[39]=2;pk[40]=133;
        uint8_t ph[40];memset(ph,0,sizeof(ph));memcpy(ph,pk+8,32);ph[35]=8;ph[39]=58;
        uint32_t s0=0;for (int i=0;i<40;i+=2) s0+=(ph[i]<<8)|ph[i+1];
        uint16_t c=csum16(pk+40,8,s0);pk[42]=(uint8_t)(c>>8);pk[43]=(uint8_t)c;
        printf("cellular: router solicitation %s\n",pkt_tx(pk,48)?"refused":"sent");fflush(stdout);
    } else if (!strncmp(l,"ping6 ",6)) {
        ping6(l+6);
    } else if (!strncmp(l,"stat",4)) {
        printf("cellular: rings id:ch ulrp/ulwp dlrp/dlwp:");
        for (unsigned i=0;i<SBD_N;i++) {uint16_t a1=0,a2=0,a3=0,a4=0;ipc_load16(sbd_rp(i,0),&a1);ipc_load16(sbd_wp(i,0),&a2);ipc_load16(sbd_rp(i,1),&a3);ipc_load16(sbd_wp(i,1),&a4);
            printf(" %u:%u %u/%u %u/%u",i,sbd_chs[i].ch,a1,a2,a3,a4);}
        uint32_t m=0,u=0;ipc_load(0x404,&m);ipc_load(0x40c,&u);printf(" | cp2ap %x united %x\n",m,u);fflush(stdout);
    } else if (!strncmp(l,"fmt ",4)) {
        unsigned g,i,t;int used=0;
        if (sscanf(l+4,"%x %x %x %n",&g,&i,&t,&used)<3) return;
        size_t d=0;const char *h=l+4+used;
        while (*h && d<1900) {while (*h==' ') h++;unsigned b;if (sscanf(h,"%2x",&b)!=1) break;f[11+d++]=(uint8_t)b;h+=2;}
        size_t n=11+d;f[0]=0xf8;f[1]=235;f[2]=(uint8_t)n;f[3]=(uint8_t)(n>>8);
        f[4]=(uint8_t)(n-4);f[5]=(uint8_t)((n-4)>>8);f[6]=ipc_mseq++;f[7]=0;f[8]=(uint8_t)g;f[9]=(uint8_t)i;f[10]=(uint8_t)t;
        int e=sbd_send(1,f,(uint32_t)n);if (e || verbose()) printf("cellular: IPC -> %02x %02x %02x (%zu data bytes) %s\n",g,i,t,d,e?"(send failed)":"");fflush(stdout);
    }
}
/* (B, 2026-11-19) iOS airplane mode (SystemConfiguration's radios plist). SpringBoard (cell_sb.h) also sends
 * "airplane" on the pipe, but only to a service already reading it: this covers a boot with airplane mode on and a
 * switch to on during the ~30 s bring-up. The plist is parsed only when its time stamp changes. */
static int airplane_on(void)
{
    static struct timespec seen;static int last;struct stat st;
    const char *path="/var/preferences/SystemConfiguration/com.apple.radios.plist";
    if (stat(path,&st)) return 0;
    if (st.st_mtimespec.tv_sec==seen.tv_sec && st.st_mtimespec.tv_nsec==seen.tv_nsec) return last;
    seen=st.st_mtimespec;last=0;
    CFURLRef url=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8 *)path,(CFIndex)strlen(path),false);
    CFReadStreamRef s=url?CFReadStreamCreateWithFile(NULL,url):NULL;
    if (s && CFReadStreamOpen(s)) {
        CFPropertyListRef p=CFPropertyListCreateWithStream(NULL,s,0,kCFPropertyListImmutable,NULL,NULL);
        if (p && CFGetTypeID(p)==CFDictionaryGetTypeID()) {
            CFTypeRef v=CFDictionaryGetValue((CFDictionaryRef)p,CFSTR("AirplaneMode"));
            last=v && CFGetTypeID(v)==CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)v);
        }
        if (p) CFRelease(p);
        CFReadStreamClose(s);
    }
    if (s) CFRelease(s);
    if (url) CFRelease(url);
    return last;
}
static void ctl_poll(void)
{
    static char buf[4096];static size_t have;
    if (ctl_fd<0) {
        mkdir("/var/tmp/redwood-cell",0755);unlink("/var/tmp/redwood-cell/cmd");
        if (mkfifo("/var/tmp/redwood-cell/cmd",0622)) return;
        chmod("/var/tmp/redwood-cell/cmd",0622);
        ctl_fd=open("/var/tmp/redwood-cell/cmd",O_RDWR|O_NONBLOCK);if (ctl_fd<0) return;
    }
    ssize_t r=read(ctl_fd,buf+have,sizeof(buf)-1-have);if (r<=0) return;have+=(size_t)r;buf[have]=0;
    char *nl;
    while ((nl=strchr(buf,'\n'))) {*nl=0;ctl_line(buf);size_t rest=have-(size_t)(nl+1-buf);memmove(buf,nl+1,rest);have=rest;buf[have]=0;}
    if (have>=sizeof(buf)-1) have=0;
}
static int online_handshake(unsigned seconds)
{
    uint32_t last=0xffffffff,lastu=0xffffffff,msi=0,lastmsi=0xffffffff;int online=0;
    uint64_t deadline=now_us(NULL)+(uint64_t)seconds*1000000;
    while (now_us(NULL)<deadline && !stopped) {
        uint32_t m=0,u=0,in=0;
        if (ipc_load(0x404,&m) || ipc_load(0x40c,&u)) return -1;
        read_word(0x13800830,&msi);inputs(&in);
        if (m!=last || u!=lastu || msi!=lastmsi) {
            if (!stay_mode || m!=last) {printf("cellular live DL: cp2ap_msg %08x cp2ap_united %08x MSI status %08x GPIO %x\n",m,u,msi,in);fflush(stdout);}
            if (msi) {char r[8192],c[64];snprintf(c,sizeof(c),"w 0x13800830 0x%x",msi);request(c,r);}
            static unsigned repeats;
            if (m==last && msi && (m&0xff)==0xc8 && online && repeats<5) {
                repeats++;printf("cellular live DL: PHONE_START repeated (MSI) -> INIT_END again (%u)\n",repeats);ap2cp(0x02);
            }
            if (online && !stay_mode) {uint16_t urp=0,uwp=0;ipc_load16(sbd_rp(1,0),&urp);ipc_load16(sbd_wp(1,0),&uwp);
                printf("cellular live DL: umts_ipc0 UL rp %u wp %u (rp moving = CP read our request)\n",urp,uwp);fflush(stdout);}
            if (m!=last && (m&0x80) && (m&0x40)) {
                unsigned cmd=m&0x3f;
                if (cmd==0x01) {phase("CP: INIT_START -> SBD rings, PIF_INIT_DONE");if (pktproc_info() || sbd_init()) return -1;ipc_store(0x408,(2u<<14)|(1u<<1)|(1u<<5));ap2cp(0x0d);}
                else if (cmd==0x08) {
                    phase("CP: PHONE_START -> IPC link (magic AA, queues zero) and INIT_END");
                    for (uint32_t o=8;o<0x28;o+=4) ipc_store(o,0);
                    ipc_store(4,1);ipc_store(0,0xaa);ap2cp(0x02);online=1;unsetenv("REDWOOD_CELL_TRY");setpriority(PRIO_PROCESS,0,0);   /* boot priority no longer needed */
                }
                else if (cmd==0x07 || cmd==0x09) phase("CP: CRASH");
            }
            last=m;lastu=u;lastmsi=msi;
        }
        if (online && stay_mode) ctl_poll();
        if (stay_mode && !stopped) {   /* airplane mode switched on while no one sent "airplane" (bring-up window) */
            static uint64_t looked;uint64_t t=now_us(NULL);
            if (t-looked>3000000ull) {looked=t;
                if (airplane_on()) {printf("cellular: airplane mode on: modem off\n");fflush(stdout);unlink("/var/tmp/redwood-cell/state");stopped=1;}
            }
        }
        if (online && stay_mode && pdn.up) {if (!data_off) utun_up();pp_poll();utun_poll();}
        if (online && stay_mode && ims.up) {ims_up();ims_poll();}
        if (online && stay_mode && ims_fd>=0) {
            /* the VoLTE/SMS client (redwood-ims, /var/root): started once IMS is up, started again when it leaves
             * (fresh registration every ~110 min, or a lost connection), at most every 30 s. /var/root/cell-noimsclient: never */
            static pid_t ims_pid;static uint64_t ims_spawned,ims_gap=30000000ull;struct stat st2;
            if (ims_pid>0 && waitpid(ims_pid,NULL,WNOHANG)==ims_pid) {
                /* (A, 2026-11-14) a long, healthy run that ended (a dropped registration connection, the ~110 min
                 * refresh): start again after 2 s, not 30 -- no VoLTE meanwhile; quick failures stay 30 s apart */
                ims_gap=now_us(NULL)-ims_spawned>120000000ull?2000000ull:30000000ull;
                printf("cellular: IMS client left (again in %llu s)\n",ims_gap/1000000);fflush(stdout);ims_pid=0;ims_spawned=now_us(NULL);
            }
            if (!ims_pid && now_us(NULL)-ims_spawned>ims_gap) {
                /* a client already running (e.g. started before this process was exec'd again by a watchdog): keep it */
                FILE *q=fopen("/var/tmp/redwood-cell/ims.pid","r");int other=0;
                if (q) {if (fscanf(q,"%d",&other)!=1) other=0;fclose(q);}
                /* (only if that pid IS redwood-ims: /var/tmp survives a restart, and after one the old pid was
                 *  installd's -- the client was never started again, no VoLTE after the 18:00 boot, 2026-11-13) */
                if (other>1 && !kill(other,0)) {
                    extern int proc_pidpath(int,void *,uint32_t);char path[4096];
                    if (proc_pidpath(other,path,sizeof(path))>0 && strstr(path,"redwood-ims")) {ims_spawned=now_us(NULL);other=-1;}
                    else unlink("/var/tmp/redwood-cell/ims.pid");
                } else if (other>1) unlink("/var/tmp/redwood-cell/ims.pid");
                if (other==-1) goto ims_done;
            }
            if (!ims_pid && now_us(NULL)-ims_spawned>ims_gap && !stat("/var/root/redwood-ims",&st2) && stat("/var/root/cell-noimsclient",&st2)) {
                posix_spawn_file_actions_t fa;posix_spawn_file_actions_init(&fa);
                posix_spawn_file_actions_addopen(&fa,1,"/var/tmp/redwood-ims.log",O_WRONLY|O_CREAT|O_APPEND,0600);
                posix_spawn_file_actions_adddup2(&fa,1,2);posix_spawn_file_actions_addopen(&fa,0,"/dev/null",O_RDONLY,0);
                char *av[]={"/var/root/redwood-ims",NULL};
                if (!posix_spawn(&ims_pid,av[0],&fa,NULL,av,environ)) {printf("cellular: IMS client started (pid %d)\n",ims_pid);fflush(stdout);}
                else ims_pid=0;
                posix_spawn_file_actions_destroy(&fa);ims_spawned=now_us(NULL);
            }
            if (stopped && ims_pid>0) {kill(ims_pid,SIGTERM);ims_pid=0;}
            ims_done:;
        }
        if (online && stay_mode) {
            /* registration watchdog: after a boot the CP has been seen to stay at "no service" (PLMN 000000, no RSSI
             * reports) until restarted; registered within 2 minutes of coming online, or a clean restart */
            static uint64_t online_at;uint64_t now=now_us(NULL);
            if (!online_at) online_at=now;
            int registered=cell.plmn[0] && strcmp(cell.plmn,"000000");
            if (registered) online_at=~0ull>>1;        /* registered once: this watchdog is done */
            else if (online_at!=(~0ull>>1) && now-online_at>75000000ull) {
                printf("cellular: watchdog: not registered 75 s after the modem came up; restarting the modem\n");fflush(stdout);
                restart_wanted=1;stopped=1;
            }
        }
        if (online && stay_mode && pdn.up && !data_off && utun_fd>=0) {
            /* downlink watchdog: the CP has been seen to stop delivering anything (ring idle, control IPC fine).
             * Traffic going out and nothing back for 60 s -> ping the PDN's DNS; still nothing 20 s later -> restart */
            static uint64_t last_rx,last_tx,probe;static uint64_t prev_rx,prev_tx,rx_at_probe;uint64_t now=now_us(NULL);
            if (!last_rx) last_rx=now;
            if (pp_rx!=prev_rx) {prev_rx=pp_rx;last_rx=now;probe=0;}
            if (pp_tx!=prev_tx) {prev_tx=pp_tx;last_tx=now;}
            {   /* health, once a minute, with the wall clock: enough to line a stall up with signal and the ring */
                static uint64_t said;
                if (now-said>60000000ull) {said=now;time_t t=time(NULL);struct tm tm;localtime_r(&t,&tm);
                    struct stat ls;if (!fstat(1,&ls) && S_ISREG(ls.st_mode) && ls.st_size>8*1024*1024 && !ftruncate(1,0)) {
                        lseek(1,0,SEEK_SET);printf("cellular: log cut back at 8 MB\n");}
                    printf("cellular: health %02d:%02d:%02d rx %llu tx %llu ring rear %u done %u fore %u dbm %d bars %d ims %s (in %llu, sbd %llu)\n",
                        tm.tm_hour,tm.tm_min,tm.tm_sec,(unsigned long long)pp_rx,(unsigned long long)pp_tx,pp_last_rear,pp_done,pp_fore,
                        cell.rssi_dbm,cell.bars,ims.up?"up":"down",ims_dl_n,ims_dl_sbd);fflush(stdout);}
            }
            if (!probe && last_tx>last_rx && now-last_rx>60000000ull) {
                char d[64];inet_ntop(AF_INET6,pdn.dns6,d,sizeof(d));
                printf("cellular: watchdog: nothing received for %llu s while sending; pinging %s\n",(unsigned long long)((now-last_rx)/1000000),d);fflush(stdout);
                ping6(d);probe=now;rx_at_probe=pp_rx;
            }
            if (probe && now-probe>20000000ull && pp_rx==rx_at_probe) {
                /* pp_rx counts every downlink packet, IMS included: silent means nothing at all came back. The internet
                 * can be blocked while the modem is fine (an expired data pack): at most one restart per 15 minutes */
                struct stat ws;time_t last=stat("/var/tmp/redwood-cell/watchdog-restart",&ws)?0:ws.st_mtime;
                if (time(NULL)-last<900) {
                    if (probe!=1) {printf("cellular: watchdog: downlink silent, but a restart was made less than 15 min ago; waiting\n");fflush(stdout);}
                    probe=1;
                } else {
                    printf("cellular: watchdog: downlink silent, restarting the modem\n");fflush(stdout);
                    close(open("/var/tmp/redwood-cell/watchdog-restart",O_CREAT|O_WRONLY|O_TRUNC,0644));utimes("/var/tmp/redwood-cell/watchdog-restart",NULL);
                    restart_wanted=1;stopped=1;
                }
            }
        }
        if (online && stay_mode && pdn.up && !ims.up && ims.tries<6 && now_us(NULL)-ims.tried>20000000ull) {
            struct stat ni;
            if (stat("/var/root/cell-noims",&ni)) {ims.tried=now_us(NULL);ims.tries++;printf("cellular: IMS PDN, try %d\n",ims.tries);fflush(stdout);ctl_line((char[]){"ims"});}
        }
        if (online && stay_mode && !data_off && !pdn.up && cell.plmn[0] && strcmp(cell.plmn,"000000")) {
            static uint64_t tried;static unsigned tries;struct stat nd;
            if (tries<12 && now_us(NULL)-tried>8000000ull && stat("/var/root/cell-nodata",&nd)) {
                tried=now_us(NULL);tries++;printf("cellular: automatic data call, try %u\n",tries);fflush(stdout);ctl_line((char[]){"data"});
            }
        }
        if (online && stay_mode) {      /* heartbeat: SpringBoard treats a state older than two minutes as no modem */
            static uint64_t beat;
            if (now_us(NULL)-beat>20000000ull) {beat=now_us(NULL);if (cell.plmn[0] || cell.have_rssi) cell_publish();}
        }
        if (online) {
            static int asked;static uint64_t shown;
            static int scanned;
            if (!stay_mode && !scanned && now_us(NULL)+8000000ull>deadline) {
                scanned=1;uint8_t c[2048];uint32_t start=0,prev=0;int in_run=0;
                printf("cellular live DL: scan IPC 0x10000..0x800000 for CP writes:\n");
                for (uint32_t o=0x10000;o<0x800000;o+=sizeof(c)) {
                    int nz=0;if (ipc_read(o,c,sizeof(c))) break;
                    for (unsigned k=0;k<sizeof(c);k++) if (c[k]) {nz=1;break;}
                    if (nz && !in_run) {start=o;in_run=1;
                        printf("   %06x:",o);for (unsigned k=0;k<32;k++) printf(" %02x",c[k]);printf("\n");}
                    if (!nz && in_run) {printf("   ... nonzero %06x..%06x\n",start,o);in_run=0;}
                    prev=o;
                }
                if (in_run) printf("   ... nonzero %06x..%06x\n",start,prev+(uint32_t)sizeof(c));
                fflush(stdout);
            }
            static int rawn;
            if (!stay_mode && rawn<3 && now_us(NULL)-shown>1000000) {
                uint8_t r[256];rawn++;
                if (!ipc_read(0xf00,r,sizeof(r))) {printf("cellular live DL: raw IPC+f00:");for (unsigned k=0;k<sizeof(r);k++) printf("%s%02x",k%32?" ":"\n   ",r[k]);printf("\n");}
                if (!ipc_read(0x400,r,32)) {printf("cellular live DL: raw IPC+400:");for (unsigned k=0;k<32;k++) printf(" %02x",r[k]);printf("\n");}
            }
            if (!stay_mode && now_us(NULL)-shown>1000000) {
                shown=now_us(NULL);uint8_t a[2*2*2*SBD_N];
                if (!ipc_read(sbd_rpwp,a,sizeof(a))) {
                    printf("cellular live DL: rpwp ulrp|ulwp|dlrp|dlwp:");
                    for (unsigned k=0;k<sizeof(a)/2;k++) printf("%s%u",k%SBD_N?",":" | ",a[2*k]|(a[2*k+1]<<8));
                    printf("\n");fflush(stdout);
                }
            }
            sbd_dump_dl();
            if (!asked && (stay_mode || now_us(NULL)+35000000ull<deadline)) {
                /* Samsung IPC on umts_ipc0 (ch 235, SBD id 1): IPC_MISC_ME_VERSION GET (0a 01, type 02), data ff */
                uint8_t f[16]={0xf8,235,0,0, 8,0, 1,0, 0x0a,0x01,0x02, 0xff};
                f[2]=12;asked=1;
                int e=sbd_send(1,f,12);
                printf("cellular live DL: IPC MISC_ME_VERSION GET on umts_ipc0: %s\n",e?"send failed":"sent");fflush(stdout);
            }
            static int asked_sim;
            if (!stay_mode && asked && !asked_sim && now_us(NULL)+25000000ull<deadline) {
                /* IPC_SEC_SIM_STATUS (05 01) GET (02), no data */
                uint8_t f[16]={0xf8,235,11,0, 7,0, 2,0, 0x05,0x01,0x02};asked_sim=1;
                int e=sbd_send(1,f,11);
                printf("cellular live DL: IPC SEC_SIM_STATUS GET: %s\n",e?"send failed":"sent");fflush(stdout);
            }
            static int legacy;
            if (0 && asked && !legacy && now_us(NULL)+12000000ull<deadline) {
                /* the same request on the legacy FMT queue: TX buf 0x1000 (4 KB), head 0x8 tail 0xc; SEND_FMT 0x82 */
                uint8_t f[12]={0xf8,235,12,0, 8,0, 2,0, 0x0a,0x01,0x02, 0xff};uint32_t h=0,t=0;legacy=1;
                ipc_load(0x8,&h);ipc_load(0xc,&t);
                if (h<0x1000 && h+12<=0x1000) {ipc_write(0x1000+h,f,12);__sync_synchronize();ipc_store(0x8,h+12);}
                ipc_store(0x400,0x82);doorbell(0);
                printf("cellular live DL: same request on legacy FMT (head %x tail %x -> %x)\n",h,t,h+12);fflush(stdout);
            }
            if (legacy) {
                static uint64_t last;
                if (now_us(NULL)-last>1000000) {
                    last=now_us(NULL);uint32_t th=0,tt=0,rh=0,rt=0;ipc_load(0x8,&th);ipc_load(0xc,&tt);ipc_load(0x10,&rh);ipc_load(0x14,&rt);
                    printf("cellular live DL: legacy FMT TX head %x tail %x, RX head %x tail %x\n",th,tt,rh,rt);fflush(stdout);
                }
                fmt_dump();
            }
        }
        usleep(1000);
    }
    {
        uint8_t r[0x800];
        if (!ipc_read(0,r,sizeof(r))) {
            printf("cellular live DL: IPC 0..800 at the end (nonzero 32-byte rows):\n");
            for (unsigned o=0;o<sizeof(r);o+=32) {int nz=0;for (unsigned k=0;k<32;k++) nz|=r[o+k];
                if (nz) {printf("   %04x:",o);for (unsigned k=0;k<32;k++) printf(" %02x",r[o+k]);printf("\n");}}
        }
    }
    printf("cellular live DL: handshake %s\n",online?"ONLINE (INIT_END sent)":"no PHONE_START seen");fflush(stdout);
    return online?0:-1;
}

/* --download-all PLAN: lines "STAGE FILE OFFSET SIZE CRC" (CRC 0 = no CRC step); "15" = FIN (START only) */
static const char *download_plan;
static int raw_send_wait(const uint8_t *frame,uint32_t n,unsigned ms)
{
    uint64_t deadline=now_us(NULL)+(uint64_t)ms*1000;
    for (;;) {
        int e=raw_send(frame,n);if (e!=-2) return e;
        if (now_us(NULL)>deadline || stopped) return -2;
        usleep(500);
    }
}
static int udl_ack(unsigned stage,enum s5123_udl_op op,unsigned ms)
{
    uint8_t reply[64];int got=raw_recv(reply,sizeof(reply),ms);
    if (got<=0) {printf("cellular live DL: stage %u op %u: no reply in %u ms\n",stage,op,ms);fflush(stdout);return -1;}
    enum s5123_udl_ack a=s5123_udl_response(reply,(size_t)((got+7)&~7),stage,op);
    if (a!=S5123_UDL_ACK) {
        printf("cellular live DL: stage %u op %u: reply",stage,op);for (int i=0;i<got && i<16;i++) printf(" %02x",reply[i]);
        printf(" -> %s\n",a==S5123_UDL_PEER_FAIL?"CP reports FAIL":"unexpected");fflush(stdout);return -1;
    }
    return 0;
}
static int udl_cmd(unsigned stage,enum s5123_udl_op op,uint32_t crc,unsigned ms)
{
    uint8_t frame[64];size_t n=s5123_udl_command(frame,sizeof(frame),stage,op,crc);
    if (!n || raw_send_wait(frame,(uint32_t)n,ms)) return -1;
    return udl_ack(stage,op,ms);
}
static int udl_image(unsigned stage,const char *path,long offset,uint32_t size)
{
    FILE *f=fopen(path,"rb");if (!f || fseek(f,offset,SEEK_SET)) {if (f) fclose(f);return -1;}
    uint32_t frames=size/S5123_UDL_MSS+(size%S5123_UDL_MSS!=0);uint8_t chunk[S5123_UDL_MSS],frame[S5123_UDL_WIRE_MAX];
    uint64_t start=now_us(NULL);
    for (uint32_t i=1;i<=frames;i++) {
        size_t want=size-(i-1)*S5123_UDL_MSS;if (want>S5123_UDL_MSS) want=S5123_UDL_MSS;
        if (fread(chunk,1,want,f)!=want) {fclose(f);return -1;}
        size_t n=s5123_udl_data(frame,sizeof(frame),stage,size,i,chunk,want);
        if (!n || raw_send_wait(frame,(uint32_t)n,5000)) {
            printf("cellular live DL: stage %u: frame %u/%u not taken (ring full 5 s or I/O)\n",stage,i,frames);fflush(stdout);fclose(f);return -1;
        }
    }
    fclose(f);
    printf("cellular live DL: stage %u: %u frames (%u bytes) queued in %llu ms\n",stage,frames,size,
        (unsigned long long)((now_us(NULL)-start)/1000));fflush(stdout);
    return udl_ack(stage,S5123_UDL_SEND,30000);
}
static int download_stages(void)
{
    FILE *p=fopen(download_plan,"r");if (!p) return -1;
    char line[512];int error=0;
    while (!error && fgets(line,sizeof(line),p)) {
        unsigned stage;char path[400];long offset;unsigned long size,crc;
        if (sscanf(line,"%u",&stage)!=1) continue;
        if (stage==15) {
            error=udl_cmd(15,S5123_UDL_START,0,10000);
            printf("cellular live DL: FIN START %s\n",error?"failed":"ACK: all stages loaded");fflush(stdout);
            break;
        }
        if (sscanf(line,"%u %399s %ld %lu %lx",&stage,path,&offset,&size,&crc)!=5) {error=-1;break;}
        printf("cellular live DL: stage %u: %lu bytes from %s+%ld%s\n",stage,size,path,offset,crc?" (with CRC)":"");fflush(stdout);
        error=udl_cmd(stage,S5123_UDL_START,0,5000) || udl_image(stage,path,offset,(uint32_t)size) ||
            (crc && udl_cmd(stage,S5123_UDL_CRC,(uint32_t)crc,10000)) || udl_cmd(stage,S5123_UDL_DONE,0,10000);
        printf("cellular live DL: stage %u %s\n",stage,error?"FAILED":"done (START, SEND, DONE acknowledged)");fflush(stdout);
    }
    fclose(p);
    if (!error) {
        phase("download complete: Samsung's IPC handshake, 20 s");
        error=online_handshake(stay_mode?(unsigned)(10*365*24*3600):40);
        for (unsigned i=0;i<2;i++) {
            uint32_t in=0,w[10];inputs(&in);for (unsigned k=0;k<10;k++) ipc_load(k*4,&w[k]);
            printf("cellular live DL: +%ums GPIO %x IPC",i*500,in);for (unsigned k=0;k<10;k++) printf(" %08x",w[k]);printf("\n");fflush(stdout);
            usleep(500000);
        }
    }
    return error;
}
static int download_run(void)
{
    char response[8192];
    phase("download: S2MPU grant (HSI2) for the CP shared memory");
    if (request("cp.s2mpu",response)) return -1;
    printf("cellular live DL: %s",response);fflush(stdout);
    if (!strstr(response,"ret 0, d0000000+4000000 ret 0") && !strstr(response,"ret 0x0, d0000000+4000000 ret 0x0")) {
        fprintf(stderr,"cellular live DL: hypervisor refused a grant; no DMA enable\n");return -1;
    }
    phase("download: root port + endpoint memory/bus master on (CP may now read/write its shared memory)");
    if (request("m 0x13800004 0xfffffff9 0x6",response)) return -1;
    printf("cellular live DL: root %s",response);
    /* s51xx_pcie_probe: BAR0 = the board's pci_db_addr (DT 0x14d60000), then memory + bus master */
    if (request("w 0x14ffe010 0x14d60000",response)) return -1;
    printf("cellular live DL: endpoint BAR0 %s",response);
    if (request("m 0x14ffe004 0xfff9 0x6",response)) return -1;
    printf("cellular live DL: endpoint %s",response);fflush(stdout);
    /* exynos_pcie_rc_msi_init for EP_SAMSUNG_MODEM: RC MSI target = shm MSI base (f6200000), INTR0 0xf1;
     * then the endpoint's MSI capability (0x50) to the same address, enabled (pci_enable_msi) */
    {
        uint32_t msi=0;
        if (request("w 0x13800820 0xf6200000",response) || request("w 0x13800824 0x0",response) ||
            request("w 0x13800828 0xf1",response)) return -1;
        if (!read_word(0x14ffe050,&msi)) {
            printf("cellular live DL: EP MSI control %08x (%s-bit)\n",msi,(msi&(1u<<23))?"64":"32");
            request("w 0x14ffe054 0xf6200000",response);
            if (msi&(1u<<23)) {request("w 0x14ffe058 0x0",response);request("w 0x14ffe05c 0x0",response);}
            else request("w 0x14ffe058 0x0",response);
            request("m 0x14ffe050 0xff8effff 0x10000",response);   /* enable, one vector */
            read_word(0x14ffe050,&msi);printf("cellular live DL: EP MSI control now %08x\n",msi);fflush(stdout);
        }
    }
    usleep(200000);
    {
        uint32_t w[10];for (unsigned i=0;i<10;i++) ipc_load(i*4,&w[i]);
        printf("cellular live DL: IPC header before:");for (unsigned i=0;i<10;i++) printf(" %08x",w[i]);printf("\n");fflush(stdout);
    }
    uint8_t frame[S5123_UDL_WIRE_MAX];size_t n;int error=-1;
    n=s5123_udl_command(frame,sizeof(frame),1,S5123_UDL_START,0);
    if (n && !udl_step(1,S5123_UDL_START,frame,n)) {
        uint8_t toc[800];size_t length=0;FILE *f=fopen(download_toc,"rb");
        if (f) {length=fread(toc,1,sizeof(toc),f);fclose(f);}
        if (length==800 && (n=s5123_udl_data(frame,sizeof(frame),1,800,1,toc,800)) && !udl_step(1,S5123_UDL_SEND,frame,n) &&
            (n=s5123_udl_command(frame,sizeof(frame),1,S5123_UDL_DONE,0)) && !udl_step(1,S5123_UDL_DONE,frame,n)) {
            phase("download: TOC stage complete (START, SEND, DONE acknowledged)");error=0;
            if (download_plan) error=download_stages();
        }
    }
    request("m 0x14ffe004 0xfffb 0x0",response);request("m 0x13800004 0xfffffffb 0x0",response);
    phase("download: bus master off again on both ends");
    return error;
}
static int send_boot(void *context,const uint8_t *data,size_t length,uint32_t *completed)
{
    struct modem *m=context;struct s5123_spi_result result={0};
    struct s5123_spi_ops ops={&m->spi,spi_read32,spi_read8,spi_write32,spi_write8,now_us};
    phase("send BOOT only; byte PIO, no DMA");uint64_t start=now_us(NULL);
    int error=s5123_spi_send_boot(&ops,&result,data,length,1625000,5000000,1);
    *completed=result.completed_bytes;
    printf("cellular live BOOT: PIO error %d io_error %d completed %u received %u packets %u elapsed_us %llu\n",
        error,m->spi.io_error,*completed,m->spi.received,result.packets,(unsigned long long)(now_us(NULL)-start));fflush(stdout);
    if (error || m->spi.io_error || *completed!=length) return -1;
    if (download_toc && ipc_boot_init()) return -1;
    phase("observe CP input transitions for 4000ms; levels do not prove acceptance");
    uint32_t first,previous,changed=0,rising=0;
    if (inputs(&first)) return -1;previous=first;
    for (unsigned i=0;i<200;i++) {
        uint32_t current;if (delay(context,20000) || inputs(&current)) return -1;
        changed|=previous^current;rising|=~previous&current;previous=current;
    }
    printf("cellular live BOOT: GPIO first %x last %x changed %x rising %x (PHONE_ACTIVE,WAKE,PS_HOLD)\n",
        first,previous,changed,rising);fflush(stdout);
    if (link_requested) {
        if (output(context,S5123_AP_WAKE,1) || delay(context,5000)) return -1;
        int link_error=s5123_link_begin(&m->link);
        printf("cellular live BOOT: PCIe root command %08x header %08x endpoint class %08x header %08x\n",
            m->link.root_command,m->link.root_header,m->link.endpoint_class,m->link.endpoint_header);fflush(stdout);
        printf("cellular live BOOT: PCIe error %d link_state %08x endpoint %08x command %08x (endpoint proof requires L0 and valid ID)\n",
            link_error,m->link.link_status,m->link.endpoint_id,m->link.endpoint_command);fflush(stdout);
        if (link_error) return -1;
        if (download_toc) {
            int dl=download_run();
            printf("cellular live DL: result %d\n",dl);fflush(stdout);
        }
    }
    return 0;
}
static int park(void *context)
{
    struct modem *m=context;cleanup=1;phase("park SPI, then hold reset and power low");
    if (!m->controller_saved) return 0;
    int error=0;
    if (link_requested && s5123_link_park(&m->link)) error=1;
    if (restore_word(0x10960000,0,1)) error=1;
    if (restore_word(0x10960020,0,1)) error=1;
    if (restore_word(0x1096000c,1,1)) error=1;
    return error?-1:0;
}
static int restore(void *context)
{
    struct modem *m=context;cleanup=1;int error=0;phase("restore SPI/clock/pad/AP ownership");
    if (ipc_mapped) {ipc_op(CP_IPC_UNMAP,0,NULL,0,NULL);ipc_mapped=0;}
    if (link_requested && s5123_link_restore(&m->link)) error=1;
    if (m->controller_saved) {
        if (restore_word(0x10960000,0,1)) error=1;
        if (restore_word(0x10960020,0,1)) error=1;
        if (restore_word(0x10960000,0x20,0)) error=1;
        if (restore_word(0x10960000,0,1)) error=1;
        uint32_t status;if (read_word(0x10960014,&status) || (status&((0x7fu<<6)|(0x7fu<<15)))) error=1;
        for (unsigned i=1;i<8;i++) if (restore_word(0x10960000+controller_regs[i],m->saved[i],1)) error=1;
        if (restore_word(0x10960000,m->saved[0],1)) error=1;
    }
    if (m->pads_changed || m->changed[S5123_DUMP_NOTI]) {
        if (restore_mask(0x10730060,0xffff0000u,m->pins)) error=1;
        if (restore_mask(0x10730068,0xffff0000u,m->pulls)) error=1;
        if (masked_write(0x10730064,0xf0u,m->pad_dat&0xf0u,0)) error=1;
    }
    for (unsigned i=S5123_AP_STATUS;i<=S5123_AP_WAKE;i++) if (m->changed[i]) {
        unsigned pin=outputs[i].pin;
        if (restore_mask(outputs[i].base,15u<<(4*pin),m->con[i])) error=1;
        if (masked_write(outputs[i].base+4,1u<<pin,m->dat[i]&(1u<<pin),0)) error=1;
    }
    if (m->mode_changed && restore_word(0x10721048,m->mode,1)) error=1;
    if (m->clock_changed) {
        if (restore_word(0x10701820,m->divider,0)) error=1;
        if (!clock_wait(0x10701820,(1u<<16)|15,m->divider&15)) error=1;
        if (restore_word(0x10700690,m->mux,0)) error=1;
        if (!clock_wait(0x10700690,(1u<<16)|16,m->mux&16)) error=1;
    }
    return error?-1:0;
}
static int quiescent(void *context)
{
    (void)context;uint32_t iso,reset_con,reset_dat,power_con,power_dat;
    return !read_word(0x15860720,&iso) && !(iso&1) &&
        !read_word(0x15c30120,&reset_con) && (reset_con&15)==1 &&
        !read_word(0x15c30124,&reset_dat) && !(reset_dat&1) &&
        !read_word(0x15c30280,&power_con) && (power_con&15)==1 &&
        !read_word(0x15c30284,&power_dat) && !(power_dat&1);
}
#ifndef LIVE_MODEM_TEST
static int read_file(const char *path,uint8_t *data,size_t capacity,size_t *length)
{
    FILE *file=fopen(path,"rb");if (!file) return -1;
    *length=fread(data,1,capacity,file);int error=ferror(file)||!feof(file);fclose(file);return error?-1:0;
}
int main(int argc,char **argv)
{
    saved_argv=argv;
    if ((argc<4 || argc>8) || strcmp(argv[1],"--reserved-base-verified")) return 2;
    if (argc==5) {if (strcmp(argv[4],"--link")) return 2;link_requested=1;}
    if (argc==6) {if (strcmp(argv[4],"--download")) return 2;link_requested=1;download_toc=argv[5];}
    if (argc==8 && !strcmp(argv[7],"--stay")) {
        char args[1024]="";size_t al=sizeof(args)-1;
        if (!sysctlbyname("kern.bootargs",args,&al,NULL,0) && strstr(args,"cellular=0")) {puts("cellular=0: modem left off");return 0;}
        if (airplane_on()) {unlink("/var/tmp/redwood-cell/state");puts("cellular: airplane mode on: modem left off");return 0;}
        stay_mode=1;{struct stat nd;data_off=!stat("/var/root/cell-nodata",&nd);}argc=7;
        {   /* (A, 2026-11-14) the call screen's app: LaunchServices forgets /var/jb's apps at every boot, and CallKit
             * refuses a provider whose app it cannot find ("bundleIdentifier: (null)"): register it again, in the background */
            struct stat ap;
            if (!stat("/var/jb/Applications/RedwoodPhone.app",&ap) && fork()==0) {
                sleep(20);execl("/var/jb/usr/bin/uicache","uicache","-p","/var/jb/Applications/RedwoodPhone.app",(char *)NULL);_exit(1);
            }
        }
        /* a newer copy in /var/root (root only) runs instead of the image's: updates without a new image */
        if (strcmp(argv[0],"/var/root/redwood-cellulard") && access("/var/root/redwood-cellulard",X_OK)==0) {
            argv[0]="/var/root/redwood-cellulard";execv(argv[0],argv);
        }
        unlink("/var/tmp/redwood-cell/state");    /* (/var/tmp survives a restart: no stale bars before the modem is up) */
        unlink("/var/tmp/redwood-cell/ims-registered");   /* VoLTE status for Settings: redwood-ims sets it again */
        /* right after boot the phone is busy and the SPI BOOT's byte PIO can starve (the CP's ROM times out).
         * Instead of waiting minutes for calm: the boot runs at top priority (dropped again once the CP is up),
         * starts 25 s after boot (or when the load is low), and is retried (5 attempts) */
        setpriority(PRIO_PROCESS,0,-20);
        for (unsigned waited=0;waited<60;waited+=5) {
            struct timeval boot;size_t bl=sizeof(boot);double load[3]={99,99,99};
            int up=!sysctlbyname("kern.boottime",&boot,&bl,NULL,0)?(int)(time(NULL)-boot.tv_sec):999;
            getloadavg(load,3);
            if (up>=25 || load[0]<8.0) break;
            if (waited==0) {printf("cellular: waiting for the system to settle (up %d s, load %.0f)\n",up,load[0]);fflush(stdout);}
            sleep(5);
        }
    }
    if (stay_mode) {   /* (B, 2026-11-20) a clean data volume has no modem files: make them from the phone's own firmware */
        static const char *need[]={"cell-boot.bin","cell-boot.info","cell-toc.bin","cell-plan.txt","cell-radio.img","cell-nv.bin"};
        int missing=0;struct stat st;char p[64];
        for (unsigned i=0;i<sizeof(need)/sizeof(need[0]);i++) {snprintf(p,sizeof(p),"/var/root/%s",need[i]);if (stat(p,&st) || !st.st_size) missing=1;}
        if (missing && !access("/redwood/bin/cell-provision",X_OK)) {
            pid_t pid;int status=0;char *cargv[]={"cell-provision",NULL};
            printf("cellular: modem files missing: cell-provision makes them from RADIO and EFS (read-only)\n");fflush(stdout);
            if (!posix_spawn(&pid,"/redwood/bin/cell-provision",NULL,NULL,cargv,environ)) waitpid(pid,&status,0);
            printf("cellular: cell-provision exit %d\n",WIFEXITED(status)?WEXITSTATUS(status):-1);fflush(stdout);
        }
    }
    if (argc==7) {if (strcmp(argv[4],"--download-all")) return 2;link_requested=1;download_toc=argv[5];download_plan=argv[6];}
    uint8_t image[131073],info[25];size_t length,info_length;uint32_t fields[5];
    if (read_file(argv[2],image,sizeof(image),&length) || read_file(argv[3],info,sizeof(info),&info_length) ||
        info_length!=24 || memcmp(info,"S512",4)) return 2;
    memcpy(fields,info+4,sizeof(fields));
    if (fields[0]!=1 || fields[1]!=length || fields[3]!=0x40000000 || fields[4]!=1) return 2;
    signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);signal(SIGHUP,stop_signal);if (stay_mode) signal(SIGHUP,SIG_IGN);
    char response[8192];
    if (request("spi8.rx 0",response) || strcmp(response,"spi8: invalid byte count/hex\n")) return 1;
    struct modem m={0};struct s5123_boot_result result={0};
    struct s5123_boot_ops ops={&m,ready,prepare,output,delay,route,send_boot,park,restore,quiescent};
    int error=s5123_boot_once(&ops,&result,image,length,fields[2]);
    for (unsigned attempt=2;stay_mode && !stopped && attempt<=5 && error && result.cleanup_safe && result.phase<=5;attempt++) {
        printf("cellular live BOOT: attempt %u in 8 s (error %d at phase %u)\n",attempt,error,result.phase);fflush(stdout);
        sleep(8);
        memset(&m,0,sizeof(m));memset(&result,0,sizeof(result));cleanup=0;ipc_mapped=0;
        error=s5123_boot_once(&ops,&result,image,length,fields[2]);
    }
    if (restart_wanted && result.cleanup_safe && saved_argv) {
        unsetenv("REDWOOD_CELL_TRY");       /* a watchdog/asked restart is not a failed bring-up */
        printf("cellular: watchdog restart: exec %s\n",saved_argv[0]);fflush(stdout);sleep(3);execv(saved_argv[0],saved_argv);
    }
    printf("cellular live BOOT: error %d attempted %u power_requested %u phase %u completed %u cleanup_verified %u; acceptance unproven\n",
        error,result.attempted,result.power_requested,result.phase,result.spi_completed,result.cleanup_safe);fflush(stdout);
    /* (B, 2026-11-19) a bring-up that ends without a stop (the firmware download's stage 5 got no reply in 5 s right
     * after a thermal shutdown: "no modem" until a reboot) is retried as a whole, 3 tries 15 s apart. Still no loop
     * on a real modem fault: after 3 tries it stays off; airplane mode off (or a reboot) starts it again. */
    if (stay_mode && !stopped && result.cleanup_safe && saved_argv) {
        const char *t=getenv("REDWOOD_CELL_TRY");int tries=t?atoi(t):1;
        if (tries<3) {
            char n[8];snprintf(n,sizeof(n),"%d",tries+1);setenv("REDWOOD_CELL_TRY",n,1);
            printf("cellular: the modem did not stay up (try %d of 3): a fresh bring-up in 15 s\n",tries);fflush(stdout);
            sleep(15);execv(saved_argv[0],saved_argv);
        }
        printf("cellular: the modem did not come up in 3 tries: left off (airplane mode off/on tries again)\n");fflush(stdout);
    }
    return result.attempted && result.cleanup_safe ? (error?1:0):2;
}
#endif

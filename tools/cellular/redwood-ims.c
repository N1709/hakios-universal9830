/* MIT. redwood-ims: a small IMS (VoLTE signalling) client for the S20 port: registration on the carrier's
 * P-CSCF with IMS AKA and the IPsec (ESP transport) security agreement of TS 33.203, then it stays and answers.
 * Reads /var/tmp/redwood-cell/ims (interface, IMS address, P-CSCFs, IMSI, IMEI) written by redwood-cellulard and
 * talks to the ISIM through the daemon's command pipe (`apdu HEX` -> /var/tmp/redwood-cell/apdu).
 *
 *   redwood-ims [--pani VALUE] [--pcscf N] [--ua STRING] [--once] [--nullonly] [--udp]
 *   redwood-ims --pfkey          is the kernel's key socket there
 *   redwood-ims --flush          remove our SAs and policies
 *
 * Secrets (RES, CK, IK, the nonce) are never printed. */
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/event.h>
#include <notify.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include "sms-pdu.h"

/* every log line about calls carries the time (A, 2026-11-14: late rings could not be measured without it) */
static const char *ts_now(void)
{
    static char b[24];struct timeval tv;gettimeofday(&tv,NULL);struct tm tm;localtime_r(&tv.tv_sec,&tm);
    snprintf(b,sizeof(b),"%02d:%02d:%02d.%03d",tm.tm_hour,tm.tm_min,tm.tm_sec,(int)(tv.tv_usec/1000));return b;
}

#ifndef IPV6_BOUND_IF
#define IPV6_BOUND_IF 125
#endif

/* ---- PF_KEY v2 as this kernel has it (bsd/net/pfkeyv2.h, bsd/netinet6/ipsec.h of xnu-8796) ---- */
#define PF_KEY_V2 2
#define SADB_ADD 3
#define SADB_FLUSH 9
#define SADB_X_SPDADD 14
#define SADB_X_SPDFLUSH 19
#define SADB_SATYPE_ESP 3
#define SADB_EXT_SA 1
#define SADB_EXT_ADDRESS_SRC 5
#define SADB_EXT_ADDRESS_DST 6
#define SADB_EXT_KEY_AUTH 8
#define SADB_EXT_KEY_ENCRYPT 9
#define SADB_X_EXT_POLICY 18
#define SADB_X_EXT_SA2 19
#define SADB_SASTATE_MATURE 1
#define SADB_AALG_MD5HMAC 1
#define SADB_AALG_SHA1HMAC 2
#define SADB_EALG_NULL 3
#define SADB_X_EALG_AESCBC 12
#define IPSEC_ULPROTO_ANY 255
#define IPSEC_MODE_TRANSPORT 1
#define IPSEC_DIR_INBOUND 1
#define IPSEC_DIR_OUTBOUND 2
#define IPSEC_POLICY_IPSEC 2
#define IPSEC_LEVEL_REQUIRE 2
#define IPSEC_LEVEL_UNIQUE 3
struct sadb_msg {uint8_t version,type,err,satype;uint16_t len,reserved;uint32_t seq,pid;};
struct sadb_sa2k {uint16_t len,exttype;uint32_t spi;uint8_t replay,state,auth,encrypt;uint32_t flags;uint16_t natt_port,natt_interval,natt_offload,natt_src;};
struct sadb_x_sa2 {uint16_t len,exttype;uint8_t mode,r1;uint16_t r2;uint32_t sequence,reqid;};
struct sadb_address {uint16_t len,exttype;uint8_t proto,prefixlen;uint16_t reserved;};
struct sadb_key {uint16_t len,exttype,bits,reserved;};
struct sadb_x_policy {uint16_t len,exttype,type;uint8_t dir,reserved;uint32_t id,reserved2;};
struct sadb_x_ipsecrequest {uint16_t len,proto;uint8_t mode,level;uint16_t reqid;};

static struct {char ifn[16],ip6[64],pcscf[2][64],imsi[16],imei[20],mcc[4],mnc[4],realm[96];int npc;} cfg;
static int load_cfg(void)
{
    FILE *f=fopen("/var/tmp/redwood-cell/ims","r");if (!f) return -1;
    char k[32],v[96];int up=0;
    while (fscanf(f,"%31s %95s",k,v)==2) {
        if (!strcmp(k,"up")) up=atoi(v);
        else if (!strcmp(k,"ifname")) snprintf(cfg.ifn,sizeof(cfg.ifn),"%s",v);
        else if (!strcmp(k,"ip6")) snprintf(cfg.ip6,sizeof(cfg.ip6),"%s",v);
        else if (!strcmp(k,"pcscf") && cfg.npc<2) snprintf(cfg.pcscf[cfg.npc++],64,"%s",v);
        else if (!strcmp(k,"imsi")) snprintf(cfg.imsi,sizeof(cfg.imsi),"%s",v);
        else if (!strcmp(k,"imei")) snprintf(cfg.imei,sizeof(cfg.imei),"%s",v);
    }
    fclose(f);
    if (!up || !cfg.npc || strlen(cfg.imsi)<6) return -1;
    memcpy(cfg.mcc,cfg.imsi,3);memcpy(cfg.mnc,cfg.imsi+3,3);   /* India: 3-digit MNC */
    snprintf(cfg.realm,sizeof(cfg.realm),"ims.mnc%s.mcc%s.3gppnetwork.org",cfg.mnc,cfg.mcc);
    return 0;
}
static void show(const char *m)
{   /* a SIP message with the IMSI, the IMEI, the challenge and the digest masked */
    const char *p=m;size_t L=strlen(cfg.imsi),E=strlen(cfg.imei)>=14?14:0;
    while (*p) {
        if (L && !strncmp(p,cfg.imsi,L)) {printf("%.5s**********",cfg.imsi);p+=L;}
        else if (E && !strncmp(p,cfg.imei,8)) {printf("<imei>");p+=8;while (*p && *p!='>' && *p!='"') p++;}
        else if (!strncmp(p,"nonce=\"",7) || !strncmp(p,"response=\"",10)) {
            const char *q=strchr(p,'"')+1,*e=strchr(q,'"');
            fwrite(p,1,(size_t)(q-p),stdout);if (e>q) printf("<%d chars>",(int)(e-q));p=e?e:q;
        } else putchar(*p++);
    }
}
static const char *opt(int argc,char **argv,const char *k,const char *def)
{for (int i=1;i+1<argc;i++) if (!strcmp(argv[i],k)) return argv[i+1];return def;}
static int flag(int argc,char **argv,const char *k) {for (int i=1;i<argc;i++) if (!strcmp(argv[i],k)) return 1;return 0;}

/* ---- the SIM, through redwood-cellulard ---- */
static int hexval(int c) {return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:-1;}
static int apdu(const char *hex,uint8_t *out,int cap)
{   /* -> the answer's bytes as the daemon wrote them (the modem's 05 06 payload), or -1 */
    unlink("/var/tmp/redwood-cell/apdu");
    int fd=open("/var/tmp/redwood-cell/cmd",O_WRONLY|O_NONBLOCK);if (fd<0) return -1;
    char line[700];int n=snprintf(line,sizeof(line),"apdu %s\n",hex);
    if (write(fd,line,(size_t)n)!=n) {close(fd);return -1;}
    close(fd);
    for (int t=0;t<150;t++) {
        usleep(20000);
        FILE *f=fopen("/var/tmp/redwood-cell/apdu","r");if (!f) continue;
        char h[1400];int got=0;
        if (fgets(h,sizeof(h),f)) {for (int i=0;h[i] && h[i+1] && got<cap;i+=2) {int a=hexval(h[i]),b=hexval(h[i+1]);if (a<0 || b<0) break;out[got++]=(uint8_t)(a<<4|b);}}
        fclose(f);
        if (got) return got;
    }
    return -1;
}
static int isim_channel;
static int isim_open(void)
{
    uint8_t r[300];int n=apdu("0070000001",r,sizeof(r));if (n<3) return -1;
    int ch=0;for (int i=1;i+1<n;i++) if (r[i]==0x90 && r[i+1]==0x00) {ch=r[i-1];break;}
    if (ch<1 || ch>3) return -1;
    char c[64];snprintf(c,sizeof(c),"%02xa4040407a0000000871004",ch);
    n=apdu(c,r,sizeof(r));if (n<2) return -1;
    isim_channel=ch;return 0;
}
static void isim_close(void)
{uint8_t r[64];char c[32];if (!isim_channel) return;snprintf(c,sizeof(c),"007080%02x00",isim_channel);(void)apdu(c,r,sizeof(r));isim_channel=0;}
static int isim_aka(const uint8_t *rand,const uint8_t *autn,uint8_t *res,int *reslen,uint8_t *ck,uint8_t *ik)
{   /* AUTHENTICATE, 3G context (TS 31.103 7.1.2): DB | L RES | 10 CK | 10 IK.  0 ok, 1 synchronisation failure (DC), -1 */
    char c[160];int o=snprintf(c,sizeof(c),"%02x88008122" "10",isim_channel);
    for (int i=0;i<16;i++) o+=snprintf(c+o,sizeof(c)-(size_t)o,"%02x",rand[i]);
    o+=snprintf(c+o,sizeof(c)-(size_t)o,"10");
    for (int i=0;i<16;i++) o+=snprintf(c+o,sizeof(c)-(size_t)o,"%02x",autn[i]);
    snprintf(c+o,sizeof(c)-(size_t)o,"00");
    uint8_t r[300];int n=apdu(c,r,sizeof(r));if (n<2) return -1;
    for (int pass=0;pass<2;pass++) {
        for (int i=0;i+3<n;i++) {
            if (r[i]==0xdb && r[i+1]>=4 && r[i+1]<=16 && i+2+r[i+1]+17+17<=n && r[i+2+r[i+1]]==0x10 && r[i+2+r[i+1]+17]==0x10) {
                *reslen=r[i+1];memcpy(res,r+i+2,(size_t)*reslen);memcpy(ck,r+i+3+*reslen,16);memcpy(ik,r+i+4+*reslen+16,16);
                memset(r,0,sizeof(r));return 0;
            }
            if (r[i]==0xdc && r[i+1]==0x0e) return 1;
        }
        if (pass) break;
        /* 61 xx: more to fetch */
        int le=-1;for (int i=0;i+1<n;i++) if (r[i]==0x61 && i+2>=n-1) {le=r[i+1];break;}
        if (le<0) break;
        snprintf(c,sizeof(c),"%02xc00000%02x",isim_channel,le);n=apdu(c,r,sizeof(r));if (n<2) break;
    }
    printf("AKA: the SIM's answer is %d bytes and has neither DB nor DC (last two %02x %02x)\n",n,n>=2?r[n-2]:0,n>=1?r[n-1]:0);
    return -1;
}

/* ---- IPsec ---- */
static int pfk=-1;static uint32_t pfseq;
static int pf_send(uint8_t type,uint8_t satype,const uint8_t *ext,size_t n)
{
    uint8_t b[1024];struct sadb_msg *m=(struct sadb_msg *)b;memset(m,0,sizeof(*m));
    m->version=PF_KEY_V2;m->type=type;m->satype=satype;m->len=(uint16_t)((sizeof(*m)+n)/8);m->seq=++pfseq;m->pid=(uint32_t)getpid();
    if (n) memcpy(b+sizeof(*m),ext,n);
    if (write(pfk,b,sizeof(*m)+n)<0) return errno;
    for (int k=0;k<20;k++) {
        struct pollfd p={pfk,POLLIN,0};if (poll(&p,1,1000)<=0) return ETIMEDOUT;
        uint8_t r[2048];ssize_t g=read(pfk,r,sizeof(r));if (g<(ssize_t)sizeof(*m)) return EIO;
        struct sadb_msg *a=(struct sadb_msg *)r;if (a->seq==pfseq && a->pid==(uint32_t)getpid()) return a->err;
    }
    return ETIMEDOUT;
}
static size_t put_addr(uint8_t *b,uint16_t exttype,const struct in6_addr *a,int port)
{
    struct sadb_address *x=(struct sadb_address *)b;memset(b,0,40);x->len=5;x->exttype=exttype;x->proto=IPSEC_ULPROTO_ANY;x->prefixlen=128;
    struct sockaddr_in6 *s=(struct sockaddr_in6 *)(b+8);s->sin6_len=sizeof(*s);s->sin6_family=AF_INET6;s->sin6_port=htons((uint16_t)port);s->sin6_addr=*a;
    return 40;
}
static int sa_add(const struct in6_addr *src,const struct in6_addr *dst,uint32_t spi,uint32_t reqid,int aalg,const uint8_t *ak,int akl,int ealg,const uint8_t *ek,int ekl)
{
    uint8_t b[512];size_t o=0;
    struct sadb_sa2k *sa=(struct sadb_sa2k *)(b+o);memset(sa,0,sizeof(*sa));sa->len=sizeof(*sa)/8;sa->exttype=SADB_EXT_SA;sa->spi=htonl(spi);
    sa->replay=4;sa->state=SADB_SASTATE_MATURE;sa->auth=(uint8_t)aalg;sa->encrypt=(uint8_t)ealg;o+=sizeof(*sa);
    struct sadb_x_sa2 *s2=(struct sadb_x_sa2 *)(b+o);memset(s2,0,sizeof(*s2));s2->len=sizeof(*s2)/8;s2->exttype=SADB_X_EXT_SA2;s2->mode=IPSEC_MODE_TRANSPORT;s2->reqid=reqid;o+=sizeof(*s2);
    o+=put_addr(b+o,SADB_EXT_ADDRESS_SRC,src,0);o+=put_addr(b+o,SADB_EXT_ADDRESS_DST,dst,0);
    for (int k=0;k<2;k++) {
        const uint8_t *key=k?ek:ak;int kl=k?ekl:akl;if (!kl) continue;
        size_t L=(sizeof(struct sadb_key)+(size_t)kl+7)&~7u;struct sadb_key *x=(struct sadb_key *)(b+o);memset(x,0,L);
        x->len=(uint16_t)(L/8);x->exttype=k?SADB_EXT_KEY_ENCRYPT:SADB_EXT_KEY_AUTH;x->bits=(uint16_t)(kl*8);memcpy(b+o+sizeof(*x),key,(size_t)kl);o+=L;
    }
    int e=pf_send(SADB_ADD,SADB_SATYPE_ESP,b,o);memset(b,0,sizeof(b));return e;
}
static int sp_add(const struct in6_addr *src,int sport,const struct in6_addr *dst,int dport,int dir,uint32_t reqid)
{
    uint8_t b[256];size_t o=0;
    o+=put_addr(b+o,SADB_EXT_ADDRESS_SRC,src,sport);o+=put_addr(b+o,SADB_EXT_ADDRESS_DST,dst,dport);
    struct sadb_x_policy *p=(struct sadb_x_policy *)(b+o);memset(p,0,24);p->len=3;p->exttype=SADB_X_EXT_POLICY;p->type=IPSEC_POLICY_IPSEC;p->dir=(uint8_t)dir;
    struct sadb_x_ipsecrequest *q=(struct sadb_x_ipsecrequest *)(b+o+16);q->len=8;q->proto=IPPROTO_ESP;q->mode=IPSEC_MODE_TRANSPORT;
    q->level=reqid?IPSEC_LEVEL_UNIQUE:IPSEC_LEVEL_REQUIRE;q->reqid=(uint16_t)reqid;o+=24;
    return pf_send(SADB_X_SPDADD,0,b,o);
}
static void ipsec_flush(void) {(void)pf_send(SADB_X_SPDFLUSH,0,NULL,0);(void)pf_send(SADB_FLUSH,SADB_SATYPE_ESP,NULL,0);}

/* ---- small helpers ---- */
static int b64(const char *s,size_t n,uint8_t *o,int cap)
{
    int v=0,bits=0,g=0;
    for (size_t i=0;i<n;i++) {
        int c=s[i],d=c>='A' && c<='Z'?c-'A':c>='a' && c<='z'?c-'a'+26:c>='0' && c<='9'?c-'0'+52:c=='+'?62:c=='/'?63:-1;
        if (d<0) continue;
        v=v<<6|d;bits+=6;if (bits>=8) {bits-=8;if (g<cap) o[g++]=(uint8_t)(v>>bits);}
    }
    return g;
}
static void md5hex(const void *d,size_t n,char *hex)
{unsigned char h[16];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
 CC_MD5(d,(CC_LONG)n,h);
#pragma clang diagnostic pop
 for (int i=0;i<16;i++) sprintf(hex+2*i,"%02x",h[i]);}
static int bound(int type,int port)
{
    unsigned idx=if_nametoindex(cfg.ifn);int s=socket(AF_INET6,type,0);int one=1;if (s<0) return -1;
    if (type==SOCK_STREAM) {struct linger lg={1,0};setsockopt(s,SOL_SOCKET,SO_LINGER,&lg,sizeof(lg));}   /* close = reset: our fixed ports stay free */
    setsockopt(s,IPPROTO_IPV6,IPV6_BOUND_IF,&idx,sizeof(idx));setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));setsockopt(s,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one));
    struct sockaddr_in6 l={.sin6_len=sizeof(l),.sin6_family=AF_INET6,.sin6_port=htons((uint16_t)port)};inet_pton(AF_INET6,cfg.ip6,&l.sin6_addr);
    if (bind(s,(struct sockaddr *)&l,sizeof(l))) {close(s);return -1;}
    return s;
}

static const char *hdr(const char *msg,const char *name,char *out,size_t cap)
{   /* one header's value, folded lines joined */
    size_t L=strlen(name);out[0]=0;
    for (const char *p=msg;p && *p;) {
        if (!strncasecmp(p,name,L) && p[L]==':') {
            p+=L+1;size_t o=0;
            for (;;) {
                while (*p==' ' || *p=='\t') p++;
                const char *e=strstr(p,"\r\n");if (!e) e=p+strlen(p);
                if (o && o+1<cap) out[o++]=' ';
                size_t n=(size_t)(e-p);if (o+n>=cap) n=cap-1-o;memcpy(out+o,p,n);o+=n;out[o]=0;
                if (e[0] && (e[2]==' ' || e[2]=='\t')) p=e+2; else break;
            }
            return out;
        }
        p=strstr(p,"\r\n");if (p) p+=2;
    }
    return NULL;
}
static int param_u(const char *v,const char *k,uint32_t *out)
{   /* ";k=NUMBER" in a header value */
    size_t L=strlen(k);
    for (const char *p=v;(p=strstr(p,k));p+=L) {
        if ((p==v || p[-1]==';' || p[-1]==' ') && p[L]=='=') {*out=(uint32_t)strtoul(p+L+1,NULL,10);return 0;}
    }
    return -1;
}
/* (A, 2026-11-14) bytes after a complete message, per connection: one TCP read can carry two messages (Jio sent 180 and
 * 200 together; the 200 was lost and the call screen stayed on "calling"), so the rest is kept for the next call */
static struct {int fd;size_t n;char b[16384];} recv_left[16];
static int recv_pending(int s)
{
    for (int i=0;i<16;i++) if (recv_left[i].fd==s+1 && recv_left[i].n) return 1;
    return 0;
}
/* a connection is gone: drop what was kept of it (the next socket can get the same number) */
static void recv_forget(int s)
{
    for (int i=0;i<16;i++) if (recv_left[i].fd==s+1) {recv_left[i].n=0;recv_left[i].fd=0;}
}
/* The network closes our registration connection now and then (minutes after a text, 2026-11-15 08:07). The
 * registration itself (contact = our port-s, the IPsec SAs) is still good: open the connection again from port-c to
 * the P-CSCF's protected port instead of throwing the registration away. A full restart took 10-90 s, and texts that
 * came in meanwhile waited for the SMSC's next try (minutes): "receiving is unpredictable".
 * The connect has a 3 s limit: if the network dropped the SAs too, the SYN goes nowhere, and a blocking connect would
 * stall calls and texts for over a minute. Then the caller starts afresh. */
static int reg_reopen(int old,const struct sockaddr_in6 *to,int portc)
{
    if (old>=0) {recv_forget(old);close(old);}
    for (int t=0;t<2;t++) {
        int s=bound(SOCK_STREAM,portc);if (s<0) {usleep(300000);continue;}
        int fl=fcntl(s,F_GETFL,0),ok=0;fcntl(s,F_SETFL,fl|O_NONBLOCK);
        if (!connect(s,(const struct sockaddr *)to,sizeof(*to))) ok=1;
        else if (errno==EINPROGRESS) {
            struct pollfd w={s,POLLOUT,0};int err=0;socklen_t el=sizeof(err);
            if (poll(&w,1,3000)==1 && !getsockopt(s,SOL_SOCKET,SO_ERROR,&err,&el) && !err) ok=1;
        }
        if (ok) {
            fcntl(s,F_SETFL,fl);
            int on=1,idle=30,intvl=10,cnt=3;setsockopt(s,SOL_SOCKET,SO_KEEPALIVE,&on,sizeof(on));
            setsockopt(s,IPPROTO_TCP,TCP_KEEPALIVE,&idle,sizeof(idle));setsockopt(s,IPPROTO_TCP,TCP_KEEPINTVL,&intvl,sizeof(intvl));
            setsockopt(s,IPPROTO_TCP,TCP_KEEPCNT,&cnt,sizeof(cnt));
            return s;
        }
        close(s);
    }
    return -1;
}
static int recv_msg(int s,char *b,size_t cap,int secs)
{   /* one SIP message from a stream (headers + Content-Length), or a datagram */
    size_t n=0;b[0]=0;
    for (int i=0;i<16;i++) if (recv_left[i].fd==s+1 && recv_left[i].n) {
        n=recv_left[i].n<cap-1?recv_left[i].n:cap-1;memcpy(b,recv_left[i].b,n);b[n]=0;recv_left[i].n=0;break;
    }
    for (;;) {
        char *e=n?strstr(b,"\r\n\r\n"):NULL;
        if (e) {
            /* (keep-alive CRLFs in front of a message are skipped) */
            size_t skip=0;while (skip+1<n && b[skip]=='\r' && b[skip+1]=='\n') skip+=2;
            if (skip) {memmove(b,b+skip,n-skip+1);n-=skip;continue;}
            char cl[32];int body=hdr(b,"Content-Length",cl,sizeof(cl))?atoi(cl):hdr(b,"l",cl,sizeof(cl))?atoi(cl):0;
            size_t len=(size_t)(e+4-b)+(size_t)(body>0?body:0);
            if (n>=len) {
                if (n>len) {   /* the next message(s): kept */
                    int k=-1;for (int i=0;i<16;i++) if (recv_left[i].fd==s+1 || (k<0 && !recv_left[i].n)) {k=i;if (recv_left[i].fd==s+1) break;}
                    if (k>=0) {size_t m=n-len<sizeof(recv_left[k].b)?n-len:sizeof(recv_left[k].b);memcpy(recv_left[k].b,b+len,m);recv_left[k].n=m;recv_left[k].fd=s+1;}
                }
                b[len]=0;return (int)len;
            }
        }
        struct pollfd p={s,POLLIN,0};if (poll(&p,1,secs*1000)<=0) return n?(int)n:-1;
        ssize_t r=recv(s,b+n,cap-1-n,0);if (r<=0) return n?(int)n:-1;
        n+=(size_t)r;b[n]=0;
    }
}

/* ---- SMS over IP: MESSAGE with application/vnd.3gpp.sms carries an RP message (TS 24.011): network -> UE RP-DATA
 * {0x01, ref, originator (SMSC) address, empty destination, user data len, TPDU SMS-DELIVER}; we answer the MESSAGE 200
 * and send RP-ACK {0x02, ref, 0x41, 2, SMS-DELIVER-REPORT 00 00} in a MESSAGE of our own back to the sender URI. */
static void sms_job_result(unsigned ref,int ok);
static char sms_route[1600],sms_impu[256],sms_sc_uri[128];static uint8_t sms_sc[12];static unsigned sms_scl,sms_mr,sms_rpref=1;
static void sms_keep_registration(const char *ok)
{   /* Service-Route(s) -> Route lines for our own requests; first P-Associated-URI as our identity */
    size_t o=0;const char *q=ok;sms_route[0]=sms_impu[0]=0;
    while ((q=strstr(q,"\r\nService-Route:")) && o<sizeof(sms_route)-8) {
        const char *v=q+16,*e=strstr(v,"\r\n");while (*v==' ') v++;
        o+=(size_t)snprintf(sms_route+o,sizeof(sms_route)-o,"Route: %.*s\r\n",(int)(e-v),v);q=e;
    }
    const char *a=strstr(ok,"\r\nP-Associated-URI:");
    if (a) {const char *b=strchr(a,'<'),*e=b?strchr(b,'>'):NULL;if (b && e && e-b<(long)sizeof(sms_impu)) snprintf(sms_impu,sizeof(sms_impu),"%.*s",(int)(e-b-1),b+1);}
    printf("SMS: %s Route line(s), identity %s\n",o?"with":"no",sms_impu[0]?"known":"unknown");
}
static void sms_to_inbox(const char *from,unsigned ref,const char *text)
{   /* for the Messages app: /var/tmp/redwood-cell/sms-in/<time>-<ref>.txt = sender, unix time, text (mobile, 0600) */
    mkdir("/var/tmp/redwood-cell/sms-in",0700);chown("/var/tmp/redwood-cell/sms-in",501,501);
    char fn[160],tmp[170];static unsigned seq;snprintf(fn,sizeof(fn),"/var/tmp/redwood-cell/sms-in/%lld-%u-%u.txt",(long long)time(NULL),ref,seq++);
    snprintf(tmp,sizeof(tmp),"%s.tmp",fn);
    FILE *q=fopen(tmp,"w");
    if (q) {fchmod(fileno(q),0600);fprintf(q,"%s\n%lld\n%s",from,(long long)time(NULL),text);fclose(q);chown(tmp,501,501);rename(tmp,fn);}
}
/* concatenated texts (TS 23.040 UDH IEI 00 / 08): parts held per sender+reference until complete or 5 minutes old */
static struct {char from[64];unsigned ref,total;time_t first;char *part[32];} sms_cat[8];
static int sms_concat_info(const uint8_t *pdu,unsigned n,unsigned *ref,unsigned *total,unsigned *seq)
{
    unsigned i=1+pdu[0];if (i>=n) return 0;unsigned fo=pdu[i++];if (!(fo&0x40)) return 0;
    if (i+2>n) return 0;unsigned ol=pdu[i];i+=2+(ol+1)/2;i+=2+7;if (i+2>n) return 0;
    i++;const uint8_t *ud=pdu+i;unsigned udhl=ud[0];if (i+1+udhl>n) return 0;
    for (unsigned k=1;k+1<=udhl;) {
        unsigned iei=ud[k],l=ud[k+1];if (k+2+l>udhl+1) break;
        if (iei==0 && l==3) {*ref=ud[k+2];*total=ud[k+3];*seq=ud[k+4];return *total>1 && *seq>=1 && *seq<=*total && *total<=32;}
        if (iei==8 && l==4) {*ref=(ud[k+2]<<8)|ud[k+3];*total=ud[k+4];*seq=ud[k+5];return *total>1 && *seq>=1 && *seq<=*total && *total<=32;}
        k+=2+l;
    }
    return 0;
}
static void sms_cat_flush(int i,int complete)
{
    char all[6000];size_t o=0;all[0]=0;
    for (unsigned k=0;k<sms_cat[i].total;k++) {
        const char *t=sms_cat[i].part[k]?sms_cat[i].part[k]:"…";
        size_t L=strlen(t);if (o+L+1<sizeof(all)) {memcpy(all+o,t,L);o+=L;all[o]=0;}
        free(sms_cat[i].part[k]);sms_cat[i].part[k]=NULL;
    }
    printf("SMS: %u-part text from …%s joined%s\n",sms_cat[i].total,strlen(sms_cat[i].from)>4?sms_cat[i].from+strlen(sms_cat[i].from)-4:sms_cat[i].from,complete?"":" (parts missing after 5 min)");fflush(stdout);
    sms_to_inbox(sms_cat[i].from,sms_cat[i].ref,all);
    sms_cat[i].from[0]=0;
}
static void sms_cat_tick(void)
{
    for (int i=0;i<8;i++) if (sms_cat[i].from[0] && time(NULL)-sms_cat[i].first>300) sms_cat_flush(i,0);
}
static void sms_cat_add(const char *from,unsigned ref,unsigned total,unsigned seq,const char *text)
{
    int i=-1,freei=-1;
    for (int k=0;k<8;k++) {if (sms_cat[k].from[0] && sms_cat[k].ref==ref && sms_cat[k].total==total && !strcmp(sms_cat[k].from,from)) {i=k;break;} if (!sms_cat[k].from[0] && freei<0) freei=k;}
    if (i<0) {
        if (freei<0) {freei=0;for (int k=1;k<8;k++) if (sms_cat[k].first<sms_cat[freei].first) freei=k;sms_cat_flush(freei,0);}
        i=freei;snprintf(sms_cat[i].from,sizeof(sms_cat[i].from),"%s",from);sms_cat[i].ref=ref;sms_cat[i].total=total;sms_cat[i].first=time(NULL);
    }
    if (!sms_cat[i].part[seq-1]) sms_cat[i].part[seq-1]=strdup(text);
    for (unsigned k=0;k<total;k++) if (!sms_cat[i].part[k]) return;
    sms_cat_flush(i,1);
}
/* RP-ACK {0x02, ref, 0x41, 2, SMS-DELIVER-REPORT 00 00} in a MESSAGE of our own to TARGET. When it went to the SMSC
 * (P-Asserted-Identity) and the network refuses it, it goes once more to ALT (the From URI, the old way), so the SMSC
 * is never left without an answer (it would deliver the text again) (A, 2026-11-15). */
static struct {uint32_t cid;uint8_t ref;char alt[300],inrep[200];time_t t;} sms_acks[8];
static void sms_rpack(int sock,int portc,const char *target,int tlen,unsigned ref,const char *inrep,const char *alt)
{
    uint8_t ack[6]={0x02,(uint8_t)ref,0x41,0x02,0x00,0x00};uint32_t cid=arc4random();
    char m[4000];int o=snprintf(m,sizeof(m),
        "MESSAGE %.*s SIP/2.0\r\n"
        "Via: SIP/2.0/TCP [%s]:%d;branch=z9hG4bK%08x;rport\r\n"
        "Max-Forwards: 70\r\n"
        "%s"
        "From: <%s>;tag=%08x\r\n"
        "To: <%.*s>\r\n"
        "Call-ID: %08x@[%s]\r\n"
        "CSeq: 1 MESSAGE\r\n"
        "In-Reply-To: %s\r\n"
        "Request-Disposition: no-fork\r\n"
        "Accept-Contact: *;+g.3gpp.smsip\r\n"
        "Content-Type: application/vnd.3gpp.sms\r\n"
        "Content-Length: %zu\r\n\r\n",
        tlen,target,cfg.ip6,portc,arc4random(),sms_route,sms_impu[0]?sms_impu:"sip:anonymous@anonymous.invalid",arc4random(),
        tlen,target,cid,cfg.ip6,inrep,sizeof(ack));
    if (o<0 || (size_t)o+sizeof(ack)>sizeof(m)) return;
    memcpy(m+o,ack,sizeof(ack));
    ssize_t w=send(sock,m,(size_t)o+sizeof(ack),0);
    printf("--> RP-ACK (ref %u) to the %s %s\n",ref,alt?"SMSC":"sender",w>0?"sent":"NOT sent");fflush(stdout);
    if (alt && alt[0]) {   /* remember it until its answer (or 60 s) */
        int k=0;for (int i=0;i<8;i++) if (!sms_acks[i].cid || time(NULL)-sms_acks[i].t>60) {k=i;break;}
        sms_acks[k].cid=cid;sms_acks[k].ref=(uint8_t)ref;sms_acks[k].t=time(NULL);
        snprintf(sms_acks[k].alt,sizeof(sms_acks[k].alt),"%s",alt);snprintf(sms_acks[k].inrep,sizeof(sms_acks[k].inrep),"%s",inrep);
    }
}
/* the answer to one of our RP-ACKs: 2xx done; an error -> once more to the From URI. 1 = it was ours. */
static int sms_ack_answer(int sock,const char *r,int portc)
{
    char ci[200];if (!hdr(r,"Call-ID",ci,sizeof(ci))) return 0;
    uint32_t cid=(uint32_t)strtoul(ci,NULL,16);
    for (int i=0;i<8;i++) if (sms_acks[i].cid && sms_acks[i].cid==cid) {
        int code=atoi(r+8);if (code<200) return 1;
        sms_acks[i].cid=0;
        if (code>=300) {printf("SMS: the SMSC address refused the RP-ACK (%d): sent to the sender instead\n",code);
            sms_rpack(sock,portc,sms_acks[i].alt,(int)strlen(sms_acks[i].alt),sms_acks[i].ref,sms_acks[i].inrep,NULL);}
        return 1;
    }
    return 0;
}
static int sms_incoming(int sock,const char *req,size_t len,int portc)
{
    char ct[120],cl[24],fr[300],ci[200];
    if (strncmp(req,"MESSAGE ",8) || !hdr(req,"Content-Type",ct,sizeof(ct)) || !strstr(ct,"vnd.3gpp.sms")) return 0;
    const char *hb=strstr(req,"\r\n\r\n");if (!hb) return 0;
    const uint8_t *b=(const uint8_t *)hb+4;size_t n=hdr(req,"Content-Length",cl,sizeof(cl))?(size_t)atoi(cl):0;
    if ((size_t)(hb+4-req)+n>len || n<2) {printf("SMS: body short\n");return 1;}
    if (b[0]==0x03 || b[0]==0x05) {   /* the result of one of our sends: tell the Messages app if it was its */
        sms_job_result(b[1],b[0]==3);
    }
    if (b[0]==0x03) {printf("SMS: sent -- the SMSC confirmed (RP-ACK, ref %u)\n",b[1]);fflush(stdout);return 1;}
    if (b[0]==0x05) {printf("SMS: NOT sent -- RP-ERROR ref %u, cause %u\n",b[1],n>3?b[3]&0x7f:0);fflush(stdout);return 1;}
    if (b[0]!=0x01) {printf("SMS: RP message type %u from the network, %zu bytes\n",b[0],n);return 1;}
    unsigned ref=b[1],i=2;
    if (i>=n) return 1;
    unsigned ol=b[i];if (ol>=2 && ol<=sizeof(sms_sc) && i+1+ol<=n) {memcpy(sms_sc,b+i+1,ol);sms_scl=ol;}
    i+=1+ol;if (i>=n) return 1;
    unsigned dl=b[i];i+=1+dl;if (i>=n) return 1;
    unsigned ul=b[i++];
    char from[64]="?",text[1600]="",when[32]="";uint8_t pdu[260];int ok=0;
    if (i+ul<=n && ul<sizeof(pdu)-1) {pdu[0]=0;memcpy(pdu+1,b+i,ul);ok=!sms_decode(pdu,ul+1,from,sizeof(from),text,sizeof(text),when,sizeof(when));}
    printf("SMS (IMS) from %s at %s: %s\n",from,when,ok?text:"(undecoded TPDU)");fflush(stdout);
    if (ok) {
        unsigned cr=0,ct=0,cs=0;int cat=sms_concat_info(pdu,ul+1,&cr,&ct,&cs);
        /* the same text again (same sender, SMSC time stamp, part and text) is an SMSC retry: ACK it, do not show it twice */
        /* (kept in a file too: redwood-ims restarts every ~110 min, and an SMSC retry can come after that) */
        static struct {uint64_t h;time_t t;} seen[32];static unsigned seen_n;static int seen_loaded;
        if (!seen_loaded) {seen_loaded=1;FILE *q=fopen("/var/root/ims-sms-seen","rb");
            if (q) {if (fread(seen,sizeof(seen),1,q)!=1 || fread(&seen_n,sizeof(seen_n),1,q)!=1) {memset(seen,0,sizeof(seen));seen_n=0;}fclose(q);}}
        uint64_t h=1469598103934665603ULL;const char *parts[3]={from,when,text};
        for (int k=0;k<3;k++) {for (const char *c=parts[k];*c;c++) h=(h^(uint8_t)*c)*1099511628211ULL;h=(h^0xff)*1099511628211ULL;}
        h=(h^(cr<<16|cs))*1099511628211ULL;
        time_t now=time(NULL);int dup=0;
        /* (single texts only: parts of a long one are put together per part in sms_cat_add, which keeps one copy of each;
         * skipping a part here after a restart would leave that text incomplete) */
        if (!cat) for (unsigned k=0;k<32;k++) if (seen[k].h==h && now-seen[k].t<86400) dup=1;
        if (dup) {printf("SMS: repeat of a text already delivered (SMSC retry), not shown again\n");fflush(stdout);}
        else if (cat) sms_cat_add(from,cr,ct,cs,text);
        else {seen[seen_n%32].h=h;seen[seen_n%32].t=now;seen_n++;
            {FILE *q=fopen("/var/root/ims-sms-seen","wb");if (q) {fchmod(fileno(q),0600);fwrite(seen,sizeof(seen),1,q);fwrite(&seen_n,sizeof(seen_n),1,q);fclose(q);}}
            sms_to_inbox(from,ref,text);}
    }
    FILE *f=fopen("/var/root/cell-sms.txt","a");
    if (f) {fchmod(fileno(f),0600);fprintf(f,"%llu\t%s\t%s\t",(unsigned long long)time(NULL),from,when);
        if (ok) {for (char *c=text;*c;c++) fputc(*c=='\n'||*c=='\t'?' ':*c,f);} else {fputs("tpdu:",f);for (unsigned k=0;k<ul && i+k<n;k++) fprintf(f,"%02x",b[i+k]);}
        fputc('\n',f);fclose(f);}
    /* RP-ACK back to the sender (the SMSC's URI in From) */
    if (!hdr(req,"From",fr,sizeof(fr)) || !hdr(req,"Call-ID",ci,sizeof(ci))) return 1;
    const char *u1=strchr(fr,'<'),*u2=u1?strchr(u1,'>'):NULL;if (!u1 || !u2) return 1;
    if (u2-u1-1<(long)sizeof(sms_sc_uri)) snprintf(sms_sc_uri,sizeof(sms_sc_uri),"%.*s",(int)(u2-u1-1),u1+1);
    {FILE *q=fopen("/var/root/cell-smsc","w");if (q) {fchmod(fileno(q),0600);fprintf(q,"%s\n",sms_sc_uri);for (unsigned k=0;k<sms_scl;k++) fprintf(q,"%02x",sms_sc[k]);fputc('\n',q);fclose(q);}}
    /* The RP-ACK goes to the SMSC: the URI in P-Asserted-Identity of its MESSAGE (TS 24.341 5.3.2.4; Jio: sip:A2P@...).
     * The From URI (the originator's number) is only the fallback: sent there, Jio sometimes answers 403 and the SMSC,
     * never acknowledged, delivers the same text again after ~1 and ~5 min (A, 2026-11-15: triple texts). */
    char pai[300];const char *t1=u1,*t2=u2;
    if (hdr(req,"P-Asserted-Identity",pai,sizeof(pai))) {const char *p1=strchr(pai,'<'),*p2=p1?strchr(p1,'>'):NULL;
        if (p1 && p2 && p2-p1>5 && !strncmp(p1+1,"sip:",4)) {t1=p1;t2=p2;}}
    char alt[300]="";if (t1!=u1) snprintf(alt,sizeof(alt),"%.*s",(int)(u2-u1-1),u1+1);
    sms_rpack(sock,portc,t1+1,(int)(t2-t1-1),ref,ci,t1!=u1?alt:NULL);
    return 1;
}

/* text -> parts: GSM 7-bit when every character is in the basic table's ASCII-equal part (160 a text, 153 a part
 * with the concatenation header), else UCS-2 (70 / 67 UTF-16 units, a surrogate pair never split) */
static int sms_is_gsm(const char *t)
{
    static const char ok7[]=" !\"#%&'()*+,-./0123456789:;<=>?ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\n\r";
    for (const char *c=t;*c;c++) if (!strchr(ok7,*c)) return 0;
    return 1;
}
static int sms_utf16(const char *t,uint16_t *u,int cap)
{
    int o=0;const unsigned char *p=(const unsigned char *)t;
    while (*p) {
        unsigned c;
        if (*p<0x80) c=*p++;
        else if ((*p&0xe0)==0xc0 && p[1]) {c=((p[0]&0x1fu)<<6)|(p[1]&0x3fu);p+=2;}
        else if ((*p&0xf0)==0xe0 && p[1] && p[2]) {c=((p[0]&0x0fu)<<12)|((p[1]&0x3fu)<<6)|(p[2]&0x3fu);p+=3;}
        else if ((*p&0xf8)==0xf0 && p[1] && p[2] && p[3]) {c=((p[0]&0x07u)<<18)|((p[1]&0x3fu)<<12)|((p[2]&0x3fu)<<6)|(p[3]&0x3fu);p+=4;}
        else return -1;
        if (c>=0x10000) {if (o+2>cap) return -1;c-=0x10000;u[o++]=(uint16_t)(0xd800+(c>>10));u[o++]=(uint16_t)(0xdc00+(c&0x3ff));}
        else {if (o+1>cap) return -1;u[o++]=(uint16_t)c;}
    }
    return o;
}
/* the Messages app's pending sends: every part's RP ref points at a job; the job answers when all parts did */
static struct {char id[80];int left,failed,retried;time_t t;char body[1400];} sms_job[32];
static int sms_conn_dead;   /* a text got no answer: the registration is dead (main loop starts afresh) */
static unsigned char sms_ref_job[256];   /* RP ref -> job index + 1 */
static void sms_job_result(unsigned ref,int ok)
{
    unsigned j=sms_ref_job[ref&0xff];if (!j) return;
    sms_ref_job[ref&0xff]=0;j--;
    if (!ok) sms_job[j].failed=1;
    if (--sms_job[j].left>0) return;
    char fn[200];snprintf(fn,sizeof(fn),"/var/tmp/redwood-cell/sms-out/%s.%s",sms_job[j].id,sms_job[j].failed?"fail":"ok");
    FILE *q=fopen(fn,"w");if (q) {fclose(q);chown(fn,501,501);}
    sms_job[j].id[0]=0;
}
/* A text the SMSC has not confirmed in 20 s (normally 0.2 s): the registration looks alive but nothing gets through
 * (seen 2026-11-15 08:17-08:20: 200 OK to the REGISTER, then no answer to anything, no texts in). The text goes back
 * into sms-out as <id>.retry.msg and the main loop registers afresh; the new registration sends it once more. A retry
 * that is not confirmed either fails (no second fresh start for it). Its refs are forgotten so a very late RP-ACK
 * cannot count for the next text in that slot. (A, 2026-11-15) */
static void sms_job_free(int j)
{
    for (int r=0;r<256;r++) if (sms_ref_job[r]==j+1) sms_ref_job[r]=0;
    sms_job[j].id[0]=0;
}
static void sms_job_expire(void)
{
    time_t now=time(NULL);
    for (int j=0;j<32;j++) if (sms_job[j].id[0] && now-sms_job[j].t>20) {
        char fn[300],tmp[310];
        if (!sms_job[j].retried && sms_job[j].body[0]) {
            snprintf(fn,sizeof(fn),"/var/tmp/redwood-cell/sms-out/%s.retry.msg",sms_job[j].id);snprintf(tmp,sizeof(tmp),"%s.tmp",fn);
            FILE *q=fopen(tmp,"w");if (q) {fputs(sms_job[j].body,q);fclose(q);chown(tmp,501,501);rename(tmp,fn);}
            printf("SMS: no confirmation in 20 s: the text is queued again and the registration renewed\n");sms_conn_dead=1;
        } else {
            snprintf(fn,sizeof(fn),"/var/tmp/redwood-cell/sms-out/%s.fail",sms_job[j].id);
            FILE *q=fopen(fn,"w");if (q) {fclose(q);chown(fn,501,501);}
            printf("SMS: no confirmation for a send (retried): failed\n");
        }
        fflush(stdout);sms_job_free(j);
    }
}
static int sms_send_tpdu(int sock,int portc,const uint8_t *tp,unsigned t)
{   /* one TPDU in an RP-DATA to the SMSC: the RP ref, or -1 */
    uint8_t rp[260];unsigned r=0;unsigned ref=sms_rpref++&0xff;
    rp[r++]=0x00;rp[r++]=(uint8_t)ref;rp[r++]=0x00;                  /* RP-DATA MS->network, ref, no originator */
    rp[r++]=(uint8_t)sms_scl;memcpy(rp+r,sms_sc,sms_scl);r+=sms_scl;  /* destination: the SMSC */
    rp[r++]=(uint8_t)t;memcpy(rp+r,tp,t);r+=t;
    char m[4000];int o=snprintf(m,sizeof(m),
        "MESSAGE %s SIP/2.0\r\n"
        "Via: SIP/2.0/TCP [%s]:%d;branch=z9hG4bK%08x;rport\r\n"
        "Max-Forwards: 70\r\n"
        "%s"
        "From: <%s>;tag=%08x\r\n"
        "To: <%s>\r\n"
        "Call-ID: %08x@[%s]\r\n"
        "CSeq: 1 MESSAGE\r\n"
        "Request-Disposition: no-fork\r\n"
        "Accept-Contact: *;+g.3gpp.smsip\r\n"
        "Content-Type: application/vnd.3gpp.sms\r\n"
        "Content-Length: %u\r\n\r\n",
        sms_sc_uri,cfg.ip6,portc,arc4random(),sms_route,sms_impu[0]?sms_impu:"sip:anonymous@anonymous.invalid",arc4random(),
        sms_sc_uri,arc4random(),cfg.ip6,r);
    if (o<0 || (size_t)o+r>sizeof(m)) return -1;
    memcpy(m+o,rp,r);
    return send(sock,m,(size_t)o+r,0)>0?(int)ref:-1;
}
static void sms_smsc_load(void)
{   /* the SMSC learnt from an earlier incoming text, else Jio's (seen as the sender of every incoming text) */
    if (sms_scl && sms_sc_uri[0]) return;
    char uri[128]="tel:+917012075009",hex[64]="";
    FILE *q=fopen("/var/root/cell-smsc","r");
    if (q) {if (fscanf(q,"%127s %63s",uri,hex)<1) uri[0]=0;fclose(q);}
    if (!uri[0]) return;
    snprintf(sms_sc_uri,sizeof(sms_sc_uri),"%s",uri);
    size_t hl=strlen(hex);sms_scl=0;
    for (size_t k=0;k+1<hl && sms_scl<sizeof(sms_sc);k+=2) {int a=hexval(hex[k]),b=hexval(hex[k+1]);if (a<0||b<0) {sms_scl=0;break;}sms_sc[sms_scl++]=(uint8_t)(a<<4|b);}
    if (!sms_scl) {   /* from the URI's digits: TON international, BCD */
        const char *d=strchr(uri,'+');d=d?d+1:uri;char dg[24];size_t nd=0;
        for (;*d && nd<20;d++) if (*d>='0' && *d<='9') dg[nd++]=*d;
        sms_sc[sms_scl++]=0x91;
        for (size_t k=0;k<nd;k+=2) sms_sc[sms_scl++]=(uint8_t)((dg[k]-'0')|((k+1<nd?dg[k+1]-'0':15)<<4));
    }
    printf("SMS: SMSC %s\n",sms_sc_uri);
}
static int sms_send(int sock,int portc,const char *number,const char *text,const char *job_id)
{
    sms_smsc_load();
    if (!sms_scl || !sms_sc_uri[0]) {printf("SMS: no SMSC known yet\n");fflush(stdout);return -1;}
    char dig[24];size_t nd=0;int intl=number[0]=='+';for (const char *c=number;*c && nd<20;c++) if (*c>='0' && *c<='9') dig[nd++]=*c;dig[nd]=0;
    if (nd<3) {printf("SMS: bad number\n");return -1;}
    if (!intl && nd==10 && dig[0]>='6') {memmove(dig+2,dig,11);dig[0]='9';dig[1]='1';nd=12;intl=1;}   /* Indian mobile without +91 */
    else if (!intl && nd==11 && dig[0]=='0') {memmove(dig+2,dig+1,11);dig[0]='9';dig[1]='1';nd=12;intl=1;}
    int gsm=sms_is_gsm(text);
    static uint16_t u[2048];int n=gsm?(int)strlen(text):sms_utf16(text,u,2048);
    if (n<0 || (gsm && n>153*255) || (!gsm && n>67*255)) {printf("SMS: text not encodable or too long\n");fflush(stdout);return -1;}
    int one=gsm?160:70,per=gsm?153:67;
    int parts=n<=one?1:(n+per-1)/per;
    if (!gsm && parts>1) {int k=0,c=0;while (k<n) {int e=k+per;if (e<n && u[e-1]>=0xd800 && u[e-1]<0xdc00) e--;k=e<n?e:n;c++;}parts=c;}
    static uint8_t cref;cref++;
    int j=-1;
    if (job_id) {for (int i=0;i<32;i++) if (!sms_job[i].id[0]) {j=i;break;}
        if (j<0) {printf("SMS: too many sends waiting\n");return -1;}
        snprintf(sms_job[j].id,sizeof(sms_job[j].id),"%s",job_id);sms_job[j].left=parts;sms_job[j].failed=0;sms_job[j].t=time(NULL);}
    int pos=0,sent=0;
    for (int part=1;part<=parts;part++) {
        int len=parts==1?n:per;if (pos+len>n) len=n-pos;
        if (!gsm && parts>1 && pos+len<n && u[pos+len-1]>=0xd800 && u[pos+len-1]<0xdc00) len--;
        uint8_t tp[200];unsigned t=0;
        tp[t++]=(uint8_t)(0x01|(parts>1?0x40:0));tp[t++]=(uint8_t)(sms_mr++);   /* SMS-SUBMIT (UDHI when in parts), TP-MR */
        tp[t++]=(uint8_t)nd;tp[t++]=intl?0x91:0x81;
        for (size_t k=0;k<nd;k+=2) tp[t++]=(uint8_t)((dig[k]-'0')|((k+1<nd?dig[k+1]-'0':15)<<4));
        tp[t++]=0x00;tp[t++]=gsm?0x00:0x08;
        uint8_t udh[6]={5,0,3,cref,(uint8_t)parts,(uint8_t)part};int hl=parts>1?6:0;
        unsigned udlpos=t++;uint8_t *ud=tp+t;memset(ud,0,140);
        if (hl) memcpy(ud,udh,(size_t)hl);
        if (gsm) {   /* septets after the header, which is padded to a septet boundary (6 bytes = 48 bits -> 49) */
            unsigned skip=hl?49:0;
            for (int k=0;k<len;k++) {unsigned bit=skip+7*(unsigned)k;uint8_t c=(uint8_t)text[pos+k];ud[bit/8]|=(uint8_t)(c<<(bit%8));if (bit%8>1) ud[bit/8+1]|=(uint8_t)(c>>(8-bit%8));}
            unsigned septets=(hl?7:0)+(unsigned)len;tp[udlpos]=(uint8_t)septets;t+=(septets*7+7)/8;
        } else {
            for (int k=0;k<len;k++) {ud[hl+2*k]=(uint8_t)(u[pos+k]>>8);ud[hl+2*k+1]=(uint8_t)u[pos+k];}
            tp[udlpos]=(uint8_t)(hl+2*len);t+=(unsigned)(hl+2*len);
        }
        int ref=sms_send_tpdu(sock,portc,tp,t);
        if (ref<0) {if (j>=0) {sms_job[j].failed=1;sms_job[j].left-=(parts-part+1);}break;}
        if (j>=0) sms_ref_job[ref]=(unsigned char)(j+1);
        sent++;pos+=len;
    }
    printf("--> SMS to …%s: %d character(s) in %d part(s), %s, %d handed to the network\n",nd>4?dig+nd-4:dig,n,parts,gsm?"GSM":"UCS-2",sent);fflush(stdout);
    if (j>=0 && sent==0) {sms_job[j].id[0]=0;return -1;}   /* nothing went out: the caller answers "failed" */
    return 0;                                               /* parts out: their RP-ACK/RP-ERROR answer the job */
}
static int sms_out_watched,sms_out_kick=1;   /* the main loop's kqueue on sms-out: set = read it now */
static void sms_outbox(int sock,int portc)
{   /* /var/tmp/redwood-cell/sms-out/<id>.msg from the Messages app: number on the first line, then the text */
    mkdir("/var/tmp/redwood-cell/sms-out",0700);chown("/var/tmp/redwood-cell/sms-out",501,501);
    DIR *d=opendir("/var/tmp/redwood-cell/sms-out");if (!d) return;
    struct dirent *e;
    while ((e=readdir(d))) {
        size_t L=strlen(e->d_name);if (L<5 || strcmp(e->d_name+L-4,".msg") || L-4>=80) continue;
        char fn[300],body[1400];snprintf(fn,sizeof(fn),"/var/tmp/redwood-cell/sms-out/%s",e->d_name);
        FILE *q=fopen(fn,"r");if (!q) continue;size_t n=fread(body,1,sizeof(body)-1,q);fclose(q);body[n]=0;unlink(fn);
        char id[80];snprintf(id,sizeof(id),"%.*s",(int)(L-4),e->d_name);
        int retried=0;size_t il=strlen(id);if (il>6 && !strcmp(id+il-6,".retry")) {id[il-6]=0;retried=1;}
        char keep[1400];snprintf(keep,sizeof(keep),"%s",body);
        char *nl=strchr(body,'\n');int rc=-1;
        if (nl) {*nl=0;rc=sms_send(sock,portc,body,nl+1,id);}
        if (rc>=0) for (int j=0;j<32;j++) if (sms_job[j].id[0] && !strcmp(sms_job[j].id,id)) {sms_job[j].retried=retried;snprintf(sms_job[j].body,sizeof(sms_job[j].body),"%s",keep);}
        if (rc<0) {snprintf(fn,sizeof(fn),"/var/tmp/redwood-cell/sms-out/%s.fail",id);q=fopen(fn,"w");if (q) {fclose(q);chown(fn,501,501);}}
    }
    closedir(d);
}
/* command pipe for the client: "sms NUMBER TEXT" (root only) */

/* Calls (2026-11-13/14 A, rewritten for several calls 2026-11-15 B): ims-media.h is the voice, ims-call.h the SIP side */
#define CALL_RTP_PORT 50010
#include "ims-media.h"
#include "ims-call.h"

static int ims_cmd_fd=-1;
static void ims_cmd_poll(int sock,int portc)
{
    {static time_t last;time_t now=time(NULL);   /* only when the folder changed (or every 5 s; every turn without the watch) */
     if (sms_out_kick || !sms_out_watched || now-last>=5) {sms_out_kick=0;last=now;sms_outbox(sock,portc);}}
    sms_cat_tick();
    sms_job_expire();
    if (ims_cmd_fd<0) {
        unlink("/var/tmp/redwood-cell/ims-cmd");
        if (mkfifo("/var/tmp/redwood-cell/ims-cmd",0600)) return;
        ims_cmd_fd=open("/var/tmp/redwood-cell/ims-cmd",O_RDWR|O_NONBLOCK);
        if (ims_cmd_fd<0) return;
    }
    static char buf[2048];static size_t have;
    ssize_t g=read(ims_cmd_fd,buf+have,sizeof(buf)-1-have);if (g<=0) return;have+=(size_t)g;buf[have]=0;
    char *nl;
    while ((nl=strchr(buf,'\n'))) {
        *nl=0;
        if (!strncmp(buf,"sms ",4)) {char *num=buf+4;while (*num==' ') num++;char *sp=strchr(num,' ');if (sp) {*sp=0;(void)sms_send(sock,portc,num,sp+1,NULL);}}
        size_t used=(size_t)(nl+1-buf);memmove(buf,nl+1,have-used+1);have-=used;
    }
    if (have>=sizeof(buf)-1) have=0;
}

/* Signing off (A, 2026-11-15): a REGISTER with Expires 0 over the protected connection, built once we are registered
 * and sent before every exit we choose (health check failed, a text not confirmed, the ~110 min refresh, SIGTERM).
 * Without it the network kept the old registration: the P-CSCF we had been on stayed silent to the next start, and a
 * registration through the other one could take 200 OK yet carry nothing (08:17, 08:39). */
/*
 * The registration's own state, kept for refreshing it in place (B, 2026-11-15). Until now the ~110 min refresh was
 * a full restart (sign-off, new challenge, new SAs): 10-90 s without VoLTE, calls "not reachable", texts held at the
 * SMSC. Now: a REGISTER on the same protected connection and SAs, same Call-ID, next CSeq, the digest with the next nc
 * (RFC 2617: HA1 from the first challenge's RES stays valid for that nonce). If the network answers 200 the
 * registration simply runs on; if it wants a new challenge (401) or does not answer, the old fresh start follows (not
 * during a call: the current registration is still good until it expires).
 */
static struct {
    char ha1[33],ha2[33],nonce[300],cnonce[20],user[128],uri[128],common[1400],secc[900],secs[900],cid[120];
    const char *T;int port,ok;unsigned nc,cseq;
} reg;
static int reg_build(char *out,size_t cap,int expires)
{
    if (!reg.ok) return 0;
    char c0[1400],resp[33],buf[600];
    snprintf(c0,sizeof(c0),"%s",reg.common);
    if (expires==0) {char *ex=strstr(c0,"Expires: 600000\r\n");if (ex) {ex[9]='0';memmove(ex+10,ex+15,strlen(ex+15)+1);}}
    unsigned nc=++reg.nc,cseq=++reg.cseq;
    {size_t L=(size_t)snprintf(buf,sizeof(buf),"%s:%s:%08x:%s:auth:%s",reg.ha1,reg.nonce,nc,reg.cnonce,reg.ha2);md5hex(buf,L,resp);}
    int n=snprintf(out,cap,
        "REGISTER %s SIP/2.0\r\n"
        "Via: SIP/2.0/%s [%s]:%d;branch=z9hG4bK%08x;rport\r\n"
        "CSeq: %u REGISTER\r\n"
        "%s"
        "Authorization: Digest username=\"%s\",realm=\"%s\",nonce=\"%s\",uri=\"%s\",response=\"%s\",algorithm=AKAv1-MD5,qop=auth,nc=%08x,cnonce=\"%s\"\r\n"
        "Security-Client: %s\r\n"
        "Security-Verify: %s\r\n"
        "Content-Length: 0\r\n\r\n",reg.uri,reg.T,cfg.ip6,reg.port,arc4random(),cseq,c0,reg.user,cfg.realm,reg.nonce,reg.uri,resp,nc,reg.cnonce,reg.secc,reg.secs);
    return n>0 && n<(int)cap?n:0;
}
/* the network's own word on our registration (RFC 3680 reg event, TS 24.229 5.1.1.3): a NOTIFY saying our contact
 * was terminated (deactivated, rejected, expired, unregistered) means the network dropped us -- without it we noticed
 * only at the next health check (minutes, texts held meanwhile); "shortened" asks for a refresh soon */
static int reg_event_dead,reg_event_refresh,reg_refresh_failed;
/* (B, 2026-11-15) a 504 carrying 3GPP "alternative-service: restoration / initial-registration" (TS 24.229
 * 5.1.2A.1.6): the P-CSCF can no longer reach our S-CSCF ("PCSCF routing to the next hop failed") and tells us to
 * register afresh. Jio sends it now and then (08:30, 09:03, 09:28, 10:01, 10:06 on 2026-11-15) and closes the
 * connection ~3 min later; until then we looked registered but nothing reached us. Now: a fresh registration at once
 * (not during a call). It came late to the health check (after its 10 s), so any response is looked at. */
static int reg_restore;
static char reg_sub_cid[64];
static void reg_notify(const char *r)
{
    char ev[64];
    if (!hdr(r,"Event",ev,sizeof(ev)) && !hdr(r,"o",ev,sizeof(ev))) return;
    if (strncmp(ev,"reg",3) || (ev[3] && ev[3]!=';' && ev[3]!=' ')) return;
    const char *b=strstr(r,"\r\n\r\n");if (!b) return;
    char me[96];snprintf(me,sizeof(me),"[%s]:%d",cfg.ip6,reg.port);
    int mine=0,term=0,shorter=0;
    for (const char *c=b;(c=strstr(c,"<contact"));c+=8) {
        const char *e=strstr(c,"</contact>");if (!e) break;
        const char *u=strstr(c,"<uri>");
        if (!u || u>e || !memmem(u,(size_t)(e-u),me,strlen(me))) continue;
        mine=1;
        const char *st=strstr(c,"state=\"terminated\"");if (st && st<e) term=1;
        const char *sh=strstr(c,"event=\"shortened\"");if (sh && sh<e) shorter=1;
    }
    if (!mine) {const char *g=strstr(b,"<registration");if (g && strstr(g,"state=\"terminated\"")) term=1;}
    printf("%s registration event: %s\n",ts_now(),term?"the network dropped us":shorter?"shortened (refresh soon)":"active");
    if (term) reg_event_dead=1; else if (shorter) reg_event_refresh=1;
}
/* the granted lifetime of our contact in a 200 to REGISTER (expires=N on our Contact, else Expires), 0 if none */
static int reg_granted(const char *r)
{
    char me[96];snprintf(me,sizeof(me),"[%s]:%d",cfg.ip6,reg.port);
    for (const char *q=r;(q=strstr(q,"\r\nContact:"));q+=10) {
        const char *e=strstr(q+2,"\r\n");if (!e) break;
        const char *m=memmem(q,(size_t)(e-q),me,strlen(me)),*x=m?memmem(m,(size_t)(e-m),"expires=",8):NULL;
        if (x) return atoi(x+8);
    }
    char ex[32];return hdr(r,"Expires",ex,sizeof(ex))?atoi(ex):0;
}
static time_t reg_refresh_after(int granted)
{   /* half the lifetime, 10 min .. 1 h (Jio grants 7200 s) */
    int h=granted>0?granted/2:3600;return h<600?600:h>3600?3600:h;
}
static void reg_subscribe(int s,int portc)
{
    if (!sms_impu[0]) return;
    char m[3000];snprintf(reg_sub_cid,sizeof(reg_sub_cid),"%08x%08x@[%s]",arc4random(),arc4random(),cfg.ip6);
    int n=snprintf(m,sizeof(m),
        "SUBSCRIBE %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=z9hG4bK%08x;rport\r\nMax-Forwards: 70\r\n%s"
        "From: <%s>;tag=%08x\r\nTo: <%s>\r\nCall-ID: %s\r\nCSeq: 1 SUBSCRIBE\r\n"
        "Contact: <sip:%s@[%s]:%d;transport=tcp>\r\nP-Preferred-Identity: <%s>\r\n"
        "Event: reg\r\nAccept: application/reginfo+xml\r\nExpires: 600000\r\nContent-Length: 0\r\n\r\n",
        sms_impu,cfg.ip6,portc,arc4random(),sms_route,sms_impu,arc4random(),sms_impu,reg_sub_cid,cfg.imsi,cfg.ip6,reg.port,sms_impu);
    if (n>0 && n<(int)sizeof(m) && send(s,m,(size_t)n,0)==n) printf("%s registration events: subscribed\n",ts_now());
}
static char dereg_msg[4000];static int dereg_len;
static void ims_dereg(int wait)
{
    if (!dereg_len || call_sock<0) return;
    dereg_len=0;
    int n=reg_build(dereg_msg,sizeof(dereg_msg),0);if (!n) return;
    int w=(int)send(call_sock,dereg_msg,(size_t)n,0);
    if (!wait) {usleep(300000);return;}
    char r[3000];int k=0;
    if (w>0) do k=recv_msg(call_sock,r,sizeof(r),2); while (k>0 && strncmp(r,"SIP/2.0",7));
    printf("signed off: %s\n",w<=0?"could not send":k>0?(strncmp(r,"SIP/2.0 2",9)?"refused":"done"):"no answer");fflush(stdout);
}
static void ims_stop(int sig)
{   /* SIGTERM/SIGINT: our SAs and policies go with us (the next start makes its own). A call in progress is closed
     * first (A, 2026-11-14: a restart mid-call left it open on the network: every later call got "busy") */
    (void)sig;
    call_abort_all("the phone service restarted");ims_dereg(0);unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();_exit(0);
}

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    call_seq=(unsigned)(time(NULL)%1000000)*10;
    signal(SIGTERM,ims_stop);signal(SIGINT,ims_stop);signal(SIGPIPE,SIG_IGN);
    pfk=socket(PF_KEY,SOCK_RAW,PF_KEY_V2);
    if (argc>1 && !strcmp(argv[1],"--pfkey")) {printf("PF_KEY socket: %s\n",pfk>=0?"available":strerror(errno));return pfk<0;}
    if (argc>1 && !strcmp(argv[1],"--flush")) {ipsec_flush();printf("SAs and policies flushed\n");return 0;}
    if (load_cfg()) {fprintf(stderr,"redwood-ims: no IMS PDN yet (/var/tmp/redwood-cell/ims)\n");return 1;}
    {   /* one client only: an older one (e.g. left by a restarted redwood-cellulard) is stopped first -- two clients
         * flush each other's SAs and neither can send */
        FILE *q=fopen("/var/tmp/redwood-cell/ims.pid","r");int old=0;
        if (q) {if (fscanf(q,"%d",&old)!=1) old=0;fclose(q);}
        if (old>1 && old!=getpid() && !kill(old,0)) {
            printf("stopping the older client (pid %d)\n",old);kill(old,SIGTERM);
            for (int i=0;i<50 && !kill(old,0);i++) usleep(100000);
            if (!kill(old,0)) kill(old,SIGKILL);
        }
        q=fopen("/var/tmp/redwood-cell/ims.pid","w");if (q) {fprintf(q,"%d\n",getpid());fclose(q);}
    }
    if (pfk<0) {perror("PF_KEY");return 1;}
    int udp=flag(argc,argv,"--udp"),tcp=!udp,pci=atoi(opt(argc,argv,"--pcscf","0"))%cfg.npc;
    if (!opt(argc,argv,"--pcscf",NULL)) {   /* the P-CSCF that answered last time, or the next one after a silent one */
        FILE *q=fopen("/var/tmp/redwood-cell/ims-pcscf","r");int v=0;if (q) {if (fscanf(q,"%d",&v)==1 && v>=0) pci=v%cfg.npc;fclose(q);}
    }
    const char *ua=opt(argc,argv,"--ua","Samsung_SM-G980F_G980FXXSNHYB7_IMS_6.0"),*pani=opt(argc,argv,"--pani","");
    struct in6_addr me6,pc6;inet_pton(AF_INET6,cfg.ip6,&me6);inet_pton(AF_INET6,cfg.pcscf[pci],&pc6);
    ipsec_flush();
    if (flag(argc,argv,"--selftest")) {     /* the kernel's side alone: dummy keys, then removed; and the ISIM opens */
        uint8_t k[20];memset(k,0x5a,sizeof(k));
        int a1=sa_add(&me6,&pc6,0x1001,1,SADB_AALG_MD5HMAC,k,16,SADB_X_EALG_AESCBC,k,16),a2=sa_add(&pc6,&me6,0x1002,0,SADB_AALG_SHA1HMAC,k,20,SADB_EALG_NULL,NULL,0);
        int q1=sp_add(&me6,40001,&pc6,40002,IPSEC_DIR_OUTBOUND,1),q2=sp_add(&pc6,40002,&me6,40001,IPSEC_DIR_INBOUND,0);
        printf("selftest: SA md5/aes %d, SA sha1/null %d, policy out %d, policy in %d (0 = accepted)\n",a1,a2,q1,q2);
        ipsec_flush();
        int io=isim_open();printf("selftest: ISIM %s (channel %d)\n",io?"did NOT open":"opened and selected",isim_channel);isim_close();
        return a1 || a2 || q1 || q2 || io;
    }
    if (isim_open()) {fprintf(stderr,"redwood-ims: the ISIM did not open (is redwood-cellulard running?)\n");return 1;}

    /* 1. the first REGISTER, in the clear, from the port that becomes port-c */
    /* The same ports every time (A, 2026-11-14): our Contact (address + port-s) is then the same after a restart, and
     * the network replaces our registration instead of keeping the old one beside it. With old ones kept, an incoming
     * call was offered to the dead contacts first and reached us only after the caller had heard "busy". */
    struct sockaddr_in6 a={.sin6_len=sizeof(a),.sin6_family=AF_INET6,.sin6_port=htons(5060),.sin6_addr=pc6};
    /* (05:46: a quick restart found port-c still in TIME_WAIT, took a free port, and the old contact stayed at the
     * network: the next call was "busy") -- the fixed port is retried for up to 60 s before any other is taken */
    int s=-1;
    for (int tries=0;tries<30;tries++) {
        s=bound(tcp?SOCK_STREAM:SOCK_DGRAM,IMS_PORT_C);
        if (s>=0 && !connect(s,(struct sockaddr *)&a,sizeof(a))) break;
        if (tries==0 || tries==29) printf("port-c %d: %s%s\n",IMS_PORT_C,strerror(errno),tries==29?"; a free port instead":"; trying again for up to 60 s");
        if (s>=0) close(s);
        s=-1;sleep(2);
    }
    if (s<0) {
        s=bound(tcp?SOCK_STREAM:SOCK_DGRAM,0);if (s<0) {perror("bind");return 1;}
        if (connect(s,(struct sockaddr *)&a,sizeof(a))) {perror("connect");isim_close();return 1;}
    }
    struct sockaddr_in6 mine;socklen_t ml=sizeof(mine);getsockname(s,(struct sockaddr *)&mine,&ml);
    int portc=ntohs(mine.sin6_port),ports=portc+2;
    int ls=tcp?bound(SOCK_STREAM,ports):-1,us=bound(SOCK_DGRAM,ports);
    if ((tcp && (ls<0 || listen(ls,4))) || us<0) {fprintf(stderr,"redwood-ims: port-s %d is taken\n",ports);isim_close();return 1;}
    uint32_t spic=(arc4random()&0x0fffffff)|0x1000,spis=spic+1;
    char secc[900];
    if (flag(argc,argv,"--nullonly"))   /* (the first form tried: Jio's P-CSCF answers it 403 with no challenge) */
        snprintf(secc,sizeof(secc),"ipsec-3gpp;prot=esp;mod=trans;spi-c=%u;spi-s=%u;port-c=%d;port-s=%d;ealg=null;alg=hmac-md5-96",spic,spis,portc,ports);
    else {
        static const char *const algs[]={"hmac-md5-96","hmac-sha-1-96"},*const ealgs[]={"aes-cbc","null"};size_t o=0;
        for (int i=0;i<2;i++) for (int j=0;j<2;j++)
            o+=(size_t)snprintf(secc+o,sizeof(secc)-o,"%sipsec-3gpp;alg=%s;ealg=%s;spi-c=%u;spi-s=%u;port-c=%d;port-s=%d",o?", ":"",algs[i],ealgs[j],spic,spis,portc,ports);
    }
    char user[128],uri[128],common[1400],m[4000],r[6000];
    snprintf(user,sizeof(user),"%s@%s",cfg.imsi,cfg.realm);snprintf(uri,sizeof(uri),"sip:%s",cfg.realm);
    uint32_t tag=arc4random(),cid=arc4random();const char *T=tcp?"TCP":"UDP";
    snprintf(common,sizeof(common),
        "Max-Forwards: 70\r\n"
        "From: <sip:%s>;tag=%08x\r\n"
        "To: <sip:%s>\r\n"
        "Call-ID: %08x@[%s]\r\n"
        "Contact: <sip:%s@[%s]:%d;transport=%s>;+sip.instance=\"<urn:gsma:imei:%.8s-%.6s-0>\";+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\";+g.3gpp.smsip;audio\r\n"
        "%s%s%s"
        "Require: sec-agree\r\nProxy-Require: sec-agree\r\n"
        "Supported: path,sec-agree\r\n"
        "Allow: INVITE,ACK,CANCEL,BYE,OPTIONS,MESSAGE,NOTIFY\r\n"
        "User-Agent: %s\r\n"
        "Expires: 600000\r\n",
        user,tag,user,cid,cfg.ip6,cfg.imsi,cfg.ip6,ports,tcp?"tcp":"udp",cfg.imei,cfg.imei+8,
        *pani?"P-Access-Network-Info: ":"",pani,*pani?"\r\n":"",ua);
    int n=snprintf(m,sizeof(m),
        "REGISTER %s SIP/2.0\r\n"
        "Via: SIP/2.0/%s [%s]:%d;branch=z9hG4bK%08x;rport\r\n"
        "CSeq: 1 REGISTER\r\n"
        "%s"
        "Authorization: Digest username=\"%s\",realm=\"%s\",uri=\"%s\",nonce=\"\",response=\"\",algorithm=AKAv1-MD5\r\n"
        "Security-Client: %s\r\n"
        "Content-Length: 0\r\n\r\n",uri,T,cfg.ip6,portc,arc4random(),common,user,cfg.realm,uri,secc);
    printf("--> REGISTER 1 to [%s]:5060 over %s, port-c %d port-s %d\n",cfg.pcscf[pci],T,portc,ports);
    if (send(s,m,(size_t)n,0)<0) {perror("send");isim_close();return 1;}
    do n=recv_msg(s,r,sizeof(r),8); while (n>0 && !strncmp(r,"SIP/2.0 1",9));
    if (n<=0) {   /* this P-CSCF is silent: the next start (redwood-cellulard, 30 s) tries the other */
        FILE *q=fopen("/var/tmp/redwood-cell/ims-pcscf","w");if (q) {fprintf(q,"%d\n",(pci+1)%cfg.npc);fclose(q);}
        printf("no answer from P-CSCF %d; the next start tries P-CSCF %d\n",pci,(pci+1)%cfg.npc);isim_close();
        /* try the other one at once (A, 2026-11-15): waiting for redwood-cellulard's next start cost 30 s more, and
         * texts sent to us meanwhile were held at the SMSC for minutes. Once per start: --pcscf-retried stops a loop. */
        if (cfg.npc>1 && !flag(argc,argv,"--pcscf-retried") && argc<60) {
            static char idx[12];snprintf(idx,sizeof(idx),"%d",(pci+1)%cfg.npc);
            char *nv[64];int k=0;for (int i=0;i<argc;i++) {if (!strcmp(argv[i],"--pcscf") && i+1<argc) {i++;continue;} nv[k++]=argv[i];}
            nv[k++]="--pcscf";nv[k++]=idx;nv[k++]="--pcscf-retried";nv[k]=NULL;
            close(s);if (ls>=0) close(ls);close(us);if (pfk>=0) close(pfk);
            printf("trying P-CSCF %s now\n",idx);fflush(stdout);execv(argv[0],nv);
        }
        return 1;}
    printf("<-- %.*s\n",(int)(strstr(r,"\r\n")-r),r);
    if (strncmp(r,"SIP/2.0 401",11)) {show(r);isim_close();return 1;}
    char www[900],secs[900],nonce[300]={0};
    if (!hdr(r,"WWW-Authenticate",www,sizeof(www)) || !hdr(r,"Security-Server",secs,sizeof(secs))) {printf("401 without a challenge or Security-Server\n");show(r);isim_close();return 1;}
    {const char *p=strstr(www,"nonce=\"");if (p) {p+=7;const char *e=strchr(p,'"');if (e && (size_t)(e-p)<sizeof(nonce)) memcpy(nonce,p,(size_t)(e-p));}}
    uint8_t chal[64];if (b64(nonce,strlen(nonce),chal,sizeof(chal))<32) {printf("the nonce is not RAND|AUTN\n");isim_close();return 1;}
    /* the first offer the P-CSCF lists is the one it wants */
    char first[400];{const char *e=strchr(secs,',');size_t L=e?(size_t)(e-secs):strlen(secs);if (L>=sizeof(first)) L=sizeof(first)-1;memcpy(first,secs,L);first[L]=0;}
    uint32_t pspic=0,pspis=0,pportc=0,pports=0;
    if (param_u(first,"spi-c",&pspic) || param_u(first,"spi-s",&pspis) || param_u(first,"port-c",&pportc) || param_u(first,"port-s",&pports)) {printf("Security-Server not understood: %s\n",first);isim_close();return 1;}
    int aalg=strstr(first,"hmac-sha-1-96")?SADB_AALG_SHA1HMAC:SADB_AALG_MD5HMAC,ealg=strstr(first,"aes-cbc")?SADB_X_EALG_AESCBC:SADB_EALG_NULL;
    printf("    challenge received; the P-CSCF chose %s / %s, its port-c %u port-s %u\n",aalg==SADB_AALG_SHA1HMAC?"hmac-sha-1-96":"hmac-md5-96",ealg==SADB_X_EALG_AESCBC?"aes-cbc":"null",pportc,pports);
    close(s);

    /* 2. the SIM answers the challenge */
    uint8_t res[16],ck[16],ik[20]={0};int resl=0;
    int e=isim_aka(chal,chal+16,res,&resl,ck,ik);isim_close();
    if (e) {printf("AKA: %s\n",e==1?"the SIM reports a sequence (SQN) failure: resynchronisation is not written yet":"failed");return 1;}
    printf("    the SIM answered (RES %d bytes)\n",resl);

    /* 3. four ESP transport SAs and the policies that send the two port pairs through them (TS 33.203 7.1) */
    int akl=aalg==SADB_AALG_SHA1HMAC?20:16,ekl=ealg==SADB_X_EALG_AESCBC?16:0;
    int e1=sa_add(&me6,&pc6,pspis,1,aalg,ik,akl,ealg,ck,ekl);        /* UE port-c -> P port-s */
    int e2=sa_add(&pc6,&me6,spic,0,aalg,ik,akl,ealg,ck,ekl);         /* P port-s  -> UE port-c */
    int e3=sa_add(&pc6,&me6,spis,0,aalg,ik,akl,ealg,ck,ekl);         /* P port-c  -> UE port-s */
    int e4=sa_add(&me6,&pc6,pspic,2,aalg,ik,akl,ealg,ck,ekl);        /* UE port-s -> P port-c */
    int p1=sp_add(&me6,portc,&pc6,(int)pports,IPSEC_DIR_OUTBOUND,1),p2=sp_add(&me6,ports,&pc6,(int)pportc,IPSEC_DIR_OUTBOUND,2);
    int p3=sp_add(&pc6,(int)pports,&me6,portc,IPSEC_DIR_INBOUND,0),p4=sp_add(&pc6,(int)pportc,&me6,ports,IPSEC_DIR_INBOUND,0);
    memset(ck,0,sizeof(ck));memset(ik,0,sizeof(ik));
    printf("    IPsec: SAs %d %d %d %d, policies %d %d %d %d (0 = accepted)\n",e1,e2,e3,e4,p1,p2,p3,p4);
    if (e1 || e2 || e3 || e4 || p1 || p2 || p3 || p4) {ipsec_flush();
        /* The two outbound SAs refused (EINVAL, "SAs 22 0 0 22"; seen after quick restarts): flushing and adding the
         * same ones again never helped (3 of 3), but the next start's fresh challenge (new SPIs) always did. So a new
         * challenge at once: re-exec once (--sa-retried), instead of redwood-cellulard's next start 30 s later. */
        if (!flag(argc,argv,"--sa-retried") && argc<60) {
            char *nv[64];int k=0;for (int i=0;i<argc;i++) nv[k++]=argv[i];nv[k++]="--sa-retried";nv[k]=NULL;
            close(s);if (ls>=0) close(ls);close(us);if (pfk>=0) close(pfk);isim_close();
            printf("    a new challenge at once\n");fflush(stdout);usleep(300000);execv(argv[0],nv);
        }
        return 1;}

    /* 4. the second REGISTER, protected: port-c -> the P-CSCF's port-s */
    char ha1[33],ha2[33],resp[33],buf[600],cnonce[20];
    {size_t L=(size_t)snprintf(buf,sizeof(buf),"%s:%s:",user,cfg.realm);memcpy(buf+L,res,(size_t)resl);md5hex(buf,L+(size_t)resl,ha1);memset(res,0,sizeof(res));}
    {size_t L=(size_t)snprintf(buf,sizeof(buf),"REGISTER:%s",uri);md5hex(buf,L,ha2);}
    snprintf(cnonce,sizeof(cnonce),"%08x%08x",arc4random(),arc4random());
    {size_t L=(size_t)snprintf(buf,sizeof(buf),"%s:%s:00000001:%s:auth:%s",ha1,nonce,cnonce,ha2);md5hex(buf,L,resp);}
    /* (kept for the in-place refresh and the sign-off: reg_build) */
    memcpy(reg.ha1,ha1,33);memcpy(reg.ha2,ha2,33);snprintf(reg.nonce,sizeof(reg.nonce),"%s",nonce);snprintf(reg.cnonce,sizeof(reg.cnonce),"%s",cnonce);
    snprintf(reg.user,sizeof(reg.user),"%s",user);snprintf(reg.uri,sizeof(reg.uri),"%s",uri);snprintf(reg.common,sizeof(reg.common),"%s",common);
    snprintf(reg.secc,sizeof(reg.secc),"%s",secc);reg.T=T;reg.port=ports;reg.nc=1;reg.cseq=2;
    snprintf(reg.cid,sizeof(reg.cid),"%08x@[%s]",cid,cfg.ip6);
    memset(ha1,0,sizeof(ha1));
    /* our previous contact, if it was another (address or port changed): removed in this same REGISTER (expires=0),
     * so the network never offers a call to a dead contact first ("busy", late rings) */
    char my_contact[200],stale_contact[300]="";
    snprintf(my_contact,sizeof(my_contact),"<sip:%s@[%s]:%d;transport=%s>",cfg.imsi,cfg.ip6,ports,tcp?"tcp":"udp");
    {FILE *q=fopen("/var/root/ims-last-contact","r");char old[200]="";
     if (q) {if (!fgets(old,sizeof(old),q)) old[0]=0;fclose(q);old[strcspn(old,"\r\n")]=0;}
     if (old[0] && strcmp(old,my_contact)) {snprintf(stale_contact,sizeof(stale_contact),"Contact: %s;expires=0\r\n",old);printf("    the previous contact is removed in this REGISTER\n");}}
    s=bound(tcp?SOCK_STREAM:SOCK_DGRAM,portc);if (s<0) {perror("bind port-c");ipsec_flush();return 1;}
    a.sin6_port=htons((uint16_t)pports);
    if (connect(s,(struct sockaddr *)&a,sizeof(a))) {perror("connect (protected)");ipsec_flush();return 1;}
    {int on=1,idle=30,intvl=10,cnt=3;setsockopt(s,SOL_SOCKET,SO_KEEPALIVE,&on,sizeof(on));
     setsockopt(s,IPPROTO_TCP,TCP_KEEPALIVE,&idle,sizeof(idle));setsockopt(s,IPPROTO_TCP,TCP_KEEPINTVL,&intvl,sizeof(intvl));
     setsockopt(s,IPPROTO_TCP,TCP_KEEPCNT,&cnt,sizeof(cnt));}
    n=snprintf(m,sizeof(m),
        "REGISTER %s SIP/2.0\r\n"
        "Via: SIP/2.0/%s [%s]:%d;branch=z9hG4bK%08x;rport\r\n"
        "CSeq: 2 REGISTER\r\n"
        "%s"
        "Authorization: Digest username=\"%s\",realm=\"%s\",nonce=\"%s\",uri=\"%s\",response=\"%s\",algorithm=AKAv1-MD5,qop=auth,nc=00000001,cnonce=\"%s\"\r\n"
        "Security-Client: %s\r\n"
        "Security-Verify: %s\r\n"
        "%s"
        "Content-Length: 0\r\n\r\n",uri,T,cfg.ip6,ports,arc4random(),common,user,cfg.realm,nonce,uri,resp,cnonce,secc,secs,stale_contact);
    printf("--> REGISTER 2 (protected) to port %u\n",pports);
    if (send(s,m,(size_t)n,0)<0) {perror("send (protected)");ipsec_flush();return 1;}
    do n=recv_msg(s,r,sizeof(r),10); while (n>0 && !strncmp(r,"SIP/2.0 1",9));
    if (n<=0) {printf("no answer to the protected REGISTER (the SAs stay for a look: redwood-ims --flush removes them)\n");return 1;}
    printf("<-- %.*s\n",(int)(strstr(r,"\r\n")-r),r);show(r);
    if (strncmp(r,"SIP/2.0 200",11)) {ipsec_flush();return 1;}
    printf("REGISTERED\n");
    /* the registration is refreshable from here on (reg_build: refresh in place, sign-off). (The sign-off of 08:53 was
     * prebuilt with REGISTER 2's digest response under nc=00000002 -- a wrong digest, so it was refused and the old
     * registration lingered; it is now built when sent, with its own nc.) */
    snprintf(reg.secs,sizeof(reg.secs),"%s",secs);reg.ok=1;dereg_len=1;
    /* the next start goes to the OTHER P-CSCF first: the one we are on still holds this connection after we leave and
     * stays silent to a new REGISTER from the same port-c for a while (seen at every restart 2026-11-15: 8 s lost) */
    if (cfg.npc>1) {FILE *q=fopen("/var/tmp/redwood-cell/ims-pcscf","w");if (q) {fprintf(q,"%d\n",(pci+1)%cfg.npc);fclose(q);}}
    {FILE *q=fopen("/var/root/ims-last-contact","w");if (q) {fprintf(q,"%s\n",my_contact);fclose(q);chmod("/var/root/ims-last-contact",0600);}}
    {FILE *q=fopen("/var/tmp/redwood-cell/ims-registered","w");if (q) {fchmod(fileno(q),0644);fprintf(q,"%lld\n",(long long)time(NULL));fclose(q);}}
    sms_keep_registration(r);
    if (flag(argc,argv,"--once")) return 0;

    /* 5. stay: show what the network sends, answer requests with 200 */
    int cs[8];int ncs=0;time_t registered_at=time(NULL),reopened_at=0;
    int granted=reg_granted(r);time_t refresh_at=registered_at+reg_refresh_after(granted),rereg_sent=0;unsigned rereg_cseq=0;int subscribed=0;
    printf("registration lifetime %d s: refreshed in place after %ld s\n",granted,(long)(refresh_at-registered_at));
    /* Health check (A, 2026-11-15): an OPTIONS to our own identity, 15 s after registering and then every 2 min when
     * idle. It goes out through the P-CSCF and S-CSCF and comes back, so any answer (even an error) proves the
     * registration carries traffic. No answer in 10 s = a dead registration that only looks alive (texts to us are
     * then held at the SMSC for minutes): start afresh. */
    /* Only a 2xx counts: it is our own 200 coming back. 408/480/504 = the network can send for us but not reach us
     * (08:28:55: 504 while a text to us waited at the SMSC until the next registration, 3.5 min later). */
    /* (08:42-08:45: checks 15 s after registering got no answer although the connection worked -- the sign-off was
     * answered -- and each failure restarted the client: a loop. So: first check after 60 s, two failures in a row
     * (30 s apart) before acting, and at most one restart from the check per 15 min, remembered across restarts.) */
    uint32_t probe_cid=0;int probe_dead=0,probe_fails=0;time_t probe_at=0,next_probe=registered_at+60;
    snprintf(call_ip6,sizeof(call_ip6),"%s",cfg.ip6);call_ports=ports;call_user=cfg.imsi;call_sock=s;call_portc=portc;
    call_registered=1;calls_snapshot(1);   /* (a fresh start: the call screen ends any call it still shows from before) */
    for (;;) {
        /* a fresh registration well before the 7200 s expiry: leave cleanly, redwood-cellulard starts us again */
        {   /* the in-place refresh (reg_build); no answer in 15 s or anything but 2xx -> the fresh start below, when no
             * call is up (with a call: the current registration still holds until it expires; tried again in 5 min) */
            time_t now=time(NULL);
            if (!subscribed && now-registered_at>=3 && sms_impu[0]) {reg_subscribe(s,portc);subscribed=1;}
            if (reg_event_refresh) {reg_event_refresh=0;if (refresh_at>now+5) refresh_at=now+5;}
            if (!rereg_sent && now>=refresh_at) {
                char rm[4000];int rn=reg_build(rm,sizeof(rm),600000);
                if (rn && send(s,rm,(size_t)rn,0)==rn) {rereg_sent=now;rereg_cseq=reg.cseq;printf("%s registration: refreshing in place\n",ts_now());}
                else refresh_at=now+60;
            }
            int failed=0;
            if (rereg_sent && now-rereg_sent>15) {printf("%s registration refresh: no answer\n",ts_now());rereg_sent=0;failed=1;}
            if (reg_refresh_failed) {reg_refresh_failed=0;failed=1;}
            if (reg_event_dead) {reg_event_dead=0;failed=1;}
            if (reg_restore) {reg_restore=0;failed=1;}
            if (failed) {
                if (calls_live()) {printf("    (a call is up: the fresh registration waits)\n");refresh_at=now+300;}
                else {printf("a fresh registration\n");fflush(stdout);unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;}
            }
        }
        if (time(NULL)-registered_at>6600 && !calls_live()) {printf("re-registration due: leaving for a fresh start\n");ims_dereg(1);unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;}
        struct pollfd p[12];int k=0;p[k++]=(struct pollfd){s,POLLIN,0};p[k++]=(struct pollfd){us,POLLIN,0};if (ls>=0) p[k++]=(struct pollfd){ls,POLLIN,0};
        int base=k;for (int i=0;i<ncs;i++) p[k++]=(struct pollfd){cs[i],POLLIN,0};
        {int pend=0;for (int j=0;j<k;j++) if (j!=1 && j!=2 && recv_pending(p[j].fd)) pend=1;
         /* the outbox folder as one more poll entry (after k: the loop below never sees it): a text from Messages
          * goes out the moment its file lands, not at the next 1 s tick (A, 2026-11-15) */
         static int okq=-2,odfd=-1;
         if (okq==-2) {okq=kqueue();int dfd=okq>=0?open("/var/tmp/redwood-cell/sms-out",O_EVTONLY):-1;
             if (dfd<0) {if (okq>=0) close(okq);okq=-2;}   /* (no folder yet: sms_outbox makes it; try again next turn) */
             else {struct kevent ev;EV_SET(&ev,dfd,EVFILT_VNODE,EV_ADD|EV_CLEAR,NOTE_WRITE|NOTE_EXTEND|NOTE_DELETE|NOTE_RENAME,0,NULL);
                 if (kevent(okq,&ev,1,NULL,0,NULL)<0) {close(dfd);close(okq);okq=-1;} else {odfd=dfd;sms_out_kick=1;}}}
         sms_out_watched=okq>=0;
         int kn=k;if (okq>=0) p[kn++]=(struct pollfd){okq,POLLIN,0};
         if (poll(p,(nfds_t)kn,pend?0:calls_live()?200:1000)<0) break;
         if (okq>=0 && (p[k].revents&POLLIN)) {struct kevent ev;struct timespec z={0,0};int gone=0;
             while (kevent(okq,NULL,0,&ev,1,&z)>0) {sms_out_kick=1;if (ev.fflags&(NOTE_DELETE|NOTE_RENAME)) gone=1;}
             if (gone) {close(okq);close(odfd);odfd=-1;okq=-2;}}}   /* (the folder was replaced: watch the new one next turn) */
        {   /* keep the registration's connection open: a CRLF CRLF ping every 25 s (RFC 5626 3.5.1). Without it the
             * network dropped the idle TCP connection within minutes, and a text sent into it got no answer at all
             * ("send: failed", 2026-11-13 18:43 and 18:46). */
            static time_t ka;time_t t=time(NULL);
            if (t-ka>=25) {ka=t;if (send(s,"\r\n\r\n",4,0)<0) {printf("keep-alive: the registration connection is gone (%s)\n",strerror(errno));
                int ns=-1;   /* (twice within a minute: something is really wrong; start afresh) */
                if (tcp && time(NULL)-reopened_at>=60 && (ns=reg_reopen(s,&a,portc))>=0) {reopened_at=time(NULL);s=ns;call_sock=s;next_probe=time(NULL)+3;probe_cid=0;printf("%s registration connection opened again (registration kept)\n",ts_now());}
                else {printf("a fresh registration\n");call_abort_all("connection lost");unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;}}}
            /* (2026-11-14) the P-CSCF's own connections to our port-s, too: a dead one is closed at once, so the next
             * call comes on a fresh connection instead of waiting out a dead one (late rings, lost CANCELs) */
            static time_t kc;
            if (t-kc>=20) {kc=t;
                for (int j=0;j<ncs;j++) if (send(cs[j],"\r\n\r\n",4,0)<0) {
                    printf("keep-alive: a P-CSCF connection is gone (%s): closed\n",strerror(errno));
                    call_fd_closed(cs[j]);recv_forget(cs[j]);
                    close(cs[j]);cs[j]=cs[--ncs];j--;
                }
            }
        }
        ims_cmd_poll(s,portc);
        {time_t now=time(NULL);int dead=0;
         int failed=0;
         if (probe_dead) {failed=1;probe_dead=0;}
         /* (no answer at all is NOT a failure: on Jio the OPTIONS to ourselves often goes unanswered on a registration
          *  that works -- a call went through at 08:48 during such a check. Only an explicit error counts.) */
         else if (probe_cid && now-probe_at>10) {printf("%s health check: no answer (not counted)\n",ts_now());probe_cid=0;}
         if (failed && ++probe_fails<2) {next_probe=now+30;printf("    (checking again in 30 s)\n");}
         else if (failed) {
            probe_fails=0;time_t lastr=0;FILE *q=fopen("/var/tmp/redwood-cell/ims-health-restart","r");
            if (q) {long long v=0;if (fscanf(q,"%lld",&v)==1) lastr=(time_t)v;fclose(q);}
            if (now-lastr<900) printf("    (a restart from the check %lld s ago: not again yet)\n",(long long)(now-lastr));
            else {q=fopen("/var/tmp/redwood-cell/ims-health-restart","w");if (q) {fprintf(q,"%lld\n",(long long)now);fclose(q);}dead=1;}
         }
         else if (!probe_cid && now>=next_probe && sms_impu[0] && !calls_live()) {
            probe_cid=arc4random()|1;probe_at=now;if (now>=next_probe) next_probe=now+120;
            char pm[2400];int po=snprintf(pm,sizeof(pm),
                "OPTIONS %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=z9hG4bK%08x;rport\r\nMax-Forwards: 70\r\n%s"
                "From: <%s>;tag=%08x\r\nTo: <%s>\r\nCall-ID: %08x@[%s]\r\nCSeq: 1 OPTIONS\r\nContent-Length: 0\r\n\r\n",
                sms_impu,cfg.ip6,portc,arc4random(),sms_route,sms_impu,arc4random(),sms_impu,probe_cid,cfg.ip6);
            if (po>0 && po<(int)sizeof(pm) && send(s,pm,(size_t)po,0)<0) {printf("%s health check: cannot send (%s)\n",ts_now(),strerror(errno));dead=1;probe_cid=0;}
         }
         if (sms_conn_dead) {dead=1;sms_conn_dead=0;}
         if (dead) {
            if (calls_live()) printf("    (a call is up: kept for now)\n");
            else {printf("the registration carries nothing: a fresh registration\n");fflush(stdout);ims_dereg(1);
                unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;}
         }}
        call_tick();
        if (ls>=0 && (p[2].revents&POLLIN) && ncs<8) {int c=accept(ls,NULL,NULL);if (c>=0) {cs[ncs++]=c;printf("the P-CSCF connected to port-s\n");}}
        for (int i=0;i<k;i++) {
            if (i!=1 && i!=2 && !(p[i].revents&(POLLIN|POLLHUP)) && recv_pending(p[i].fd)) p[i].revents|=POLLIN;   /* kept messages */
            if (!(p[i].revents&(POLLIN|POLLHUP)) || (ls>=0 && i==2)) continue;
            struct sockaddr_in6 from;socklen_t fl=sizeof(from);ssize_t g;
            if (i==1) {g=recvfrom(us,r,sizeof(r)-1,0,(struct sockaddr *)&from,&fl);if (g>0) r[g]=0;}
            else g=recv_msg(p[i].fd,r,sizeof(r),2);
            if (g<=0) {if (i>=base) {call_fd_closed(cs[i-base]);recv_forget(cs[i-base]);close(cs[i-base]);cs[i-base]=cs[--ncs];} else if (i==0) {printf("%s the registration connection closed\n",ts_now());
                int ns=-1;
                if (tcp && time(NULL)-reopened_at>=60 && (ns=reg_reopen(s,&a,portc))>=0) {reopened_at=time(NULL);s=ns;call_sock=s;next_probe=time(NULL)+3;probe_cid=0;printf("%s registration connection opened again (registration kept)\n",ts_now());}
                else {call_abort_all("connection lost");unlink("/var/tmp/redwood-cell/ims-registered");ipsec_flush();return 0;}} break;}
            printf("%s <-- %zd bytes\n",ts_now(),g);show(r);printf("\n");
            if (strncmp(r,"SIP/2.0",7) && !strncmp(r,"INVITE ",7) && i!=1 && call_invite(p[i].fd,r)) continue;
            if (!strncmp(r,"SIP/2.0",7) && atoi(r+8)==504 && strstr(r,"<type>restoration</type>") && strstr(r,"initial-registration")) {
                if (!reg_restore) printf("%s the network asks for a fresh registration (504 restoration)\n",ts_now());
                reg_restore=1;
            }
            if (!strncmp(r,"SIP/2.0",7)) {   /* our refresh REGISTER's / SUBSCRIBE's answer */
                char rci[200],rcq[80];
                if (hdr(r,"Call-ID",rci,sizeof(rci)) && hdr(r,"CSeq",rcq,sizeof(rcq))) {
                    if (rereg_sent && !strcmp(rci,reg.cid) && strstr(rcq,"REGISTER") && (unsigned)atoi(rcq)==rereg_cseq) {
                        int code=atoi(r+8);if (code<200) continue;
                        rereg_sent=0;
                        if (code<300) {
                            registered_at=time(NULL);granted=reg_granted(r);refresh_at=registered_at+reg_refresh_after(granted);
                            {FILE *q=fopen("/var/tmp/redwood-cell/ims-registered","w");if (q) {fchmod(fileno(q),0644);fprintf(q,"%lld\n",(long long)registered_at);fclose(q);}}
                            printf("%s registration refreshed in place (lifetime %d s, next in %ld s)\n",ts_now(),granted,(long)(refresh_at-registered_at));
                            subscribed=0;   /* (the subscription is renewed with it) */
                        } else {printf("%s registration refresh: %d%s\n",ts_now(),code,code==401?" (the network wants a new challenge)":"");reg_refresh_failed=1;}
                        continue;
                    }
                    if (reg_sub_cid[0] && !strcmp(rci,reg_sub_cid) && strstr(rcq,"SUBSCRIBE")) {
                        int code=atoi(r+8);if (code>=200) printf("%s registration events: %d\n",ts_now(),code);continue;
                    }
                }
            }
            if (!strncmp(r,"SIP/2.0",7) && probe_cid) {char pci_[200];   /* the health check's answer */
                if (hdr(r,"Call-ID",pci_,sizeof(pci_)) && (uint32_t)strtoul(pci_,NULL,16)==probe_cid) {int code=atoi(r+8);
                    if (code==408 || code==480 || code==503 || code==504) {printf("%s health check: %d -- the network cannot reach us\n",ts_now(),code);probe_dead=1;}
                    if (code>=200 && code<300) probe_fails=0;
                    if (code>=200) probe_cid=0;
                    continue;}}
            if (!strncmp(r,"SIP/2.0",7) && sms_ack_answer(s,r,portc)) continue;
            if (!strncmp(r,"SIP/2.0",7) && call_response(r)) continue;
            if (strncmp(r,"SIP/2.0",7) && i!=1 && call_request(p[i].fd,r)) continue;
            if (strncmp(r,"SIP/2.0",7)) {      /* a request: 200 with its Via/From/To/Call-ID/CSeq */
                /* An INVITE (a call to us) must NOT get a bare 200: the caller would be connected to silence, and
                 * billed. Until calls exist: 100 Trying, the request saved (root-only, for working out the call
                 * flow), then 486 Busy Here. The ACK for the 486 needs no answer. (2026-11-13) */
                int invite=!strncmp(r,"INVITE ",7);
                const char *status=invite?"486 Busy Here":"200 OK";
                if (invite) {
                    char fn[96];snprintf(fn,sizeof(fn),"/var/root/ims-invite-%ld.txt",(long)time(NULL));
                    int fd=open(fn,O_WRONLY|O_CREAT|O_TRUNC,0600);if (fd>=0) {(void)write(fd,r,(size_t)g);close(fd);}
                    printf("INCOMING CALL: saved %s, answered busy (calls not implemented yet)\n",fn);
                }
                char via[900],fr[300],to[300],ci[200],cq[80];int o=0;
                if (invite) {
                    o=snprintf(m,sizeof(m),"SIP/2.0 100 Trying\r\n");
                    const char *q=r;while ((q=strstr(q,"\r\nVia:"))) {const char *e2=strstr(q+2,"\r\n");if (!e2) break;MSG_CAT(m,o,"%.*s\r\n",(int)(e2-q-2),q+2);q=e2;}
                    if (hdr(r,"From",fr,sizeof(fr))) MSG_CAT(m,o,"From: %s\r\n",fr);
                    if (hdr(r,"To",to,sizeof(to))) MSG_CAT(m,o,"To: %s\r\n",to);
                    if (hdr(r,"Call-ID",ci,sizeof(ci))) MSG_CAT(m,o,"Call-ID: %s\r\n",ci);
                    if (hdr(r,"CSeq",cq,sizeof(cq))) MSG_CAT(m,o,"CSeq: %s\r\n",cq);
                    MSG_CAT(m,o,"Content-Length: 0\r\n\r\n");
                    if (MSG_FITS(m,o)) {if (i==1) sendto(us,m,(size_t)o,0,(struct sockaddr *)&from,fl); else send(p[i].fd,m,(size_t)o,0);}
                }
                o=snprintf(m,sizeof(m),"SIP/2.0 %s\r\n",status);
                const char *q=r;while ((q=strstr(q,"\r\nVia:"))) {const char *e2=strstr(q+2,"\r\n");if (!e2) break;MSG_CAT(m,o,"%.*s\r\n",(int)(e2-q-2),q+2);q=e2;}
                (void)via;
                if (hdr(r,"From",fr,sizeof(fr))) MSG_CAT(m,o,"From: %s\r\n",fr);
                if (hdr(r,"To",to,sizeof(to))) MSG_CAT(m,o,"To: %s%s\r\n",to,strstr(to,"tag=")?"":";tag=rw1");
                (void)status;
                if (hdr(r,"Call-ID",ci,sizeof(ci))) MSG_CAT(m,o,"Call-ID: %s\r\n",ci);
                if (hdr(r,"CSeq",cq,sizeof(cq))) MSG_CAT(m,o,"CSeq: %s\r\n",cq);
                MSG_CAT(m,o,"Content-Length: 0\r\n\r\n");
                if (strncmp(r,"ACK",3)) {if (MSG_FITS(m,o)) {if (i==1) sendto(us,m,(size_t)o,0,(struct sockaddr *)&from,fl); else send(p[i].fd,m,(size_t)o,0);}}
                if (!strncmp(r,"NOTIFY ",7)) reg_notify(r);
                sms_incoming(s,r,(size_t)g,portc);
            }
        }
    }
    return 0;
}

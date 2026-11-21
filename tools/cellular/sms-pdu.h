/* MIT. SMS TPDU text decoding (GSM 7-bit, UCS-2) shared by redwood-cellulard and redwood-ims. */
#ifndef REDWOOD_SMS_PDU_H
#define REDWOOD_SMS_PDU_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static size_t gsm7_utf8(const uint8_t *ud,unsigned septets,unsigned skip_bits,char *o,size_t cap)
{
    static const char *const t[128]={"@","£","$","¥","è","é","ù","ì","ò","Ç","\n","Ø","ø","\r","Å","å","Δ","_","Φ","Γ","Λ","Ω","Π","Ψ","Σ","Θ","Ξ","","Æ","æ","ß","É",
        " ","!","\"","#","¤","%","&","'","(",")","*","+",",","-",".","/","0","1","2","3","4","5","6","7","8","9",":",";","<","=",">","?",
        "¡","A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z","Ä","Ö","Ñ","Ü","§",
        "¿","a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z","ä","ö","ñ","ü","à"};
    size_t n=0;int esc=0;
    for (unsigned k=0;k<septets;k++) {
        unsigned bit=skip_bits+7*k,c=((ud[bit/8]|(ud[bit/8+1]<<8))>>(bit%8))&0x7f;
        const char *s;char one[2]={0};
        if (esc) {esc=0;s=c==0x14?"^":c==0x28?"{":c==0x29?"}":c==0x2f?"\\":c==0x3c?"[":c==0x3d?"~":c==0x3e?"]":c==0x40?"|":c==0x65?"€":" ";}
        else if (c==0x1b) {esc=1;continue;} else s=t[c];
        if (!*s) {one[0]='?';s=one;}
        size_t L=strlen(s);if (n+L+1>cap) break;memcpy(o+n,s,L);n+=L;
    }
    o[n]=0;return n;
}
static size_t ucs2_utf8(const uint8_t *u,unsigned bytes,char *o,size_t cap)
{
    size_t n=0;
    for (unsigned k=0;k+1<bytes;k+=2) {
        unsigned c=(u[k]<<8)|u[k+1];
        if (c>=0xd800 && c<0xdc00 && k+3<bytes) {unsigned c2=(u[k+2]<<8)|u[k+3];c=0x10000+((c-0xd800)<<10)+(c2-0xdc00);k+=2;}
        if (n+5>cap) break;
        if (c<0x80) o[n++]=(char)c;
        else if (c<0x800) {o[n++]=(char)(0xc0|c>>6);o[n++]=(char)(0x80|(c&0x3f));}
        else if (c<0x10000) {o[n++]=(char)(0xe0|c>>12);o[n++]=(char)(0x80|((c>>6)&0x3f));o[n++]=(char)(0x80|(c&0x3f));}
        else {o[n++]=(char)(0xf0|c>>18);o[n++]=(char)(0x80|((c>>12)&0x3f));o[n++]=(char)(0x80|((c>>6)&0x3f));o[n++]=(char)(0x80|(c&0x3f));}
    }
    o[n]=0;return n;
}
static int sms_decode(const uint8_t *p,unsigned n,char *from,size_t fcap,char *text,size_t tcap,char *when,size_t wcap)
{
    unsigned i=0;if (n<1) return -1;i=1+p[0];if (i+2>n) return -1;
    unsigned fo=p[i++];if ((fo&3)!=0) return -1;                 /* SMS-DELIVER only */
    unsigned ol=p[i++],toa=p[i++],ob=(ol+1)/2;if (i+ob+9>n) return -1;
    if ((toa&0x70)==0x50) gsm7_utf8(p+i,ol*4/7,0,from,fcap);
    else {size_t k=0;if ((toa&0x70)==0x10 && k+1<fcap) from[k++]='+';
        for (unsigned d=0;d<ol && k+1<fcap;d++) {unsigned v=(p[i+d/2]>>((d&1)*4))&15;from[k++]=v<10?(char)('0'+v):'?';} from[k]=0;}
    i+=ob;i++;unsigned dcs=p[i++];const uint8_t *ts=p+i;i+=7;
    #define SO(b) (((b)&15)*10+((b)>>4))
    snprintf(when,wcap,"20%02u-%02u-%02u %02u:%02u:%02u",SO(ts[0]),SO(ts[1]),SO(ts[2]),SO(ts[3]),SO(ts[4]),SO(ts[5]));
    unsigned udl=p[i++];const uint8_t *ud=p+i;unsigned left=n-i,udh=(fo&0x40)&&left?ud[0]+1u:0;
    int enc=((dcs&0xc0)==0)?((dcs>>2)&3):((dcs&0xf0)==0xf0?((dcs&4)?1:0):((dcs&0xf0)==0xe0?2:0));  /* 0 gsm7 1 8bit 2 ucs2 */
    if (enc==2) ucs2_utf8(ud+udh,udl>udh?udl-udh:0,text,tcap);
    else if (enc==1) {unsigned L=udl>udh?udl-udh:0;if (L>tcap-1) L=(unsigned)tcap-1;for (unsigned k=0;k<L;k++) text[k]=(char)(ud[udh+k]>=32&&ud[udh+k]<127?ud[udh+k]:'.');text[L]=0;}
    else {unsigned skip=udh*8,fill=(7-(skip%7))%7;unsigned sk=(skip+fill)/7;if ((udl*7+7)/8>left) return -1;gsm7_utf8(ud,udl>sk?udl-sk:0,skip+fill,text,tcap);}
    return 0;
}
#endif

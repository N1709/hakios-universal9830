/* Input gate for the temporary native NMEA lab bridge; no device I/O. */
#ifndef GNSS_NMEA_LAB_GATE_H
#define GNSS_NMEA_LAB_GATE_H
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
static inline bool gnss_nmea_checksum(const char *s)
{
    size_t n=strlen(s);if(n<10 || n>256 || s[0]!='$' || s[n-3]!='*')return false;
    if(!isxdigit((unsigned char)s[n-2]) || !isxdigit((unsigned char)s[n-1]))return false;
    unsigned expected;char tail;
    if(sscanf(s+n-2,"%2x%c",&expected,&tail)!=1)return false;
    unsigned sum=0;for(size_t i=1;i<n-3;i++){if(s[i]<32 || s[i]>126)return false;sum^=(unsigned char)s[i];}
    return sum==expected;
}
static inline bool gnss_nmea_live_epoch(const char *s,const char *gga,time_t now)
{
    char copy[257];strlcpy(copy,s,sizeof(copy));
    char *f[20],*p=copy;unsigned n=0;
    while(n<20 && (f[n]=strsep(&p,",")))n++;
    if(n<10 || strcmp(f[2],"A") || strlen(f[1])<6 || strlen(f[9])!=6 || strncmp(f[1],gga+7,6))return false;
    if(n>12 && f[12][0] && strchr("ADFP",f[12][0])==NULL)return false;
    char quality[257];strlcpy(quality,gga,sizeof(quality));
    char *q=quality,*value=NULL;
    for(unsigned i=0;i<=6;i++)if(!(value=strsep(&q,",")))return false;
    if(strlen(value)!=1 || value[0]<'1' || value[0]>'5')return false;
    char stamp[13];memcpy(stamp,f[9],6);memcpy(stamp+6,f[1],6);stamp[12]=0;
    for(unsigned i=0;i<12;i++)if(stamp[i]<'0' || stamp[i]>'9')return false;
    struct tm tm={0};if(!strptime(stamp,"%d%m%y%H%M%S",&tm))return false;
    time_t t=timegm(&tm);return t>0 && fabs(difftime(now,t))<=3;
}
#endif

#include "nmea_lab_gate.h"
#include <assert.h>

int main(void)
{
    const time_t now=1794806100; /* 2026-11-16 05:15:00 UTC */
    const char *gga="$GPGGA,051500.00,0000.0000,N,00000.0000,E,1,07,1.2,0.0,M,0.0,M,,";
    const char *rmc="$GPRMC,051500.00,A,0000.0000,N,00000.0000,E,0.0,0.0,161126,,,A";
    assert(gnss_nmea_live_epoch(rmc,gga,now));
    assert(!gnss_nmea_live_epoch(rmc,gga,now+4));
    assert(!gnss_nmea_live_epoch(rmc,gga,now-4));
    char changed[257];strlcpy(changed,rmc,sizeof(changed));
    strstr(changed,",A,")[1]='V'; /* invalid receiver status */
    assert(!gnss_nmea_live_epoch(changed,gga,now));
    strlcpy(changed,gga,sizeof(changed));
    char *quality=strstr(changed,",1,07");assert(quality);quality[1]='6';
    assert(!gnss_nmea_live_epoch(rmc,changed,now)); /* dead reckoning */
    quality[1]='0';assert(!gnss_nmea_live_epoch(rmc,changed,now));
    strlcpy(changed,rmc,sizeof(changed));changed[strlen(changed)-1]='E';
    assert(!gnss_nmea_live_epoch(changed,gga,now)); /* estimated mode */
    strlcpy(changed,rmc,sizeof(changed));changed[12]='1';
    assert(!gnss_nmea_live_epoch(changed,gga,now)); /* mismatched epoch */
    assert(gnss_nmea_checksum("$GPGSV,2,1,02,27,,,37*79")); /* live capture */
    assert(!gnss_nmea_checksum("$GPGSV,2,1,02,27,,,37*78"));
    assert(!gnss_nmea_checksum("$GPGSV,2,1,02,27,,,37* 9"));
    puts("NMEA gate: live epoch, freshness, invalid/estimated fixes and checksum checks passed");
}

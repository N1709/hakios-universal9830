/* MIT. ICMPv6 echo of a given payload size over one interface: finds uplink size limits. */
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef IPV6_BOUND_IF
#define IPV6_BOUND_IF 125
#endif
int main(int argc,char **argv)
{
    if (argc<4) {fprintf(stderr,"usage: cell-ping IF ADDR6 SIZE...\n");return 2;}
    unsigned idx=if_nametoindex(argv[1]);int s=socket(AF_INET6,SOCK_RAW,IPPROTO_ICMPV6);
    setsockopt(s,IPPROTO_IPV6,IPV6_BOUND_IF,&idx,sizeof(idx));
    struct timeval tv={3,0};setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    struct sockaddr_in6 a={.sin6_len=sizeof(a),.sin6_family=AF_INET6};inet_pton(AF_INET6,argv[2],&a.sin6_addr);
    for (int k=3;k<argc;k++) {
        int n=atoi(argv[k]);uint8_t b[2000];if (n>1900) n=1900;memset(b,0xa5,sizeof(b));
        struct icmp6_hdr *h=(struct icmp6_hdr *)b;h->icmp6_type=ICMP6_ECHO_REQUEST;h->icmp6_code=0;h->icmp6_id=htons(0x5207);h->icmp6_seq=htons((uint16_t)k);
        sendto(s,b,8+(size_t)n,0,(struct sockaddr *)&a,sizeof(a));
        int got=0;for (int t=0;t<4;t++) {uint8_t r[2100];ssize_t m=recv(s,r,sizeof(r),0);if (m<=0) break;
            struct icmp6_hdr *q=(struct icmp6_hdr *)r;if (q->icmp6_type==ICMP6_ECHO_REPLY && q->icmp6_id==htons(0x5207) && q->icmp6_seq==htons((uint16_t)k)) {got=(int)m;break;}}
        printf("payload %4d (IP %4d): %s\n",n,n+48,got?"reply":"lost");fflush(stdout);
    }
    return 0;
}

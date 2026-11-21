/* MIT. One SIP request over UDP from the IMS interface to a P-CSCF; prints the answer. Test tool. */
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
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
    if (argc<4) {fprintf(stderr,"usage: sip-probe IF LOCAL6 PCSCF6 [port]\n");return 2;}
    unsigned idx=if_nametoindex(argv[1]);int s=socket(AF_INET6,SOCK_DGRAM,0);
    setsockopt(s,IPPROTO_IPV6,IPV6_BOUND_IF,&idx,sizeof(idx));
    struct sockaddr_in6 l={.sin6_len=sizeof(l),.sin6_family=AF_INET6,.sin6_port=htons(5060)};inet_pton(AF_INET6,argv[2],&l.sin6_addr);
    if (bind(s,(struct sockaddr *)&l,sizeof(l))) perror("bind");
    struct sockaddr_in6 a={.sin6_len=sizeof(a),.sin6_family=AF_INET6,.sin6_port=htons(argc>4?atoi(argv[4]):5060)};inet_pton(AF_INET6,argv[3],&a.sin6_addr);
    char m[1024];int n=snprintf(m,sizeof(m),
        "OPTIONS sip:[%s] SIP/2.0\r\nVia: SIP/2.0/UDP [%s]:5060;branch=z9hG4bK%08x;rport\r\nMax-Forwards: 70\r\n"
        "From: <sip:probe@[%s]>;tag=%08x\r\nTo: <sip:[%s]>\r\nCall-ID: %08x@redwood\r\nCSeq: 1 OPTIONS\r\nContent-Length: 0\r\n\r\n",
        argv[3],argv[2],arc4random(),argv[2],arc4random(),argv[3],arc4random());
    if (sendto(s,m,(size_t)n,0,(struct sockaddr *)&a,sizeof(a))<0) {perror("sendto");return 1;}
    struct timeval tv={8,0};setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    char b[4096];ssize_t r=recv(s,b,sizeof(b)-1,0);if (r<=0) {printf("no answer in 8 s\n");return 1;}
    b[r]=0;printf("%s\n",b);return 0;
}

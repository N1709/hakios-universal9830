/* MIT. Fetch http://[ADDR]/PATH over one interface only (IPV6_BOUND_IF): proves the cellular data path. */
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
    if (argc<4) {fprintf(stderr,"usage: cell-http IFNAME IPV6 HOST [PATH]\n");return 2;}
    unsigned idx=if_nametoindex(argv[1]);if (!idx) {fprintf(stderr,"no interface %s\n",argv[1]);return 1;}
    int s=socket(AF_INET6,SOCK_STREAM,0);
    if (setsockopt(s,IPPROTO_IPV6,IPV6_BOUND_IF,&idx,sizeof(idx))) perror("IPV6_BOUND_IF");
    struct timeval tv={10,0};setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    struct sockaddr_in6 a;memset(&a,0,sizeof(a));a.sin6_len=sizeof(a);a.sin6_family=AF_INET6;a.sin6_port=htons(80);
    if (inet_pton(AF_INET6,argv[2],&a.sin6_addr)!=1) {fprintf(stderr,"bad address\n");return 2;}
    struct timeval t0,t1;gettimeofday(&t0,NULL);
    if (connect(s,(struct sockaddr *)&a,sizeof(a))) {perror("connect");return 1;}
    gettimeofday(&t1,NULL);printf("connected over %s in %ld ms\n",argv[1],(long)((t1.tv_sec-t0.tv_sec)*1000+(t1.tv_usec-t0.tv_usec)/1000));
    char req[512];snprintf(req,sizeof(req),"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",argc>4?argv[4]:"/",argv[3]);
    write(s,req,strlen(req));
    char buf[4096];ssize_t n;size_t total=0;
    while ((n=read(s,buf,sizeof(buf)-1))>0) {buf[n]=0;if (total<3000) fputs(buf,stdout);total+=(size_t)n;}
    printf("\n[%zu bytes]\n",total);close(s);return total?0:1;
}

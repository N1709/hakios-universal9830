/* MIT. Plain app-style fetch: getaddrinfo(HOST) + connect + GET, no interface pinning. */
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>
int main(int argc,char **argv)
{
    const char *host=argc>1?argv[1]:"ipv6.google.com",*path=argc>2?argv[2]:"/";
    struct addrinfo h,*r=NULL;memset(&h,0,sizeof(h));h.ai_socktype=SOCK_STREAM;
    int e=getaddrinfo(host,"80",&h,&r);if (e) {printf("dns %s: %s\n",host,gai_strerror(e));return 1;}
    for (struct addrinfo *a=r;a;a=a->ai_next) {
        char t[64];inet_ntop(a->ai_family,a->ai_family==AF_INET6?(void *)&((struct sockaddr_in6 *)a->ai_addr)->sin6_addr:(void *)&((struct sockaddr_in *)a->ai_addr)->sin_addr,t,sizeof(t));
        printf("dns %s -> %s\n",host,t);
        int s=socket(a->ai_family,SOCK_STREAM,0);struct timeval tv={10,0};setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
        if (connect(s,a->ai_addr,a->ai_addrlen)) {perror("connect");close(s);continue;}
        char q[512];snprintf(q,sizeof(q),"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",path,host);write(s,q,strlen(q));
        char b[4096];ssize_t n;size_t tot=0;while ((n=read(s,b,sizeof(b)-1))>0) {b[n]=0;if (!tot) {char *x=strchr(b,'\n');if (x) *x=0;printf("%s\n",b);}tot+=(size_t)n;}
        printf("[%zu bytes]\n",tot);close(s);freeaddrinfo(r);return tot?0:1;
    }
    freeaddrinfo(r);return 1;
}

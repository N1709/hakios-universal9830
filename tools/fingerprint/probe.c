/* Fixed-command client for the manually armed TEE diagnostic. No credentials. */
#include <sys/sysctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
int main(int argc,char **argv) {
    const char *inspect=NULL,*call=NULL,*prefix=NULL;
    bool do_call=false;
    if(argc==2 && (!strcmp(argv[1],"--inspect") || !strcmp(argv[1],"--call-once"))) {
        inspect="fp.tee-version 0";call="fp.tee-version 1";prefix="fp.tee-version:";
        do_call=!strcmp(argv[1],"--call-once");
    } else if(argc==2 && (!strcmp(argv[1],"--inspect-channels") || !strcmp(argv[1],"--connect-channels"))) {
        inspect="fp.tee-channels 0";call="fp.tee-channels 1";prefix="fp.tee-channels:";
        do_call=!strcmp(argv[1],"--connect-channels");
    } else if(argc==2 && (!strcmp(argv[1],"--inspect-sysconf") || !strcmp(argv[1],"--exchange-sysconf"))) {
        inspect="fp.tee-sysconf 0";call="fp.tee-sysconf 1";prefix="fp.tee-sysconf:";
        do_call=!strcmp(argv[1],"--exchange-sysconf");
    } else {
        fputs("Usage: fingerprint-probe --inspect | --call-once | --inspect-channels | --connect-channels | --inspect-sysconf | --exchange-sysconf\n",stderr);return 2;
    }
    if(geteuid()!=0){fputs("Root is required for this debug diagnostic\n",stderr);return 1;}
    /* Check the fixed command's diagnostic prefix before an armed operation. */
    char response[8192]={0};size_t n=sizeof(response)-1;
    if(sysctlbyname("kern.redwood.poke",NULL,NULL,(void *)inspect,strlen(inspect)+1) ||
        sysctlbyname("kern.redwood.poke",response,&n,NULL,0)) {perror("fingerprint probe inspect");return 1;}
    response[sizeof(response)-1]=0;
    if(strncmp(response,prefix,strlen(prefix))){fputs("Kernel lacks the fingerprint diagnostic\n",stderr);return 1;}
    if(do_call) {
        memset(response,0,sizeof(response));n=sizeof(response)-1;
        if(sysctlbyname("kern.redwood.poke",NULL,NULL,(void *)call,strlen(call)+1) ||
            sysctlbyname("kern.redwood.poke",response,&n,NULL,0)) {perror("fingerprint probe call");return 1;}
        response[sizeof(response)-1]=0;
    }
    fputs(response,stdout);return 0;
}

/* Temporary, metadata-only locationd probe. Never publishes a location. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
static void (*original_begin)(id,SEL);
static void dump_class(FILE *f,Class cls)
{
    if(!cls)return;
    fprintf(f,"class %s\n",class_getName(cls));
    unsigned n=0;Method *methods=class_copyMethodList(cls,&n);
    for(unsigned i=0;i<n;i++)fprintf(f," method %s %s\n",sel_getName(method_getName(methods[i])),method_getTypeEncoding(methods[i]));
    free(methods);
}
static void begin_service(id self,SEL cmd)
{
    original_begin(self,cmd);
    static bool reported;
    if(reported)return;reported=true;
    int fd=open("/var/tmp/gnss-location-abi.txt",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0)return;
    FILE *f=fdopen(fd,"w");if(!f){close(fd);return;}
    dump_class(f,object_getClass(self));
    SEL adaptee=sel_registerName("adaptee");
    void *provider=((void *(*)(id,SEL))objc_msgSend)(self,adaptee);
    fprintf(f,"provider present %d\n",provider!=NULL);
    if(provider){
        /* Verified 20F66 CLNotifier base member, used by its callback's silo assertion. */
        id context=*(id *)((char *)provider+0x20);
        dump_class(f,object_getClass(context));
        if([context respondsToSelector:@selector(silo)]){
            id silo=((id(*)(id,SEL))objc_msgSend)(context,@selector(silo));
            dump_class(f,object_getClass(silo));
            dump_class(f,class_getSuperclass(object_getClass(silo)));
        }
    }
    fclose(f);
}
__attribute__((constructor)) static void install(void)
{
    int metadata=open("/var/tmp/gnss-universe-abi.txt",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(metadata>=0){FILE *f=fdopen(metadata,"w");if(f){
        dump_class(f,objc_getClass("CLIntersiloUniverse"));
        Class base=objc_getClass("CLNotifierServiceAdapter");
        for(unsigned i=0;base && i<4;i++,base=class_getSuperclass(base))dump_class(f,base);
        fclose(f);
    }else close(metadata);}
    Class cls=objc_getClass("CLAccessoryLocationProviderAdapter");
    Method m=cls?class_getInstanceMethod(cls,@selector(beginService)):NULL;
    static const unsigned char expected[]={0xf4,0x4f,0xbe,0xa9,0xfd,0x7b,0x01,0xa9,0xfd,0x43,0x00,0x91,0xf3,0x03,0x00,0xaa};
    bool compatible=m && !strcmp(method_getTypeEncoding(m),"v16@0:8") && !memcmp((void *)method_getImplementation(m),expected,sizeof(expected));
    int fd=open("/var/tmp/gnss-location-arm.txt",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd>=0){dprintf(fd,"class_present=%d method_present=%d compatible=%d\n",cls!=Nil,m!=NULL,compatible);close(fd);}
    if(!compatible)return;
    original_begin=(void(*)(id,SEL))method_getImplementation(m);
    method_setImplementation(m,(IMP)begin_service);
}

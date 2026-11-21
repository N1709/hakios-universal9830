/* Read-only Google Maps renderer metadata. No hooks, locations or user content. */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
static void walk(FILE *f, CALayer *l, unsigned depth) {
 if (!l || depth>24) return;
 fprintf(f,"%*s%s %.0fx%.0f opacity=%g hidden=%d\n",depth,"",class_getName(l.class),l.bounds.size.width,l.bounds.size.height,l.opacity,l.hidden);
 for (CALayer *c in l.sublayers) walk(f,c,depth+1);
}
__attribute__((constructor)) static void start(void) {
 int fd=open("/var/tmp/google-renderer-probe.loaded",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
 if(fd>=0){write(fd,"loaded\n",7);close(fd);}
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),dispatch_get_main_queue(),^{
 int fd=open("/var/tmp/google-renderer-probe.txt",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)return;FILE*f=fdopen(fd,"w");if(!f){close(fd);return;}
 unsigned count=0;Class *classes=objc_copyClassList(&count);
 for(unsigned i=0;i<count;i++) {
  const char *name=class_getName(classes[i]);if(strncmp(name,"GMS",3))continue;
  for(unsigned meta=0;meta<2;meta++){
   unsigned n=0;Method *methods=class_copyMethodList(meta?object_getClass(classes[i]):classes[i],&n);
   for(unsigned j=0;j<n;j++){const char*s=sel_getName(method_getName(methods[j]));if(strcasestr(s,"metal")||strstr(s,"RendererType"))fprintf(f,"%s %c %s %s\n",name,meta?'+':'-',s,method_getTypeEncoding(methods[j]));}free(methods);
  }
 }free(classes);
 for(UIWindowScene*scene in UIApplication.sharedApplication.connectedScenes)if([scene isKindOfClass:UIWindowScene.class])for(UIWindow*w in scene.windows)walk(f,w.layer,0);
 fclose(f);
 });
}

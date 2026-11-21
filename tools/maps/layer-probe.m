#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#import <Metal/Metal.h>
/* Read-only Apple Maps layer-state diagnostic; geometry/opacity only. */
static unsigned count;
static void layer(FILE *f,CALayer *l,unsigned depth){
 if(!l || depth>30 || count++>3000)return;
 CALayer *p=l.presentationLayer;
 CGRect r=l.bounds;

 fprintf(f,"%*s%s bounds=%.0f,%.0f,%.0f,%.0f opacity=%g presented=%g hidden=%d opaque=%d animations=%lu\n",depth*2,"",class_getName(object_getClass(l)),r.origin.x,r.origin.y,r.size.width,r.size.height,l.opacity,p?p.opacity:-1,l.hidden,l.opaque,(unsigned long)l.animationKeys.count);
 for(NSString *key in l.animationKeys){CAAnimation *a=[l animationForKey:key];fprintf(f,"%*s anim %s %s duration=%g removed=%d\n",depth*2,"",key.UTF8String,class_getName(object_getClass(a)),a.duration,a.removedOnCompletion);}
 for(CALayer *s in l.sublayers)layer(f,s,depth+1);
}
__attribute__((constructor))static void start(void){
 for(unsigned n=1;n<=60;n++)dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)n*2*NSEC_PER_SEC),dispatch_get_main_queue(),^{
  NSString *path=[NSString stringWithFormat:@"/var/tmp/maps-layers.%s.%u.txt",getprogname(),n];
  int fd=open(path.UTF8String,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)return;FILE*f=fdopen(fd,"w");if(!f){close(fd);return;}count=0;
  for(UIWindow*w in UIApplication.sharedApplication.windows){fprintf(f,"WINDOW %s level=%g hidden=%d alpha=%g\n",class_getName(object_getClass(w)),w.windowLevel,w.hidden,w.alpha);layer(f,w.layer,0);}fclose(f);
 });
}

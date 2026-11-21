/* RedwoodCustomWallpaper (SpringBoard): a picture of the user's own as the wallpaper, without
 * PosterBoard's editor. The newest picture in Files > On My iPhone > Wallpaper becomes the lock
 * and home wallpaper; one in Wallpaper/Home overrides the home screen alone. Empty the folder to
 * go back to the poster. The picture is laid over the lock poster's view, which the home screen
 * mirrors through its portal, so home blur/dim and the parallax (wallpaper_motion.h) still apply.
 * Re-checked every 2 s and on `notify com.redwood.wallpaper.changed`. */
#import <UIKit/UIKit.h>
#import <ImageIO/ImageIO.h>
#include <notify.h>
#include <objc/message.h>
#include <sys/stat.h>
#define RW_TAG 0x52575750
static NSString *lockPath,*homePath; static NSTimeInterval lockTime,homeTime;
static UIImage *lockImage,*homeImage;

static NSArray<NSString*> *wallpaperDirs(NSString *sub)
{
    NSMutableArray *r=[NSMutableArray array];
    NSString *base=@"/var/mobile/Containers/Shared/AppGroup";
    for(NSString *g in [NSFileManager.defaultManager contentsOfDirectoryAtPath:base error:nil]) {
        NSString *d=[NSString stringWithFormat:@"%@/%@/File Provider Storage/Wallpaper%@",base,g,sub];
        BOOL dir=NO; if([NSFileManager.defaultManager fileExistsAtPath:d isDirectory:&dir]&&dir) [r addObject:d];
    }
    [r addObject:[@"/var/mobile/Library/Redwood/Wallpaper" stringByAppendingString:sub]];
    return r;
}
static NSString *newestPicture(NSString *sub,NSTimeInterval *when)
{
    NSString *best=nil; NSTimeInterval bt=0;
    NSSet *ext=[NSSet setWithArray:@[@"jpg",@"jpeg",@"png",@"heic",@"heif"]];
    for(NSString *d in wallpaperDirs(sub)) for(NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:d error:nil]) {
        if(![ext containsObject:f.pathExtension.lowercaseString]) continue;
        NSString *p=[d stringByAppendingPathComponent:f]; struct stat st;
        if(stat(p.fileSystemRepresentation,&st)) continue;
        NSTimeInterval t=st.st_mtimespec.tv_sec+st.st_mtimespec.tv_nsec/1e9;
        if(t>bt) {bt=t;best=p;}
    }
    *when=bt; return best;
}
static UIImage *load(NSString *path)
{
    CGImageSourceRef src=CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],NULL);
    if(!src) return nil;
    CGFloat px=MAX(UIScreen.mainScreen.nativeBounds.size.width,UIScreen.mainScreen.nativeBounds.size.height)*1.2;
    NSDictionary *o=@{(id)kCGImageSourceCreateThumbnailFromImageAlways:@YES,(id)kCGImageSourceThumbnailMaxPixelSize:@(px),(id)kCGImageSourceCreateThumbnailWithTransform:@YES};
    CGImageRef img=CGImageSourceCreateThumbnailAtIndex(src,0,(__bridge CFDictionaryRef)o);
    CFRelease(src);
    if(!img) return nil;
    UIImage *u=[UIImage imageWithCGImage:img scale:UIScreen.mainScreen.scale orientation:UIImageOrientationUp];
    CGImageRelease(img); return u;
}
static void refresh(NSString *sub,NSString *__strong *path,NSTimeInterval *time,UIImage *__strong *image)
{
    NSTimeInterval t=0; NSString *p=newestPicture(sub,&t);
    if((p==*path||[p isEqual:*path]) && t==*time) return;
    *path=p;*time=t;*image=p?load(p):nil;
    NSLog(@"redwood: custom wallpaper%@: %@ (%@)",sub,p?:@"none, back to the poster",*image?@"loaded":@"-");
}
static void collect(UIView *v,Class lock,Class home,NSMutableArray *locks,NSMutableArray *homes,int depth)
{
    if(depth>16) return;
    if(lock&&[v.nextResponder isKindOfClass:lock]) [locks addObject:v];
    else if(home&&[v.nextResponder isKindOfClass:home]) [homes addObject:v];
    for(UIView *s in v.subviews) collect(s,lock,home,locks,homes,depth+1);
}
static void cover(UIView *v,UIImage *img)
{
    UIImageView *iv=(UIImageView*)[v viewWithTag:RW_TAG];
    if(iv && iv.superview!=v) iv=nil;
    if(!img) { [iv removeFromSuperview]; return; }
    if(!iv) {
        iv=[[UIImageView alloc] initWithFrame:v.bounds];
        iv.tag=RW_TAG;iv.contentMode=UIViewContentModeScaleAspectFill;iv.clipsToBounds=YES;
        iv.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        iv.userInteractionEnabled=NO;
    }
    if(iv.image!=img) iv.image=img;
    if(v.subviews.lastObject!=iv) [v addSubview:iv];
    if(!CGRectEqualToRect(iv.frame,v.bounds)) iv.frame=v.bounds;
}
static void apply(void)
{
    refresh(@"",&lockPath,&lockTime,&lockImage);
    refresh(@"/Home",&homePath,&homeTime,&homeImage);
    Class sw=NSClassFromString(@"_SBWallpaperSecureWindow"),lock=NSClassFromString(@"PBUIPosterLockViewController"),home=NSClassFromString(@"PBUIPosterHomeViewController");
    NSArray *ws=((id(*)(id,SEL,BOOL,BOOL))objc_msgSend)(UIWindow.class,sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:"),YES,NO);
    for(UIWindow *w in ws) {
        if(sw && ![w isKindOfClass:sw]) continue;
        NSMutableArray *locks=[NSMutableArray array],*homes=[NSMutableArray array];
        collect(w,lock,home,locks,homes,0);
        for(UIView *v in locks) cover(v,lockImage);
        for(UIView *v in homes) cover(v,homeImage);
    }
}
__attribute__((constructor)) static void init(void)
{
    if(![NSProcessInfo.processInfo.processName isEqual:@"SpringBoard"]) return;
    [NSFileManager.defaultManager createDirectoryAtPath:@"/var/mobile/Library/Redwood/Wallpaper" withIntermediateDirectories:YES attributes:nil error:nil];
    dispatch_async(dispatch_get_main_queue(),^{
        static dispatch_source_t t; t=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
        dispatch_source_set_timer(t,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),2*NSEC_PER_SEC,200*NSEC_PER_MSEC);
        dispatch_source_set_event_handler(t,^{ @autoreleasepool { apply(); } });
        dispatch_resume(t);
        int tok; notify_register_dispatch("com.redwood.wallpaper.changed",&tok,dispatch_get_main_queue(),^(int x){ apply(); });
    });
}

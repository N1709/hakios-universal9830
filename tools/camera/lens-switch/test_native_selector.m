/* Device test of the actual selector/delegate, with an isolated control file. */
#define RW_LENS_TELE_ENABLE_PATH "/var/root/native-lens-test-tele-enable"
#define RW_LENS_PRIMARY_TEST_PATH "/var/root/native-lens-test-primary-default"
#define RW_LENS_CONTROL_PATH @"/var/root/native-lens-test-zoom"
#include "camera_lens_switch.m"
#include <dlfcn.h>
#include <assert.h>
#include <stdio.h>
@interface RWTestGraph:NSObject
@property(nonatomic) NSInteger devicePosition;
@end
@implementation RWTestGraph @end
@interface RWTestController:NSObject
@property(nonatomic,strong) RWTestGraph *graph;
@property(nonatomic) NSUInteger forwarded;
@property(nonatomic) double graphZoom;
@end
@implementation RWTestController
- (id)_currentGraphConfiguration{return self.graph;}
- (void)zoomControl:(id)zoom didChangeZoomFactor:(double)factor interactionType:(NSInteger)type {
    (void)zoom;(void)type;self.graphZoom=factor;
}
- (void)zoomControl:(id)zoom didChangeSelectedButtonIndex:(NSUInteger)index displayZoomFactor:(double)factor {
    (void)zoom;(void)index;(void)factor;self.forwarded++;
}
@end
int main(void){@autoreleasepool{
    unlink(RW_LENS_PRIMARY_TEST_PATH);unlink(RW_LENS_TELE_ENABLE_PATH);
    assert(dlopen("/System/Library/PrivateFrameworks/CameraUI.framework/CameraUI",RTLD_NOW));
    UIView *zoom=[[NSClassFromString(@"CAMZoomControl") alloc]initWithFrame:CGRectMake(0,0,375,102)];
    ((void(*)(id,SEL,double,double,double,double,double))objc_msgSend)(zoom,NSSelectorFromString(@"configureForSingleCameraPlusToggleWithZoomFactor:minimumZoomFactor:displayMinimumZoomFactor:maximumZoomFactor:displayMaximumZoomFactor:"),1,1,1,5,5);
    RWTestController *original=[RWTestController new];original.graph=[RWTestGraph new];original.graph.devicePosition=0;
    SEL setter=NSSelectorFromString(@"setDelegate:"),getter=NSSelectorFromString(@"delegate");
    ((void(*)(id,SEL,id))objc_msgSend)(zoom,setter,original);
    [@"1\n" writeToFile:control atomically:YES encoding:NSUTF8StringEncoding error:nil];
    configureZoom(zoom);
    RWNativeLensDelegate *proxy=objc_getAssociatedObject(zoom,&proxyKey);assert(proxy&&proxy.rear&&proxy.original==original);
    assert(requestedZoom()==0.9);
    FILE *flag=fopen(RW_LENS_PRIMARY_TEST_PATH,"w");assert(flag);fclose(flag);
    assert(defaultRearValue().doubleValue==1);unlink(RW_LENS_PRIMARY_TEST_PATH);
    assert(defaultRearValue().doubleValue==0.9);
    assert(fabs(((double(*)(id,SEL,double))objc_msgSend)(zoom,NSSelectorFromString(@"_displayZoomValueForZoomFactor:"),0.9)-0.9)<0.0001);
    assert([[zoom valueForKey:@"_zoomFactors"] containsObject:@0.5]);
    assert([[zoom valueForKey:@"minimumZoomFactor"] doubleValue]==0.5);
    [proxy zoomControl:zoom didChangeSelectedButtonIndex:0 displayZoomFactor:(0.5/displayBase)];assert(requestedZoom()==0.5&&original.forwarded==0);
    [proxy zoomControl:zoom didChangeSelectedButtonIndex:1 displayZoomFactor:(1/displayBase)];assert(requestedZoom()==1&&original.forwarded==0);
    [proxy toggleBetweenZoomFactorsForZoomControl:zoom];assert(requestedZoom()==0.5);
    [proxy toggleBetweenZoomFactorsForZoomControl:zoom];assert(requestedZoom()==1);
    [proxy zoomControl:zoom didChangeZoomFactor:2.5 interactionType:1];assert(requestedZoom()==2.5&&original.graphZoom==0);
    [proxy zoomControl:zoom didChangeZoomFactor:0.8 interactionType:1];assert(requestedZoom()==0.8&&original.graphZoom==0);
    [proxy zoomControl:zoom didChangeZoomFactor:0.5 interactionType:1];assert(requestedZoom()==0.5);
    ((void(*)(id,SEL,double))objc_msgSend)(zoom,NSSelectorFromString(@"setMinAvailableZoomFactor:"),1);
    ((void(*)(id,SEL,double))objc_msgSend)(zoom,NSSelectorFromString(@"setZoomFactor:"),1);
    configureZoom(zoom);
    assert([[zoom valueForKey:@"minAvailableZoomFactor"] doubleValue]==0.5);
    assert([[zoom valueForKey:@"zoomFactor"] doubleValue]==0.5);
    original.graph.devicePosition=1;configureZoom(zoom);assert(!proxy.rear);
    [proxy zoomControl:zoom didChangeZoomFactor:3 interactionType:1];assert(original.graphZoom==3&&requestedZoom()==0.5);
    assert(((id(*)(id,SEL))objc_msgSend)(zoom,getter)==original);
    original.graph.devicePosition=0;configureZoom(zoom);assert(proxy.rear);
    assert(((id(*)(id,SEL))objc_msgSend)(zoom,getter)==proxy);
    configureZoom(zoom);assert(proxy.original==original);
    flag=fopen(RW_LENS_TELE_ENABLE_PATH,"w");assert(flag);fclose(flag);configureZoom(zoom);
    assert([zoom valueForKey:@"_buttonPlatter"]==nil);
    [proxy zoomControl:zoom didChangeSelectedButtonIndex:2 displayZoomFactor:3];assert(requestedZoom()==3);
    [proxy zoomControl:zoom didChangeZoomFactor:10 interactionType:1];assert(requestedZoom()==10&&original.graphZoom==3);
    [proxy toggleBetweenZoomFactorsForZoomControl:zoom];assert(requestedZoom()==0.5);
    [proxy toggleBetweenZoomFactorsForZoomControl:zoom];assert(requestedZoom()==1);
    [proxy toggleBetweenZoomFactorsForZoomControl:zoom];assert(requestedZoom()==3);
    SEL nativeTap=NSSelectorFromString(@"_handleButtonTapped:");id button=[zoom valueForKey:@"_zoomButton"];
    ((void(*)(id,SEL,id))objc_msgSend)(zoom,nativeTap,button);assert(requestedZoom()==0.5);
    ((void(*)(id,SEL,id))objc_msgSend)(zoom,nativeTap,button);assert(requestedZoom()==1);
    ((void(*)(id,SEL,id))objc_msgSend)(zoom,nativeTap,button);assert(requestedZoom()==3);
    assert(!((BOOL(*)(id,SEL))objc_msgSend)(zoom,NSSelectorFromString(@"isShowingZoomDial")));
    ((void(*)(id,SEL))objc_msgSend)(zoom,NSSelectorFromString(@"_handleButtonTappedForSingleCameraZoomPlusToggle"));assert(requestedZoom()==0.5);
    unlink(RW_LENS_TELE_ENABLE_PATH);configureZoom(zoom);
    assert(![[zoom valueForKey:@"_zoomFactors"] containsObject:@3]);
    [[NSFileManager defaultManager]removeItemAtPath:control error:nil];
    puts("PASS: actual native selector, rear selection/toggle/drag writes, front restoration, rear re-entry");
}}

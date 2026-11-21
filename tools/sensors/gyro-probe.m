// Read-only probe of the public iOS CoreMotion path. No injected test values.
#import <Foundation/Foundation.h>
#import <CoreMotion/CoreMotion.h>
#import <objc/message.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <mach/mach_time.h>

int main(void)
{
    @autoreleasepool {
        CMMotionManager *manager = [CMMotionManager new];
        printf("gyro_available=%d accel_available=%d device_motion_available=%d\n",
            manager.gyroAvailable, manager.accelerometerAvailable, manager.deviceMotionAvailable);
        Class wake = NSClassFromString(@"CMWakeGestureManager");
        SEL available = NSSelectorFromString(@"isWakeGestureAvailable");
        printf("wake_gesture_available=%d\n", wake && [wake respondsToSelector:available] ?
            ((BOOL (*)(id, SEL))objc_msgSend)(wake, available) : -1);
        manager.gyroUpdateInterval = .02;
        manager.deviceMotionUpdateInterval = .02;
        [manager startGyroUpdates];
        [manager startDeviceMotionUpdatesUsingReferenceFrame:CMAttitudeReferenceFrameXArbitraryZVertical];
        double previous = 0, peak = 0, angle[3] = {0}, maxGap = 0;
        unsigned samples = 0, fusedSamples = 0;
        double fusedPrevious = 0;
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        double scale = (double)tb.numer / tb.denom / 1e9;
        double start = mach_absolute_time() * scale, report = start;
        while (mach_absolute_time() * scale - start < 20) {
            @autoreleasepool {
                usleep(10000);
                CMGyroData *data = manager.gyroData;
                if (data && data.timestamp > previous) {
                    CMRotationRate rate = data.rotationRate;
                    double dt = previous ? data.timestamp - previous : 0;
                    if (dt > maxGap) maxGap = dt;
                    if (dt > 0 && dt < 1) {
                        angle[0] += rate.x * dt; angle[1] += rate.y * dt; angle[2] += rate.z * dt;
                    }
                    double speed = sqrt(rate.x * rate.x + rate.y * rate.y + rate.z * rate.z);
                    if (speed > peak) peak = speed;
                    previous = data.timestamp; samples++;
                }
                double now = mach_absolute_time() * scale;
                CMDeviceMotion *motion = manager.deviceMotion;
                if (motion && motion.timestamp > fusedPrevious) {
                    fusedPrevious = motion.timestamp;
                    fusedSamples++;
                }
                if (now - report >= 1) {
                    report = now;
                    CMRotationRate rate = data ? data.rotationRate : (CMRotationRate){0};
                    printf("samples=%u t=%.3f gyro_rad_s=(%.5f,%.5f,%.5f) peak=%.5f integrated_rad=(%.3f,%.3f,%.3f) fused=%s fused_samples=%u fused_t=%.3f\n",
                        samples, previous, rate.x, rate.y, rate.z, peak, angle[0], angle[1], angle[2], motion ? "yes" : "no",
                        fusedSamples, fusedPrevious);
                    fflush(stdout);
                }
            }
        }
        [manager stopDeviceMotionUpdates]; [manager stopGyroUpdates]; [manager release];
        printf("result: gyro_samples=%u fused_samples=%u max_gyro_gap_s=%.3f\n", samples, fusedSamples, maxGap);
        return samples ? 0 : 1;
    }
}

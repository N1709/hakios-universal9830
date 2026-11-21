# CoreMotion gyro probe

Build from the repository root, then run on the connected iOS phone:

```sh
xnu-deps/host/bin/xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1 -framework Foundation -framework CoreMotion -o /tmp/gyro-probe tools/sensors/gyro-probe.m
scp -O /tmp/gyro-probe s20:/var/tmp/gyro-probe
ssh s20 /var/tmp/gyro-probe
```

The probe runs for about 20 seconds plus framework startup time. It reports
real samples received through CMMotionManager, peak rotation rate, accumulated
angles and whether fused device-motion samples arrive. Rotate about one axis,
hold still, then return. A 90-degree turn corresponds to about 1.57 radians;
returning approximately cancels the signed angle. Public gyro rates are rad/s;
Apple's HID gyro events use deg/s and CoreMotion performs that conversion.
This probe does not change sensor settings beyond starting/stopping its own
CoreMotion subscriptions, nor does it override the bridge's input.

To exercise the port's software-fusion route in this standalone command-line
process, run it with the existing bridge loaded:

```sh
ssh s20 'DYLD_INSERT_LIBRARIES=/redwood/lib/libredwood.dylib /var/tmp/gyro-probe'
```

The kernel's normal injection rules cover app directories, not `/var/tmp`.
Compare both runs: live gyro data with no fused data in the plain run, followed
by increasing `fused_samples` and `fused_t` in the injected run, distinguishes
raw sensor delivery from the missing Apple SPU/software-fusion route. The probe
returns success for raw gyro delivery; inspect fused counters separately.
`wake_gesture_available` only checks the platform capability, not delivery of
Raise to Wake events.

## Lift to Wake on the S20

`userland/src/motion_wake.m` implements a table-pickup wake service for this
port, which has raw sensors but no Apple SPU wake-gesture stream. It uses real
CoreMotion accelerometer and gyro samples. It requires a still, face-up table
pose, followed by a tilt toward portrait; it does not implement Apple's full
pocket classifier. A short side-button event uses the existing port wake path.
It never unlocks the phone. The native `SBSUndimScreen` request was tested and
did not turn on an off display on this port.

The service honours mobile's native `SBSupportLiftToWake` preference (default
on). It stops its sensor subscriptions while the display is on or the setting
is disabled, rejects stale data and shaking, and requires a new resting pose
before another pickup. BrightnessSystemClient needs its run loop serviced to
receive display-state changes. A wake requires both its screen-off state and
the native HID-display/blanked-screen notification states to agree.

Both build paths compile and sign `/redwood/bin/redwood-motiond`; images that
contain it include `com.redwood.motion-wake.plist` for startup. Set
`REDWOOD_MOTION_DEBUG=1` on the service only when collecting per-second samples.

For a bounded on-device test, build the same implementation from the wrapper:

```sh
xnu-deps/host/bin/xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1 -framework Foundation -framework CoreMotion -o /tmp/raise-probe tools/sensors/raise-probe.m
scp -O /tmp/raise-probe s20:/var/tmp/raise-probe
ssh s20 'DYLD_INSERT_LIBRARIES=/redwood/lib/libredwood.dylib /var/tmp/raise-probe --probe'
# --probe only observes; --once requests one pickup wake (both run up to 120 s).
ssh s20 'DYLD_INSERT_LIBRARIES=/redwood/lib/libredwood.dylib /var/tmp/raise-probe --once'
```

Lock the phone, leave it face-up and still for two seconds, and lift it by the
edges toward portrait without touching the screen or side button. A successful
test prints `table pickup; wake event=sent`, `display-on verified=1`, and
`wake_verified=1`. The background service (`--daemon`) runs until stopped and
logs its subscriptions turning off after the wake.

Host scenario checks:

```sh
cc -Wall -Wextra -Werror tools/tests/raise_policy.c -lm -o /tmp/raise-policy
/tmp/raise-policy
```

On 2026-11-13, the live service recorded two pickups with changing raw data
(including 1.792 and 3.219 rad/s gyro magnitudes), sent the port wake event,
verified display-on, and stopped its subscriptions. The user confirmed that
pickup wake worked during the controlled test. The temporary live installation
is under `/var/root/redwood-motion`; loading it with launchctl does not by
itself make it start after a reboot. Boot persistence requires an image built
with the new binary and launch job.

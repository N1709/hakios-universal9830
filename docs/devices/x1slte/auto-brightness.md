# Adaptive brightness and optional software warmth

## Status (2026-11-08)

The first port-side automatic controller was verified on the phone: about
8 lux / 26 nits rose to 1071 lux / 277 nits, continued toward 350+ nits,
then dimmed again. CoreBrightness and driver targets agreed. Auto-Brightness
OFF stopped automatic writes, and the user confirmed manual slider operation.

The newer adaptive policy, lower-overhead monitor and optional software True Tone
approximation are built and host-checked. Their final device validation is still
pending because the phone became unreachable over USB and Wi-Fi. The warmth is
OFF by default. Do not describe those new features as device-verified yet.

## Adaptive behavior

`userland/src/brightness_policy.h` implements the ambient-to-target policy used
by `inject_sensors.c`. It follows the design of AOSP's hysteresis, debounce and
short-term user model, with original C code and port-specific thresholds:

- A new light level must remain within a stability band for 5 seconds before
  brightening, or 8 seconds before dimming. Small fluctuations and brief flashes
  keep the last accepted target. The band is 20% or 2 lux; accepted ambient changes
  also require 20% or 3 lux of hysteresis.
- Startup and wake hold the existing brightness for at least 5 seconds. Changes
  then ramp in perceptual/log luminance, faster upward than downward. No initial
  jump is intentionally issued by this controller.
- A Control Center adjustment anchors the desired nits to the current lux. Similar
  lighting retains that level. A sustained large lighting change blends toward the
  ordinary curve, constrained so brighter light cannot dim below the chosen anchor
  and darker light cannot brighten above it.
- Calibration is saved after 10 seconds without further slider movement in
  `com.redwood.display` / `BrightnessAnchor`. It survives lock/wake and process
  restarts. Auto-Brightness OFF leaves manual control alone.
- The curve anchors are 0 lux → 3 nominal nits, 3 → 8, 10 → 30, 150 → 150,
  and 1500 → 420. Dark segments are linear; brighter segments interpolate in
  log lux. These are policy values, not photometer-calibrated measurements.

The controller uses Apple's existing `DisplayNitsKey` output API and respects
`DisplayBrightnessAuto`, `DisplayOn` and sensor freshness. It bridges an unhandled
native ramp; it does not claim to repair Apple's entire automatic-brightness
implementation. The kernel now accepts/applies/publishes `brightness-nits`
(16.16 nits), while legacy manual-slider writes remain active. A previous cached-
nits ownership experiment blocked the slider; that rule was removed.

## Faster policy, perceptual slider (2026-11-13, A)

Light every 250 ms; brighten after 1.5 s, dim after 3 s, 4x changes after 0.5 / 1.5 s, wake hold 1 s, ramps
0.5 / 1.2 s. The slider's legacy backlight value maps to 2 + 418 s^2.2 nits in the kernel (`fb_bl_curve`, 
`legacyToNits16`) and in `rb_slider_nits` for the manual anchor; backboardd writes the slider as 0.025 + 0.95 s.
The figures below (3/5 s, 1000 ms) are the earlier state.

## Light sensor timing (2026-11-13)

The sensor hub (kernel, `redwood_ssp.c`) reads the light sensor every 1 s while the display is on and stops it
while the display is off (`shub_light_ms=`). Do not put the light sensor on a burst schedule: this controller
needs 5-8 s of steady, fresh readings before it moves the brightness, and with 2 s bursts every 30 s it never did.
Only proximity (an IR LED) runs in bursts (`shub_burst_on=2000`, `shub_burst_every=30000`).

## CPU, GPU and sleep

Brightness interpolation runs at up to 20 updates/s only while ramping. When
settled the worker checks at about 2 Hz; with the display/feature off or stale
samples it backs off to about 1 Hz and issues no brightness writes. Redundant
nits writes are suppressed. Calibration writes are debounced, not performed for
every slider event. No per-pixel CPU/GPU processing is added.

The dashboard uses one persistent SSH/helper stream, reusing its HID and
BrightnessSystem clients. It no longer launches SSH and multiple phone processes
for every sample. Stop the dashboard when measuring the phone's idle power.
These are scheduling/design bounds; phone CPU/power measurements remain pending
for this adaptive build.

## Optional software True Tone approximation

The native True Tone availability/enabled queries and toggle on 20F66 are bridged
on the exact `CBColorModuleiOS` methods, with implementation-address checks. The
setting is stored as `SoftwareTrueToneEnabled` in `com.redwood.display`.
It is independent of Auto-Brightness and OFF by default.

ON requests `fb.warm 1`: a static DQE gamma LUT in the Samsung display pipeline,
with approximate channel gains R=100%, G=98%, B=94%. This is optional fixed mild
warmth, not a claimed ambient color measurement or sensor-based True Tone. It
adds no rendering pass. OFF (`fb.warm 0`) restores the saved LUT, DQE control,
image size and data path; mode-size updates are handled while enabled. Panel
factory gamma/AOR data and power-off fade are not modified. Power-on fade stays
disabled. The warmth is deferred while the display is off or changing modes.

Register definitions/layout come from Samsung's Exynos9830 dpu30 `regs-dqe.h`,
`regs-decon.h` and `dqe_reg_start()` in the local reference kernel. Actual neutral/
warm appearance, native toggle visibility and exact restoration still need the
connected-device test; compilation alone does not establish those results.

## Native sensor integration and evidence

The light service's Apple VendorID (0x5ac) must follow generic VendorID defaults;
a later zero assignment previously overwrote it. CoreBrightness also resolves
its HID registry ID to an IOKit entry before constructing CBABModuleiOS._AABC.
Virtual HID IDs have no matching entry. A version-checked process-private import
adapter maps only the Redwood light service to the framebuffer registry ID as
metadata backing, retaining the original virtual ID for HID routing. The null
guard protects absent _AABC; it does not implement True Tone.

Stock CoreBrightness received changing TrustedLux and could calculate sensible
luminance, yet its automatic ramp stayed fixed. Capability flags, event traffic
and an accepted ALSCurveInfo dictionary alone were insufficient proof of working
automatic brightness. The port-side controller explicitly supplies the missing
luminance requests.

## Checks and references

```sh
clang -std=c11 -Wall -Wextra -Werror tools/tests/brightness_policy.c -o /tmp/brightness-policy-test
/tmp/brightness-policy-test
```

The test covers temporal debounce, brief bright flashes, manual calibration,
wake holds, extreme anchors and curve monotonicity/bounds. The kernel, userland
library, root image and recovery package build successfully; device tests above
are required before treating the new adaptive/warmth build as complete.
`tools/als-monitor/README.md` describes the read-only live dashboard.

Design references:
- [AOSP AutomaticBrightnessController](https://android.googlesource.com/platform/frameworks/base.git/+/master/services/core/java/com/android/server/display/AutomaticBrightnessController.java)
- [Clight curve configuration](https://github.com/FedeDP/Clight/wiki/Config)
- [Apple: brightness and True Tone are separate controls](https://support.apple.com/en-au/109351)

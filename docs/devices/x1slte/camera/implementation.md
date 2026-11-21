# S20 primary camera: first quality-correctness implementation

Branch: **`feat/s20-camera-quality`**, from research preservation commit `d5b02944`.
The frozen `feat/s20-camera-research` and `camera-improve` branches are preserved.
Worktree: `<workspace>/work/hackios-camera-improve`.
Status: host-tested and cross-built; **not deployed or optically validated**.

## Behavior changed

### Black correction belongs to the rendered frame

Previously, `camera_device.c` copied the ISP into the render handoff, but the
renderer read `isp_black4`, `isp_black4_low` and band pointer ranges from mutable
process globals. Reading/calibrating frame n+1 could therefore change the black
subtraction used while rendering frame n. The state was also shared between
camera workers.

`struct isp` now contains its own per-site upper/lower black levels and band row.
The existing handoff copies them with the other ISP settings. Preview, statistics,
autofocus sharpness, flat-field measurement and full/still demosaic all read the
same snapshot. Band selection uses a raw-image row coordinate, eliminating the
old cross-buffer address-range comparisons. Existing calibration files and values
are retained; this change does not import unverified Samsung calibration.

The magnified-preview row cache now includes the correction snapshot, timestamp,
sensor, CFA and frame dimensions/stride in its key. A different correction or
reused frame allocation cannot reuse cached rows merely because the raw pointer
and sequence happen to match. Its allocation fallback also requires both row
buffers, avoiding dereferencing one failed allocation.

Default black correction remains 64 for the front and other callers after
`isp_init`. The diagnostic `isp_measure(NULL, ...)` uses the frame header's black
level instead of process-global state.

### Rear tap exposure uses the same pedestal as the picture

The rear tap meter previously subtracted a fixed 64 and omitted RAW10's low bits,
even when rendering used different per-site blacks. It now measures through the
same RAW10 cell decoder and frame-owned correction. This fixes inconsistent
metering in rear shadows and across the full-mode band boundary. The existing
front tap-meter branch is retained. No automatic full-resolution switch was added.

### Rear exposure commands obey the normal-mode quantum

After gain/line/frame limits are applied, S5K2LD normal-mode coarse integration is
rounded down to an even number of lines. This occurs after the last frame-length
clamp so an odd explicit frame length cannot reintroduce an odd exposure.
Minimum exposure stays 16 lines, the conservative margin stays 40, and gain stays
0x20..0x200. Front and non-S20 normalization behavior is preserved.

This is only the supported normal/LN-off contract. LN2/LN4 still require their
own minimum/step/margin sequence and are not enabled here.

### Rear exposure I2C failures end the stream cleanly

The page, group-hold, optional frame-length, exposure, gain and hold-release writes
now have checked return values. If a hold may have reached the sensor, release is
attempted even after a NACK. On failure, no further exposure/gain writes follow;
the loop breaks through its normal DMA drain/abort and sensor/power cleanup.
The rear is not retained warm after that failure, and the stream wrapper returns
failure even if earlier frames were received. The failing register is reported.

The existing group-hold sequence is retained to isolate error handling from the
separate stock-versus-port group-hold experiment. There are no inner retry loops.
This prevents subsequent frames being emitted under an unacknowledged request;
it does not replace the existing two-frame latency model with measured embedded
sensor exposure metadata. Initial-frame/latency verification is still required.

### Rear timing and link descriptors reflect the Samsung reference

The two rear descriptors now use 1547000000 Hz, C-PHY / three trios and settle 7.
Reported line time comes from line clocks divided by that pixel clock:
1468 ns for binned mode and 2740 ns for full mode, floored to the existing integer
ABI. Front timing remains unchanged. The actual rear receiver already used its
separate C-PHY configuration; this aligns its descriptor/metadata with that path.
The sensor's imported register tables have not changed.

## Validation completed

* Synthetic RAW10 fixtures compare frame-specific band correction with an
  independently constructed uniform-black reference in preview, full NV12 and
  still NV12 output; statistics and the rear tap meter agree too.
* Re-rendering the same frame with different correction snapshots verifies cache
  invalidation. Two render threads plus a changing reader state verify snapshot
  isolation. AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer pass.
  Leak detection is disabled for the address-sanitized fixture because existing
  renderer scratch caches intentionally persist per thread; this is not a claim
  that all renderer allocation-lifetime issues were audited.
* Every write position in the rear control transaction is fault-injected, checking
  failure propagation, skipped writes and hold-release attempts.
* Host compilation of the **actual production exposure_limits function body**
  checks 66000 requested exposures, odd explicit frame lengths, gain clamps,
  minimum/maximum bounds and preserved front/non-S20 behavior.
* All 24 Samsung research/parser/math unit tests pass.
* Software-passcode clean-build integration checks pass.
* The media development library and camera diagnostic build for iOS arm64. The
  library is signed locally with ldid. The build emits an existing CoreVideo
  deprecation warning at CVBufferGetAttachment; no new camera compilation error.
* The complete GICv2 XNU kernel builds in an isolated source tree reconstructed
  from Apple's base tag plus this branch's tracked patch/files. Camera sources
  and the new helper header match tracked xnu/files byte-for-byte. Build logs have
  existing host-tool deprecation/empty-object/linker warnings, not camera-source
  compiler warnings. No unrelated fingerprint build-tree edits were exported.

Reproduce host tests and local library build (neither command accesses the phone):

```sh
sh tools/camera/test-quality-host.sh /local/camera-test-output
sh tools/camera/build-quality-media.sh /local/camera-build-output
python3 tools/passcode/test_build_integration.py
```

The library build accepts CAMERA_SDK, CAMERA_OPENH264 and CAMERA_LDID overrides.
It does not install the library or restart a service. It signs when the local
ldid executable is available. Avoid `media-dev.sh` during offline work: that
existing script also deploys and restarts mediaserverd.

Local artifacts/logs: workspace `build/camera-quality/`. A preserved copy and
source/output checksums are stored outside git under
`discovery/s20-camera-quality/2026-11-14/` and recorded in
x1slte-camera-quality-build.json (private build manifest).

Kernel build command used in this environment:

```sh
GIC_V2=1 bash xnu/xnu-build.sh -j6 -k build HOST_GM4=/usr/bin/m4
```

The isolated build tree is `xnu-src/xnu-8796.121.2` in this worktree. Missing local
SDK/tool artifacts were linked from the existing xnu-deps installation. The build
did not change those dependencies or another source tree. The first attempt
before those local tools were linked failed during make setup; the reconstructed
kernel then completed a full build and a final incremental build.

## Deployment and remaining work

At the ownership check, the phone was held by **codex-fingerprint** for its audit
kernel work. No phone command, capture, library installation, service restart,
flash or reboot was performed. The fingerprint session's separate reboot approval
does not authorize a camera reboot.

The staged camera kernel is based on accepted master `8d584cc0` plus camera work.
**Do not flash it over the current fingerprint work as-is.** Integrate these camera
commits into the chosen current device branch and rebuild before a combined test.
The frozen research branch documents the stock/control differences before these
fixes; its hash-pinned control-audit tool intentionally rejects changed source.

Once phone ownership permits, the userland changes can be tested separately with
a reversible development-library installation, without a reboot. Verify loading,
rear/front switching, repeat capture, tap exposure and controlled dark/flat-field
fixtures. Kernel testing requires a newly approved reboot, recovery/readback and
the existing passcode repeat-lock checks. Keep binned rear mode as the default.

These changes address correctness and stability prerequisites for better shadows;
there is no measured on-phone improvement claim yet. Stock CCM/GRAS calibration
conversion, noise reduction, reliable 12 MP mode, full AF/PDAF/OIS integration and
stock HDR/night/video processing remain subsequent work.

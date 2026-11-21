# Shared Metal discovery and map presentation

## Accepted behavior

2026-11-16: user confirmed Apple Maps, Google Maps and Find My render correctly
in portrait/landscape with labels/panning in the shared trial. Ordinary apps use
one graphics configuration; newly installed apps do not need a Metal enable list.
This is evidence for those apps, not complete implementation of every Metal API.

Google Maps previously had a blank canvas without driver discovery. The runtime
function-hook approach also caused a Mach-port guard termination in the thread
patching path. Publishing the actual accelerator discovery properties avoids
that dependency for ordinary apps. The shared driver then exposed actual format,
vertex-fetch and argument-buffer incompatibilities, addressed below.

## Shared implementation

- `userland/src/graphics_compat.m`: automatically finds CAMetalLayer-backed views
  and rasterizes their immediate container, preserving opacity, orientation and
  GPS state. Uses foreground lifecycle/rotation events plus a bounded startup
  sweep; no disk logs or repeating background timer. It replaces the Maps-only
  VKMapView trial. This is a common presentation workaround; the underlying
  compositor fast-path defect has not been isolated. Offscreen composition has
  a performance cost that has not been benchmarked across every app.
- `tools/mesa/air/air_metal.c`: actual encoder blend colour, RGB10A2Unorm and
  RG11B10Float textures, Unknown-usage render targets, raw single-aspect depth
  uploads, sparse argument layouts and numeric integer-to-float vertex fetch.
  Vertex-stream conflict checking uses the Metal set index, not an internal
  flattened member slot. Combined depth/stencil uploads remain refused.
- `air_to_nir.c`: pointer-address-space fallback for omitted AIR metadata and
  reuse of free buffer slots for argument members instead of allocating above
  the highest sparse direct Metal binding (which exceeded Gallium's limit32).
- `redwoodmetal.m` and `metal_argument_indices.h`: per-draw blend state, sparse
  descriptor spans and setters that validate declared IDs rather than the count.
  Both sides of rwa_draw are336bytes; never mix the new driver with an old328byte
  library. Label pack.unorm2x16 support was already in HEAD; a stale generated
  library had needed rebuilding, not a new intrinsic patch.

Packed layouts follow Apple's specifications:
https://developer.apple.com/documentation/metal/mtlpixelformat/rgb10a2unorm
https://developer.apple.com/documentation/metal/mtlpixelformat/rg11b10float
Vertex input conversions:
https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Render-Ctx/Render-Ctx.html

## Permanent assembly

`tools/graphics/prepare-root.py` stages the paired signed driver/library and
common helper. Startup uses `metal_props auto`, which enables discovery only
when the accelerator reports `RedwoodGraphicsPolicyVersion=1`. Older kernels
retain discovery off. Kernel prerequisite remains metal=1. The existing system
RedwoodMetal.bundle symlink points into /redwood/lib; no base-system rewrite is
required. Ordinary application paths and ExtensionKit hosts receive the shared
presentation helper automatically, with no Maps/Find My names in that rule.

The kernel accelerator filters only the five graphics-discovery properties for
incompatible core renderers and MobileSlideShow. The live registry is never
modified by a read. Direct, plane-search and whole-dictionary property APIs use
the same compatibility decision. Unrelated metadata remains visible. This
replaces the rejected user-space IOKit hooks; no core-process hook is staged.
Photos retains its working fallback while its missing Metal-buffer binding is
unresolved. This is a compatibility fallback, not a fix to that unsupported draw.

`userland/build.sh` applies the assembler for GL builds by default; explicit
GRAPHICS_COMPAT=0 retains previous assembly behavior for diagnosis. Source and
tracked signed prebuilts include RedwoodMetal, libredwoodair, common presentation
and the guarded metal_props. PREBUILT assembly is covered by the root fixture.
Full source builds regenerate Mesa using mk-mesa.sh's AIR-source hash guard.
The presentation helper coalesces lifecycle notifications, avoids unchanged
layer assignments, uses the window's screen scale, and skips background scans.

## Validation

-29 Panfrost blend cases: exact expected pixels;8 unsupported cases still refused.
-490 GPU packed-format pixels: RGB10A2 and RG11B10 channel ordering/clear/readback,
  including Unknown-usage render-target allocation.
- Native argument encoder: sparse/unordered IDs,48byte span, actual sparse setter,
  undeclared-hole and duplicate refusal; host sanitizer boundary test also passed.
- GPU Short4→float4 vertex fetch: full64pixel quad, numeric conversion.
- Mixed binding metadata: direct buffer31 remains fixed; member reuses free slot0.
- Depth32Float upload and aspect-aware readback: six distinct values.
- Root assembly idempotence, preserved authentication handoff and camera rules;
  camera image/profile, passcode and AOD integration checks pass.
- User confirmed shared rendering across the three map apps. Cold deployment
  verification is recorded below once complete.

No simulated GPS position or authorization changes are part of this graphics
patch. GNSS receiver fixes were verified separately in a host-assisted lab;
standalone native Location Services completion is not claimed here.

## Deployment, regression and rollback

Private artifacts: out/graphics/permanent, including complete recovery,
boot-header and root-tree backups, manifests and loader read-back logs. The first
user-approved cold rollout exposed repeated SIGBUS in backboardd, SpringBoard,
posters and a still-running mediaserverd. Logs place faults at a stack guard
while the legacy user-space IOKit hooks were installed; exact recursion source
has not been independently proved. Photos also logged fragment_main requiring
buffer5 while only3 buffers were bound. Its SIG9 during a UI restart alone is
not evidence of a separate app exception.

Containment: disabled global discovery, removed runtime legacy-hook injection,
restarted the UI and remaining old media/poster processes. User confirmed Photos
previews and albums work again. backboardd1188 and SpringBoard1192 remained
stable across baseline checks. Do not describe this as universal Metal support.

Replacement candidate: kernel-side per-request compatibility, no C hooks,
guarded startup and optimized presentation helper. Kernel build, sanitizer
policy checks, root idempotence/auth preservation and signed-image verification
pass. The updated helper on the old kernel demonstrably leaves discovery off.
The user authorized installation and further repair reboots. The replacement
recovery head/tail and standalone boot were installed with loader write/read-back
verification, followed by a cold reboot. Image/kernel/root hashes are recorded in
private fixed-manifest.json and fixed-deployment.json. That first replacement attempt stopped in the loader; the size correction and
subsequent successful boot verification are recorded below.

The candidate preserves the base system, loader configuration, logos, camera,
AOD, telephony and user data; it updates the kernel compatibility policy and root.
A real wiped-data install has not been performed. Rollback uses saved original
recovery plus matching standalone boot header, not an older tree's rebuilt image.

### Loader-window correction

The first replacement attempt failed to reach iOS. Preflight had missed the
loader's 16 MiB kernel read limit: the raw linked kernel was 17,606,360 bytes.
The standard `xcrun strip -x` step produces 15,295,672 bytes, retaining global
symbols. The corrected candidate retains the signed boot area and root, replacing
only the kernel slot. `tools/graphics/verify-package.py` now checks that limit,
embedded hashes, root-size payload, Samsung DT table, flash chunks and passcode
flags before deployment. Signature verification remains a separate required step.
The corrected deployment and bounded hardware checks subsequently passed; see below.

### Corrected cold deployment verified

The stripped-kernel candidate was flashed with read-back checks for recovery
head/tail and standalone boot, then cold-rebooted. `uname -v` reports the new
2026-11-16 14:39:48 kernel. User confirmed Apple/Google Maps in both orientations
and Photos previews/albums. Global discovery and the shared helper survive boot.
Normal and Photos/backboardd/Weather/SpringBoard-named probes pass direct,
plane-search and dictionary property tests plus Metal device creation/fallback.
The diagnostic probes exit without launching or restarting the real services.
Core UI/media PIDs remained 9/21/57 during the postboot checks. No core fault was
seen in those captures; this is a bounded observation, not a long-term guarantee.
The phone reports no protected passcode bag, consistent with its pre-flash state;
protected-bag repeat unlocking was not exercised in this deployment.

Remaining wallpaper incompatibilities: AegirPoster rejects an extended-range
CAMetalLayer format; WeatherPoster traps in CoreFoundation's CFRelease(NULL),
with WeatherUI/QuartzCore frames. These background extensions restarted during
wallpaper prewarming. Temporary diagnostic injection and discovery changes were
restored. Do not claim the wallpaper extensions or every Metal application work.
The source/build change is accepted for the verified Maps/Photos/UI behavior.
The prebuilt kernel retains the previously committed bounded GNSS hub-poll fix;
its source ancestry is required alongside this graphics change. New uncommitted
GPS experiments are excluded from the graphics commit.

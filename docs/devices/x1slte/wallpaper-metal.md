# Wallpaper, parallax and Metal for Photos / PosterBoard (session B, 2026-11-13..14)

What changed, where it lives, and every switch. Everything here was done without a reboot:
the system image still carries the older driver, and the new one is loaded per process from `/var/jb`.

## 1. Metal driver fixes (CoreImage, Photos "Use as Wallpaper")

Source: `tools/mesa/air/` (compiler + `libredwoodair.dylib`) and `userland/src/redwoodmetal.m`,
`userland/src/redwooddrawable.m` (the `RedwoodMetal` bundle). Commits on `x1slte`: 472395a, 58d0a8c, de15d00.

| Problem seen | Fix |
| --- | --- |
| `ciKernelMain takes sampler 0 ... 0 samplers were bound`, `reads texture 1 and 1 texture was bound` | an empty texture/sampler slot gets the zero placeholder / default sampler (Metal does not refuse this; CoreImage relies on it) |
| `intrinsic: air.simdgroup.barrier` | translated; at the top level of a kernel it is a threadgroup barrier |
| SIGSEGV far 0x4c in `util_image_to_sampler_view` | Panfrost image unbind passed as `count`, not trailing slots (stale image mask) |
| `takes a threadgroup buffer whose size only the dispatch can give` | `-setThreadgroupMemoryLength:atIndex:0` -> new `rwa_dispatch_tg` -> `pipe_grid_info.variable_shared_mem`; placed after static threadgroup arrays |
| CIAreaAverage / CIAreaMinMax gave 0,0,0,0 | (a) Panfrost's texel fetch (`read()`) uses sampler 0: sampler 0 is always bound when anything is sampled; (b) `threadExecutionWidth` reports Apple's 32 (MPS reductions assume 32 lanes) |
| GPU writes to small/unaligned IOSurfaces lost | copied surfaces written by the GPU are written back after the command buffer (`-redwoodWriteBackCopy`) |
| `surface is 896x640 and the descriptor asks for 837x629` | a texture smaller than its IOSurface is accepted |
| `surface is '420f' and Metal format 10`, `plane 1 ... plane 0 only` | planes of 420f/420v surfaces import as R8 (plane 0) / RG8 (plane 1) |
| `pixel format 25`, `'1C0h'` | CoreVideo codes `L00h`/`1C0h` (R16F), `L00f`/`1C0f` (R32F), `2C0h` (RG16F), `RGfA` (RGBA32F), `L008` (R8) |
| `air.pack.unorm4x8.v4f16`, `bitcast 1 x 32 bits to 4 x 8 bits` | `air.pack.{u,s}norm{4x8,2x16}`; any same-size bitcast repacking (`nir_extract_bits`) |
| `samples texture 1 ... made without MTLTextureUsageShaderRead` | served (noted once), as Apple's GPUs do |

Check: `proposals/metal-per-process/citest.m` runs 7 CoreImage filters; on the phone they now give
the same pixels as macOS (blur edge differs by 1-2). `mtlk.m` runs MPS's
`compute_intermediate_mean_float_statistics` through the Metal API.

### Debug switches (environment of the process)
- `RWA_PRINT_NIR=1` — every shader (now kernels too) as handed to the GPU compiler, on stderr.
- `RWM_PROBE_DISPATCH=1` — after each compute dispatch, the first texel of every bound texture, in `/m2m` (`PROBE`).
- `MESA_SHADER_CACHE_DIR` — overrides the disk cache dir (the cache cannot be disabled by env: the driver sets `MESA_SHADER_CACHE_DISABLE=false`).

### Metal per process, without a reboot
- `proposals/metal-per-process/metalon.c` -> `/var/jb/usr/lib/libredwood-metalon.dylib`. Hooks the IORegistry
  reads (MSHookFunction) so Metal finds a driver in this process only; MPS failures made nil instead of abort.
- If `/var/jb/Library/Extensions/RedwoodMetal.bundle/RedwoodMetal` exists, metalon points Metal at that dev copy
  (`MetalPluginName = ../../../../var/jb/Library/Extensions/RedwoodMetal`), and the driver loads the
  `libredwoodair.dylib` next to its own executable (only when not under `/System`).
- Enabled through `kern.exec_inject` (live): append `;/var/jb/usr/lib/libredwood-metalon.dylib PROC1 PROC2 ...`.
  Currently: `PhotosPosterProvider GradientPosterExtension EmojiPosterExtension`.
  NOT for `PosterBoard` (with our driver its bold titles render as scrambled glyphs; it works without Metal),
  `CollectionsPoster` (stack overflow) or `WeatherPoster` (SIGSEGV in CoreFoundation from IOKit) -- both crash-looped with it.
- Revert: `/redwood/bin/rwsys kern.exec_inject "$(cat /var/root/exec_inject.boot2)"` (the boot's list, saved).
- Metal stays OFF system-wide (`metal_props`), because every new mediaserverd aborted with it on (2026-11-12).
- To ship: `DRIVERS=softpipe,panfrost AIR=1 tools/mk-mesa.sh` + userland build, image rebuild, reboot.

## 2. Wallpaper parallax

Source: `userland/src/wallpaper_motion.h`, `wallpaper_pose.h`, entry `inject_wallpaper.c` (branch
`x1slte-cellular`, worktree `work/hackios-b-wallpaper`; commits 868043b, 7800676, 4c951c3, 8588259).
Installed: `/var/jb/usr/lib/TweakInject/RedwoodWallpaperMotion.dylib` (filter `RedwoodWallpaperMotion.plist`, SpringBoard).

- Moves the home poster's `layer.sublayerTransform` (a view transform is cancelled by PosterBoard's replica view).
- Each 20 Hz sensor step is a 66 ms Core Animation from the presentation value: the render server glides it at display rate.
- Moves under 0.25 pt are skipped: a still phone does no wallpaper work (measured: parallax costs ~1% backboardd CPU).
- Only for Apple's collection posters (provider `com.apple.WallpaperKit.CollectionsPoster`, read from
  `_SBWallpaperSecureWindow.rootViewController.viewController.posterController.configuration.providerBundleIdentifier`;
  `posterController` has no getter, so each step falls back to the `_name` instance variable). The log says why:
  "wallpaper motion v2: poster check: ...".
  A photo poster, or our own still picture (section 3), stays still and the motion sensor is off.
- Off automatically: screen off, wallpaper hidden, Reduce Motion, Low Power Mode.

Settings (CFPreferences domain `com.redwood.motion`, user mobile, re-read every second):

| Key | Default | Meaning |
| --- | --- | --- |
| `HomePosterMotionEnabled` | true | master switch |
| `HomePosterMotionAmplitude` | 40 (0-60) | travel in points |
| `HomePosterMotionRange` | 15 (3-60) | degrees of tilt for full travel |
| `HomePosterMotionRecenter` | 3 (0-60) | seconds to re-centre after a held tilt |
| `HomePosterMotionProviders` | `com.apple.WallpaperKit.CollectionsPoster` | comma-separated poster providers that move |

Write from root: `/var/root/rwpref com.redwood.motion KEY bool|num VALUE` (string values: edit the plist).
Boot argument `sens_wallpaper_motion=0` disables it entirely. Status: `rwtouch notify com.redwood.wallpaper.motion.status`
-> "wallpaper motion v2: ..." in `/log`.

## 3. Custom wallpaper without the editor

`userland/src/custom_wallpaper.m` -> `/var/jb/usr/lib/TweakInject/RedwoodCustomWallpaper.dylib` (SpringBoard).
The newest jpg/jpeg/png/heic/heif in Files > On My iPhone > **Wallpaper** (or `/var/mobile/Library/Redwood/Wallpaper`)
covers the lock poster (UIImageView tag 0x52575750) and is mirrored to home; `Wallpaper/Home` overrides home only.
Checked every 2 s and on `notify com.redwood.wallpaper.changed`. An empty folder removes the overlay, and then the
poster chosen in Photos / Settings shows. While a picture is in the folder it wins over the Photos poster.

## 4. Wallpaper gallery ("Add New Wallpaper", Settings previews)

The gallery is filled by `duetexpertd` (`com.apple.proactive.FaceSuggestions.xpc`), which the port's launch list
leaves out. Without it the gallery is empty and PosterBoard aborts ("attempt to insert nil object from objects[1]").
- Started by hand: `/var/jb/redwood/com.apple.duetexpertd.plist` (from the image's `System/Library/xpc/launchd.plist`),
  `launchctl bootstrap system ...` (undo: `launchctl bootout system/com.apple.duetexpertd`). Lost at reboot.
- It traps in `ATXEnableMobileAssetDataVault` (a data vault this kernel cannot make): `libredwood-duet.dylib`
  (MSHookFunction, `/var/jb/usr/lib`, exec_inject `duetexpertd`) skips that call.
- `libredwood-crashlog.dylib` (exec_inject any process): signal / uncaught exception / exit() backtrace to
  `/var/tmp/crash.<proc>.txt` -- how the trap above was found.
- To make permanent: add com.apple.duetexpertd to the image's LaunchDaemons and the hook to the image.

## 5. Tools on the phone
`/var/root/citest [image]`, `/var/root/mtlk`, `/var/root/air_run` (new build), `/var/root/rwpref`,
`/var/root/rwp PROC` (rwprobe: `snap PATH`, `find Class`, `kv ADDR keypath`, `ivars ADDR`, ...).

# S20 Dynamic Island (`island=1`)

iOS 16.5's own Dynamic Island on the S20's top-centre camera hole: SpringBoard's SystemAperture, the
"Pill" status bar, Live Activities layout. Nothing is drawn by us; iOS is told the screen is a 14 Pro's.
User-verified 2026-11-12 (pill over the hole, home-bar swipe, touch normal).

## The flag

| Boot argument | Result |
| --- | --- |
| `island=1` (**default**, in `x1slte.cmdline`) | Dynamic Island |
| no `island=` / `island=0` | iPhone X notch layout, as before |

Switch it by editing `x1slte.cmdline`, then `./x1slte.sh payload "$(cat devices/exynos990/x1slte/x1slte.cmdline)"`,
`./x1slte.sh pack`, `tools/x1slte-auto.sh --recovery`. One reboot; nothing else to do.

## What it changes

All in the loader's device tree (`xnuloader/loader.c`), only with `island=1`:

| Device tree | iPhone X (off) | With `island=1` | Read by |
| --- | --- | --- | --- |
| `/product/artwork-device-subtype` | 2436 | **2556** (iPhone 14 Pro) | MobileGestalt `ArtworkTraits.ArtworkDeviceSubType` → SpringBoard turns on SystemAperture |
| `/buttons/home-button-type` | (absent) | **2** (no home button) | MobileGestalt `HomeButtonType` → BaseBoard `-[BSPlatform homeButtonType]` |

Why the second one: BaseBoard takes MobileGestalt's `HomeButtonType` (1 or 2) and otherwise calls a phone
buttonless **only if its subtype is 2436**. The iPhone X's tree has no `/buttons` node, so with the subtype
alone at 2556 iOS acts like a home-button iPhone ("press home to unlock", swipe-up opens Control Center,
a black bar at the top). A real 14 Pro's tree has `home-button-type` 2.

MobileGestalt caches its answers in
`/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist`
and does not rebuild it when the device tree changes. `init` (`userland/src/init.c`, `island_cache_sync`)
keeps the last boot's flag in `/var/db/redwood-island` and removes that cache when the flag changed, so
MobileGestalt rebuilds it from the new tree. (This needs a root image built with that `init`; with an
older one, delete the cache by hand before the reboot.)

## Pill position and size (in libredwood-sb)

iOS 16.5 hard-codes the pill in SpringBoardUIServices (14 Pro numbers): size 125.33 x 36.67 pt and top gap
11.33 pt (`-[SBUISystemApertureLayoutMetrics _minimumScreenEdgeInset]`). `userland/src/island_sb.h`,
part of libredwood-sb (SpringBoard's library on the boot image), replaces those two methods from the main
queue once SpringBoard runs, only with `island=1`.

| Setting | Default | Apple |
| --- | --- | --- |
| top gap `y` | **6 pt** (punch hole `punch-21`, user-chosen 2026-11-13 evening) | 11.33 |
| width `w` | **21 pt** (same; before: 75, preset `18pro-small`) | 125.33 |
| height `h` | **21 pt** (same; before: 28) | 36.67 |
| Live Activity height `lh` | **32 pt** (see "Live Activity height" below) | -- |

Override in `/var/jb/etc/redwood-island.conf` (e.g. `y=5 w=81 lh=34`; `off` = Apple's pill), then respring.
Keys are case-insensitive since 2026-11-13 (`Y=5` used to be ignored silently).

### Presets (2026-11-13, user-chosen names)

Saved in the repo (`tools/island-presets/*.conf`) and on the phone (`/var/jb/etc/redwood-island.presets/`).
Switch from the Mac with `tools/island-preset.sh NAME` (copies the preset to the conf, resprings; no reboot);
`tools/island-preset.sh` alone lists them and shows what the phone uses now.

| Preset | Conf | Size (w x h, top gap) | Notes |
| --- | --- | --- | --- |
| `apple-14pro` | `off` | 125.33 x 36.67, 11.33 pt | Apple's 14 Pro pill, no adjustment ("no dynamic config") |
| `18pro` | `y=5 w=81` | 81 x 36.67, 5 pt | first "iPhone 18 Pro" look |
| `18pro-adapted` | `y=5 w=81 h=30.67` | 81 x 30.67, 5 pt | 6 pt shorter (~1 mm) |
| `18pro-small` | `y=5 w=75 h=28` | 75 x 28, 5 pt | a little smaller all round (the built-in default until 2026-11-13 21:05) |
| `favorite-pill` | `w=75 h=28` | 75 x 28, Apple's 11.33 pt | the pill the user liked most (2026-11-13): 18pro-small at Apple's top gap |
| `circle-24` | `y=5 w=24 h=24` | 24 x 24, 5 pt | round, just over the camera hole (user: good) |
| `circle` | `y=6 w=22 h=22` | 22 x 22, 6 pt | round, a bit smaller, same centre |
| `punch-21` | `y=6 w=21 h=21 lw=0 lh=32` | 21 x 21, 6 pt | punch hole, 32 pt tall during activities (the default 21:05..22:10) |
| `punch-live18` | `y=6 w=21 h=21 lw=81 lh=36.667` | 21 x 21 at rest; **81 x 36.67 while a Live Activity runs** (the `18pro` pill) | user's "preset 2"; **the built-in default** since 2026-11-13 22:10 |

On the phone by hand: `cp /var/jb/etc/redwood-island.presets/NAME.conf /var/jb/etc/redwood-island.conf; /var/jb/usr/bin/sbreload`.
Turning the island off altogether (iPhone X notch) is the boot argument `island=0`, not a preset.
`sbreload` restarts SpringBoard twice in a row here; the second start (~10 s) is the one that reads the conf.
Keep the pill over the camera hole: the hole's bottom edge sits roughly 25-28 pt below the top, so a round pill much
below ~22 pt would show the hole's edge (estimate; 24 pt user-checked as covering it, 22 pt to be checked).
The conf is on the data volume: it survives reboots and new boot images, but a fresh install starts with the
built-in default, which is `punch-21` from the boot image after 2026-11-13 21:05. A conf file defines the whole pill:
what it leaves out (e.g. `h`) is Apple's value, not the built-in default.
Log: `/var/mobile/Library/Logs/redwood-island.log`. 1 pt on the S20 panel ~ 0.18 mm.

Not as a /var/jb tweak: any library from /var/jb loaded into SpringBoard fails the kernel's
`map_with_linking_np` code-signature check and costs SpringBoard its CS_VALID; if that lands before
SpringBoardUI's +load, SpringBoardUI throws "This process should not be linking or loading
SpringBoardUI.framework" and SpringBoard crash-loops (timing-dependent). (userland/src/island_tweak.m
and island-install.sh are that withdrawn version.)

### The user's two punch-hole presets (2026-11-13)

| | Preset | At rest | While a Live Activity / system element shows |
| --- | --- | --- | --- |
| **1** | `punch-21` | 21 x 21 pt round, 6 pt from the top | 32 pt tall, at least 32 wide (`lh` default) |
| **2** (built-in default since 2026-11-13 22:10) | `punch-live18` | the same | 81 x 36.67 pt: the iPhone 18 Pro pill (`lw=81 lh=36.667`) |

Switch: `tools/island-preset.sh punch-21` or `tools/island-preset.sh punch-live18` (a respring, no reboot).

**Changing the active (Live Activity) size:** edit `lw=` (width) and `lh=` (height) in
`/var/jb/etc/redwood-island.conf`, then respring. Examples: `lw=75 lh=28` (18pro-small), `lw=81 lh=30.67`
(18pro-adapted), `lw=125 lh=36.667` (Apple's 14 Pro). Without `lw` the width is the resting width (at least `lh`);
`lh=0` keeps activities as small as the pill. To keep a new combination, save it as a file in `tools/island-presets/`
and on the phone in `/var/jb/etc/redwood-island.presets/`.

**Entry animation** (user's choice, 2026-11-13 22:00): only the way in (punch hole -> live size) is handled; the way
out is iOS's own. The switch to the live size is made on the main queue's next turn inside a 0.4 s UIKit animation of
the element's window (island_note_mode, commit 04f9b79): the user preferred it to an immediate switch (it reads as two
pills, the small one jumping) and to an eased-size version (919b123: the height did not apply properly).

### Live Activity height (`lh`, 2026-11-13, user-verified)

SpringBoard sizes a compact or minimal Live Activity from the resting pill's own size (`sensorRegionSize`); it never
asks `minimumCompactSize` (not among SpringBoard's selectors). With a 21 pt punch hole every activity was 21 pt tall.
`island_sb.h` follows the elements' layout modes (`-[SAUILayoutSpecifyingElementViewController setLayoutMode:reason:]`
and the `forTargetWithOverrider:` variant; `SAUIStringFromElementViewLayoutMode`: -1 removed, 0 none, 1 minimal,
2 compact, 3 custom/expanded). While any element is 1-3 the pill is `lh` tall and at least `lh` wide, and every mapped
getter (corner radius, inter-sensor rect, minimal sizes) follows; when all are 0/-1 it is the w x h circle again.
SpringBoard asks the size again on every layout pass (137 calls in the first minute), so the change shows at once.
System elements (charging, silent switch) grow the same way. `lh=0` in the conf: activities as small as the pill.
The log shows each change: `island: SAUIElementViewController 0x... layout mode 2 -> live height`.

The rest of the geometry (minimise animation, radius, minimal views) follows the pill by value since 2026-11-13
(B's proposals/notch/island-geometry-hooks.patch, now in `island_sb.h`).

## Status bar

The phone keeps UIKit's notch layout (_UIStatusBarVisualProvider_Split1125; the screen is the iPhone X's), shaped by
`src/statusbar_shift.h` (in SpringBoard via libredwood-sb and in every app via libredwood). Settings in
`/var/jb/etc/redwood-statusbar.conf` (one line, e.g. `lead=12 bolt=0`), read at process start: respring / reopen apps.

| Key | Default | Meaning |
| --- | --- | --- |
| `lead=N` | **12** (2026-11-15: "remove the left padding") | moves the time N/2 pt nearer the left edge; the battery side is given back so it stays put. `lead=0`: Apple's place |
| `bolt=0/1` | **0** | island phones only: `0` no bolt in the battery icon (the island shows charging), `1` keep it |
| `trail=N` | 0 | battery side N pt further inward |
| `notch=N` | 0 (Apple's 209) | notch width reported to the layout (wider side areas, e.g. 125) |
| `cut=N` | 0 | DynamicSplit layout only (not used on this screen) |

Island phones (detected by the device tree's `/product artwork-device-subtype` 2556 or 2796, the value `island=1`
sets; MobileGestalt only as a fallback) also get, with no setting:
- **plain time**: the status bar's background-activity entry reads as disabled, so a call, recording or navigation is
  shown by the island only, not as a coloured pill around the time;
- no bolt in the battery icon (see `bolt`).
With `island=0` (or another device) none of this is hooked. Log: `/var/mobile/Library/Logs/redwood-statusbar.log`
(`island: artwork subtype 2556 (island 1)`, `background-activity pill off (entry isEnabled)`).

Traps:
- MobileGestalt answers `ArtworkDeviceSubType` only to entitled processes: SpringBoard's and apps' libraries got nil,
  so the first version never armed. Read the device tree (IOKit) instead.
- The mic/camera privacy dot still sits at the notch layout's place (above the signal); moving it into the island is
  not done.

## Known issue

**An app does not shrink ("minimise") into the island** with a resized pill (user report 2026-11-13 with `circle`;
the user says it also happened earlier with other sizes). Test 2026-11-13: with `apple-14pro` (no hook) the app DOES
shrink into the island -- so the cause is our size hook, not rendering. Why: iOS has ~20 island sizes in
SBUISystemApertureLayoutMetrics and only `sensorRegionSize` (and the top inset) are changed; the minimal/compact sizes,
`_inertSize`, the corner radius and the inter-sensor rect stay at the 14 Pro's 125 x 36.67, so the transition no longer
lines up. Apple's values (read on the phone): sensorRegionSize/_inertSize/minimumCompactSize 125 x 36.667,
minimumMinimalSize/compactConcentricAreaSize 36.667 x 36.667, maximumHorizontalMinimalViewSize 45 x 36.667,
maximumLeadingTrailingViewSize 88.333 x 36.667, maximumCompactSize 301.667 x 36.667, minimumContinuousCornerRadius 18.333,
interSensorRegionInWindowSpace {213.333, 11.333, 9, 36.667}, minimumScreenEdgeInsets 11.333.
**Status 2026-11-13 21:05:** the fix is in `island_sb.h` and on the phone; the user reported everything working.
Original note: `proposals/notch/island-geometry-hooks.patch` maps
every geometry getter by value (36.667 -> h, 125 -> w, 18.333 -> h/2, 11.333 -> y). Needs a root.apfs (libredwood-sb)
flash + reboot, then `tools/island-preset.sh circle` and the timer test. Until then: `apple-14pro` keeps the animation,
the other presets lose it. Phone on `18pro-small` (user's choice, 2026-11-13 18:27).


With the 81 pt pill, the charging banner's battery number is cut (shows "%" without "100"). Not yet
checked at Apple's width. Likely the trailing label is laid out against the narrower sensor region.

## Live Activities

The Clock timer shows in the island (user-verified 2026-11-12, after a cold boot). The first one after a boot
can take a while to appear (the Live Activity services are still starting), so give it a moment before
calling it broken. The stopwatch shows too (user-verified).
Live log on the phone: `ssh s20 /var/jb/usr/bin/oslog` (Procursus `oslog`, installed 2026-11-12).

## Checking it

```
ssh s20 /redwood/bin/mg_test HomeButtonType oPeik/9e8lQWMszEjbPzng   # 2, ArtworkDeviceSubType 2556
ssh s20 /usr/sbin/ioreg -p IODeviceTree -l -w0 | grep -E "home-button-type|artwork-device-subtype"
```

## Notes

- Not an overlay like Dynamic Peninsula; same mechanism as DynamicCow / Nugget (ArtworkDeviceSubType 2556),
  plus the home-button answer, which on real iPhones comes from their own device tree.
- The screen iOS renders stays 1125x2436 (iPhone X); the pill is placed by points from the top centre and
  covers the S20's hole. Its size is Apple's (bigger than the hole, as on a 14 Pro).
- How it was found: `ipsw` 3.1.745 on the 20F66 iPhone10,3 shared cache (no `.symbols` file in it:
  a copy of the main cache with the expected UUID stands in). BaseBoard `sub_18dd64384`
  (`cmp w0, #0x984`); libMobileGestalt `sub_19c53a248` (`IODeviceTree:/buttons`, `home-button-type`).

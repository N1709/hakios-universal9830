# S20 Always-On Display (AOD)

## Final state (15 November 2026, A) — read this first

Native iOS 16.5 Always-On Display on the SM-G980F, user-verified. Settings → Always-On Display (row under Display & Brightness):
(**default on** since 2026-11-15 10:10, wallpaper and notifications in AOD **default off**; a missing or unreadable preference file reads as these defaults, `tools/aod/config.h` `rw_aod_default`). In x1slte/main (6b629ecd and later); the sections further down are
the history of how it got here.

| Part | Where | What it does |
|---|---|---|
| AOD service + Settings | `tools/aod/alwayson.m`, `settings.m` → `/redwood/lib/libredwood-alwayson.dylib` (clean image, `prepare-root.py`); on the current phone also `/var/jb/usr/lib/TweakInject/RedwoodAlwaysOn.dylib` | Lets iOS's own AOD run (S20 support, Apple's pseudo-flipbook live composition instead of the missing CAFlipBook backend); applies the saved settings; revokes authentication on InactiveOn (passcode still asked) |
| Panel dimming | kernel `kern.redwood.display_aod` (`MiniIOMobileFramebuffer.cpp` `redwood_fb_aod`, `aodLevel`; sysctl in `redwood_dbgport.c`) | iOS's AOD brightness never reaches the driver (AOD stayed at 40-55 nits). The service sets 1 on InactiveOn: AOD sits at **30 % of the slider position** the screen had when AOD began (slider curve 2 + 418 s^2.2 nits: 100 % -> 30 %, 50 % -> 15 %, 25 % -> 8 %; floor = the panel's 2 nits), then held: auto-brightness wobble during AOD (dark rooms) no longer moves it; only a 2x / 1/2 change of light takes it again. 600 ms fade in, 200 ms out, exact level back on wake. Live change, no reboot: `rwsys kern.redwood.poke "fb.aod 25"` (10..60); boot args `fb_aod_pct`, `fb_aod_max`. Check: `rwsys kern.redwood.poke fb.aod`. Boot args `fb_aod_pct`, `fb_aod_max`; values 2..60 = fixed ceiling in nits |
| Black fade on lock | `alwayson.m` `wallpaperForAOD` | With the AOD wallpaper off, locking from the home screen/an app fades a black layer (0.35 s) over the lock screen's wallpaper (`SBCoverSheetPanelBackgroundContainerView`), under the clock; removed on wake. Locking from the lock screen keeps iOS's own fade (user's choice) |
| Lift to wake from AOD | `userland/src/motion_wake.m` (redwood-motiond, root image) | AOD keeps DisplayOn/displayStatus set; the daemon reads the AOD status notification (`tools/aod/state.h`) and treats requested+completed InactiveOn as asleep |
| Low Power Mode | `alwayson.m`, `motion_wake.m` | AOD and raise-to-wake are suspended while Low Power Mode is on; the saved setting is kept and comes back after |
| Idle timer | `userland/src/inject.c` | No lock-button presses while AOD shows (in source and clean images; the phone's current root still has the older libredwood) |
| Framebuffer fixes | `MiniIOMobileFramebuffer.cpp` | Fade state read under the DSIM lock (no stale fade-out after a wake); swap signal/cancel never reach panel power |

Status/diagnostics: `ssh s20 /var/jb/redwood/bin/aod-preview status` (requested/completed backlight state,
saved settings), `sysctl kern.redwood.display_aod`, SpringBoard log lines `RedwoodAOD: will N / did N`
(0 off, 1 AOD, 2 awake). Tests: `python3 tools/aod/test_*.py` (root assembly, swap dispatch, fade/wake races,
engine init), `tools/aod/test_state.c`, `tools/aod/panel/test_alpm.c`.

How it behaves (researched with logs, all expected):
- ~2-6 s into AOD iOS ends live rendering: backboardd "Set Blanked: YES / Display 1 power off" (DCS 28). The panel
  keeps showing the AOD picture from its own memory, so AOD stays visible at no cost; minute updates re-render briefly.
- Waking after that point plays iOS's fade-in-from-black (performWake's fade-in is hard-coded on): a short black
  moment, button or tap alike. The only fix is keeping live rendering on through AOD (battery): left by choice.
- A tap in AOD wakes through the port's tap-to-wake (inject.c injects a side-button press).

Traps:
- Never hook SpringBoard from a plain /var/jb dylib outside TweakInject; the clean image carries the root-image copy
  with a shared duplicate guard (both may load; only one installs).
- `sync` on the phone after installing to /var before any flash/reboot: x1slte-auto.sh's cold restart drops unsynced
  writes.
- main is reset to x1slte by other sessions' workflow: land AOD work in x1slte.

Open (not started): Samsung's panel AOD mode (ALPM/HLPM, `tools/aod/panel/`, inactive prototype; keep 120 Hz when
awake per the user), a deeper AOD refresh drop (the mode switch is one-way per boot today).

---

## History: native AOD appearance test — software only

User request: test the iPhone14Pro-style AOD display with a SpringBoard refresh,
without panel/kernel changes or a hardware reboot. The user explicitly authorized
the test, but another agent retains phone priority. Do not interrupt that session.

## What this tests

The actual iOS16.5 native method
`-[CSCoverSheetViewController _updateAppearanceForAODTransitionToInactive:]`.
In the locally owned20F66 iPhone10,3 cache its unslid address is `0x1c48950e0`.
The method creates a UIKit animation; its block starts at `0x1c489517c`, changes
CoverSheet appearance components, and has a corresponding normal-appearance path.
Calls include appearance components, legibility, wallpaper/notification choices
and content-view presentation. The inspected path is separate from the
BacklightServicesHost hardware-AOD state machine.

The tweak checks the native implementation address plus image slide and ABI
before calling it. It does not patch that method or globally enable a capability.
Only SpringBoard loads the test. It is idle until explicitly armed from root.

**This is not a full AOD service enablement.** It tests native appearance only.
The display remains under its existing sleep/brightness policy and may turn off
normally during the test. No clock replacement, screenshot overlay or imitation
of the native rendering is supplied. If the native rendering requires additional
scene/backlight state, it may show only part of the effect or none; that is an
outstanding test result, not reason to silently fake the result.

Full AOD still needs policy/capability integration, BacklightServices scene and
frame scheduling, platform/kernel power-state support, panel low-power entry/exit,
reliable wake, privacy handling and measured power behavior. Merely enabling a
MobileGestalt capability does not establish those parts.

## Test behavior

- Root creates a fresh30-second one-shot arming token. Replaying an already-used
  token in the same SpringBoard process cannot extend/restart the test.
- Start requires the native locked keybag state, a locked UI, awake display,
  visible main lock-screen page, and neither inline nor full passcode entry.
- The native AOD appearance is requested for at most12seconds. An observing
  gesture recognizer ends the preview on touch without recognizing/cancelling
  the original touch. A watchdog also ends it on display-off, passcode visibility,
  leaving the main page, unlocking, timeout, or an explicit `end` notification.
- End calls the native appearance transition with false, removes the observer
  and invalidates the watchdog. Native exceptions produce fixed diagnostic text;
  credential or view contents are not dumped.
- No change to UIScreen brightness, idle-sleep assertions, lock timers, native
  authentication, saved passcode/keybag, MobileGestalt, BacklightServices,
  refresh rate, DECON/DSIM/panel commands, boot arguments or kernel.
- The exact framework method has not yet been invoked on the phone by this
  session. Host compile/tests do not prove native appearance or restoration.

## Build and package

```sh
tools/aod/build.sh
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/aod/test_token.c -o out/aod/test_token
out/aod/test_token
python3 tools/aod/package.py
```

Uses the local iOS16.5 SDK and Apple clang; no full userland/kernel rebuild.
Artifacts and hash manifest are under ignored `out/aod/`. The tar bundle contains
only the test dylib, SpringBoard filter, control helper, installer, this document
and manifest. It contains no Apple binaries or personal data.

## Device procedure, when the priority agent releases the phone

Acquire the shared phone lock as this session, or have the current holder run the
test. `device.sh` requires explicit `CLAUDE_SESSION` matching the existing owner;
it refuses another holder before making any SSH connection. No forced takeover.

1. Record passcode/relock baseline and current SpringBoard PID. Do not request,
   print or record the passcode. The user enters it only on the phone.
2. `CLAUDE_SESSION=codex-aod tools/aod/device.sh install`. This copies only the
   independent idle tweak and helper, preserving the passcode/relock tweak.
3. `CLAUDE_SESSION=codex-aod tools/aod/device.sh respring`. This terminates only
   SpringBoard so launchd reloads it. It is not a hardware/kernel reboot.
4. Confirm SpringBoard returns and software-passcode unlock/relock works. Native
   appearance has still not been requested; loading the tweak is idle.
5. User locks, wakes and leaves the main lock screen visible. When ready, run
   `CLAUDE_SESSION=codex-aod tools/aod/device.sh begin`. Observe native wallpaper,
   clock, widgets and notification appearance. Do not promise all will change.
6. Verify normal appearance returns on timeout, then repeat a fresh test and
   verify touch abort. Verify display-off/passcode transitions exit as well.
7. Confirm passcode unlock -> lock -> passcode unlock still works. Record which
   native effects actually rendered and whether restoration was complete.

To stop: `CLAUDE_SESSION=codex-aod tools/aod/device.sh end`.
To remove: end first, remove only `RedwoodAODPreview.dylib` and its plist from
`/var/jb/Library/MobileSubstrate/DynamicLibraries/`, plus
`/var/jb/redwood/bin/aod-preview` and `/var/tmp/redwood-aod-preview/arm`.
Coordinate a SpringBoard refresh to unload the removed tweak. Leave every other
tweak, keybag, passcode, image and phone-owner session alone.

If the native method/ABI differs, start is refused. If UI restoration is wrong,
stop and remove the test before trying another capability or backlight change.
A successful preview must not be documented as working low-power AOD.

## Live installation and timeout check (15 November, 03:07)

The user explicitly transferred phone priority from A to codex-aod. The independent
tweak and helper were installed, and SpringBoard refreshed (PIDs9 ->337 ->374);
no kernel flash or hardware reboot command was issued. Direct notify status
confirms the tweak loaded and found the pinned native appearance method.
A manually armed begin returned `declined-lockscreen-state`: **the native AOD
appearance transition has not yet been invoked or visually validated**. No
capability, brightness, authentication or passcode-setting change was made.

The user reported missing passcode prompting after the refresh and subsequently
reported it returned. Before that report, read-only status showed native MKB1,
raw AKS0x5, protected private saved bag present, softpasscode1/autounlock0 and the
existing passcode-relock tweak/filter intact. Cause is not established. The AOD
request had been refused; do not claim it ran or that a keybag repair was applied.

User also requested screen timeout. The documented mode1 setting was applied,
but their first12-second test stayed on. Source and copied installed-library
disassembly confirm mode1 is the wake-only heuristic; any later input disables
that branch until another detected wake. A read-only diagnostic of three globals
in the installed backboardd library showed touch_mode0x101, button service present
and input_idle274s; no process memory was written.
Switched `/var/jb/etc/redwood-idle.conf` to documented `mode=2`, preserving the
user's effective Auto-Lock60seconds. The first SSH write failed during USB loss;
a subsequent write/read verified mode2. It reloads on the next detected wake.
Current boot console logs show the10-second untouched-wake timeout firing, and
independent display state/touch_mode0x1fe confirm display-off. After-interaction
60-second behavior still needs user confirmation. Reference: x1slte-background.md.

Uptime reset while USB was unavailable; this session issued no reboot command.
Reset log reads PINRESET with PMIC WTSR. Those bits alone do not identify the
cause. User was asked whether it was a manual restart. Keep AOD idle while
resolving that observation. Fresh boot loads the idle test but does not arm it.

The helper now has `status`, reporting only native keybag flags, display-awake
and preview state. Native notify states distinguish ready, declined, active
appearance-requested, normal-appearance-requested, unsupported and exception.
These are operational diagnostics, never authentication or proof of rendering.
Deployment uses explicit-file SCP because the phone's tar could not find gzip.
Updates were staged separately and atomically renamed before refreshing so mapped
executables were not overwritten in place.

## Persistent native AOD and restart fix (15 November, 08:30)

The manual native appearance preview was subsequently visually confirmed. A
separate `RedwoodAlwaysOn` module now integrates Settings and native SpringBoard
Always-On policy on this exact iOS 16.5 cache/S20 profile. It is default **off**.
The installed preference persists in
`/var/mobile/Library/Preferences/com.redwood.alwayson.plist`; no enabled preference
is shipped. Settings exposes Enabled, wallpaper and notification switches.

The module adapts two iPhone capability checks: SpringBoard's platform provider
and BacklightServicesHost's OS-interface provider. Original OS support remains
reported separately from the effective port capability. Method addresses and ABI
are pinned; unsupported versions decline installation. This is not a global
MobileGestalt override. The native policy owns lock/timeout, wake, suppression
and appearance. Native authentication is revoked on inactive/off transitions,
preserving the existing software-passcode bridge; no authentication-success stub
or keybag mutation is introduced.

Initially enabling the missing OS capability entered `InactiveOn` (native state
1) but repeatedly crashed SpringBoard with SIGTRAP. A temporary diagnostic
captured PC `0x1eebbd068` in BacklightServicesHost, inside
`BLSHEngineRenderFlipbookSession`'s completion path. Local cache disassembly
identifies the fatal condition as `renderedFrame != nil`. CAFlipBook had supplied
no rendered frame. Do not bypass this assertion or fabricate a successful frame.

The fix selects Apple's existing `BLSHPseudoFlipbook` live-composition path using
`BLSHAlwaysOnPresentationEngine setUsePseudoFlipbook:` after native initialization,
and preserves that selection on later setters. Apple's own frame/presentation
machinery remains in use. The user confirmed AOD stayed visible without a restart
on the next test. Telemetry independently showed native policy enabled, state1,
the same SpringBoard PID4270 across observations, and no additional diagnostic
trap record. Repeat unlock/lock validation is tracked separately in the handover.
The temporary signal diagnostic is excluded from the normal build.

Build with `bash tools/aod/build.sh`, package with `python3 tools/aod/package.py`.
`out/aod/alwayson-bundle.tar.gz` contains only the module, filter, root helper,
installer, documentation and a SHA-256 manifest. The installer requires the
existing passcode/relock bridge and explicitly disables AOD before installation.
It does not refresh SpringBoard or reboot. This standalone bundle is not yet
integrated into clean root-image assembly or the final TWRP release package.

To disable immediately: `/var/jb/redwood/bin/aod-preview disable` over root SSH.
For removal, disable first, remove only `RedwoodAlwaysOn.dylib` and its matching
filter from the tweak directory, then refresh SpringBoard when phone ownership
and call state allow. Keep the existing software-passcode tweak intact.

Validation: iOS build with warnings as errors; host config regression
(`test_config.m`) checks default-off and rejection of malformed, world-writable,
symlink and FIFO inputs. Token lifetime tests remain separate. A visible change
in animation or brightness is **not** a refresh-rate or power measurement.

## Samsung panel low-power follow-up

Panel ALPM is **not implemented or tested** by this module. Live composition can
retain native AOD while still spending normal compositor/display power. The
current framebuffer has binary `displayPower(bool)` and no distinct retained
AOD mode. Do not route AOD through its existing display-off path: that masks
frames and changes clocks for a fully off screen.

Samsung's locally available GPL source at commit `69515fbb7`,
`drivers/video/fbdev/exynos/panel/s6e3hab/s6e3hab_hubble1_a3_s0_panel.h`, defines
`hubble1_a3_s0_alpm_enter_cmdtbl` (line3148) and exit (line3175). Entry programs
LPM AOR, gamma update, a60Hz-frame delay, mapped low-nit parameters, ISC, AVS,
LPM mode and AVC2 with level2/3 keys. The driver enforces the panel-specific
minimum delay from panel-on before entering ALPM (the table specifies124ms),
switches the brightness subdevice, changes regulator voltage and optionally enters
the panel AOD engine. Exit restores
LPM-off nit, ASWIRE, AVC2 and normal display control. Packet parameter offsets
(e.g. AVS0x28, ASWIRE0x53, AVC2 offset0x09) must be respected; simply sending their
payload bytes is insufficient. Brightness table mappings and panel revision must
match the actual device; other Exynos990 panels need their own profiles.

Next implementation needs a serialized active/AOD/off framebuffer state machine,
verified mode/timing handling from120Hz, retained valid content and clock policy,
then full normal gamma/brightness/refresh restoration before the first wake frame.
Disable/revert, touch/button wake, repeated transitions and charging must be tested.
Measure actual refresh/panel power separately. No DSI writes, kernel flash or
hardware reboot were performed for the native-service fix. A kernel installation
and reboot require fresh explicit user authorization.

### Clean-image integration (15 November, follow-up)

S20 root assembly now runs `tools/aod/prepare-root.py` after writing inject.conf.
It copies tracked `libredwood-alwayson.dylib` and the control helper into the image
and adds exactly one targeted SpringBoard/Preferences injection rule. Repeated
assembly preserves other injection rules. Missing required AOD prebuilts fail
assembly rather than silently dropping the feature. No user preference is seeded;
new installs remain off, and existing settings survive image updates.

Both source compilation in `userland/build.sh` and the macOS GPU build path
refresh the module/helper. Root-image and standalone Settings controller classes
have distinct runtime names; shared service/Settings guards avoid duplicate hooks
when the existing data-volume tweak is present. The root-image module supplies
its own inactive/off passcode revocation and still retains the core passcode chain.
`python3 tools/aod/test_build_integration.py` and the required software-passcode
assembly regression pass without firmware or an existing `/var/jb` tree. The
complete multi-gigabyte image has not been rebuilt or flashed in this step.

A portable, inactive ALPM protocol prototype and exhaustive transport-failure
regression now live in `tools/aod/panel/`. It refuses mismatched panels,120Hz,
off/pending display states and missing normal-settings backups; cleanup restores
or latches a fault. It is not wired into the kernel and sends no live commands.
Stock investigation additionally found distinct update-capable doze vs deeper
host suspend and a VDDR1.8V→1.5V reference-board requirement; the panel README
records exact source locations and remaining integration contracts.

### Defaults correction

Both **Always-On Display** and **Show Wallpaper** default off. Missing or invalid
preferences leave both off; either can be explicitly enabled in Settings. The
user also requested both off on the current phone: saved values and settled
native policy were verified off without a refresh or reboot. Updated tracked
prebuilts carry these defaults into clean builds.

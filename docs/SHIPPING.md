## 2026-11-21 — 0.1.3 and hosted full-package workflow

- Bump the shared release.conf version to 0.1.3. Local and Actions packaging use it.
- Add a GitHub-hosted macOS option to Private TWRP release, with isolated tool setup,
  reviewed board/bootstrap archive transport and the unchanged build.sh release entry point.
- Full ZIP upload is checksum-verified; public output requires explicit opt-in; ordinary CI still only builds
  the kernel and Restore ZIP. No version tag or public release is created.
- Hosted runtime verification is pending: GitHub rejected current jobs before execution
  due to account billing/spending limits. No paid spending settings were changed. The owner subsequently made the repository public
  and explicitly approved a publicly downloadable full ZIP.

## 2026-11-21 — Keep the Live Photo default off on first Camera launch

- CameraUI 16.5's first-launch migration overwrites DesiredIrisMode unless
  CAMUserPreferenceForcedLivePhotoOnTenTwo exists (cache address 0x1cdccd898).
  Seed this and CAMUserPreferenceDidPerformAutoLivePhotoUpgrade alongside the off/preserve settings.
- A separate .redwood-camera-defaults-v2 marker repairs existing installs once without
  reapplying keyboard, location or region defaults. Subsequent Camera choices remain user-controlled.
- Validation: inspected the installed-version CameraUI migration code; iOS arm64 compiler
  syntax check passed (two existing warnings); all 61 release tests passed. Device verification pending.

## 2026-11-20 (B) — SIM and data survive a clean install

- What: a clean data volume had no modem files, so no SIM/data. redwood-cellulard now runs cell-provision when any
  is missing: the modem image, TOC, BOOT, boot info and download plan from the phone's own RADIO partition, and NV
  (nv_data.bin) from its EFS ext4 file system, all read-only (nothing Samsung's shipped or committed). Data APN from
  /var/root/cell-apn (default jionet). Docs: docs/devices/x1slte/cellular/provisioning.md (other regions/carriers),
  docs/clean-install.md (the rule).
- Tested on the phone: all six files moved aside, reboot: made by the phone, Jio 4 bars + data in ~20 s.
- Commit 4efe500f (main).


## 2026-11-20 (A): Touch ID without the iPhone 8 cache, and on a clean install
- `libBKDM1` (Touch ID module) ported onto the iPhone X cache: `tools/touchid/fetch-bkdm1.sh` (DyldExtractor +
  `relink_bkdm1.py` + `bkdm1-map.json`); biometrickitd opens the sensor; enrolment verified on the phone.
- Clean install: redwood-fpd puts the TA set back from the image (local builds only, never SHARE=1) and the sensor
  calibration from the phone's sec_efs (`cell-provision --fp`, read-only, byte-identical).
- Unlock still NO MATCH (the TA's matcher); cause and plan: docs/devices/x1slte/touchid-port.md.

## 2026-11-20 (A) — 0.1.1 with iPhone's boot and charging pictures, installed from the Mac with no key press

- What: the release ZIP (9d9e8dc5) carries iOS's boot logo and iBoot's charging pictures (lpm.jpg = charging,
  low_battery_alert.jpg = battery empty; Apple's art from the IPSW at build time, tools/bootlogo/mkcharging.py);
  the installer swaps them in up_param and keeps Samsung's as .stock (Restore puts them back).
  twrp-install.sh asks once on the computer (yes / ERASE) and the installer takes that approval instead of Volume Up;
  TWRP's own Install still needs the key. tools/install/flash-twrp.sh: TWRP from iOS with no buttons (heimdall
  restarts the phone, misc's boot-recovery starts TWRP; not yet run on the phone).
- Tested on the phone 2026-11-20: install "Approved on the computer", pictures set, iOS up 40 s after the automatic
  restart, data and fingerprint kept. Not yet seen: the charging picture with the phone off (needs the charger-boot
  loader fix, feat/offmode-charging).

## 2026-11-20 (A) — 0.1.1 installed on the phone through TWRP (update path, data kept)

- What: TWRP (Artisan 3.7.1) put into recovery with heimdall from Download Mode; backup of every partition except
  userdata to the SSD (7.7 GiB, 9 min); `twrp-install.sh` with the full-size 0.1.1 ZIP (IOS_DATA_GB=all, main 724e65cd):
  super, recovery, boot written and read back; iOS up 40 s after `adb reboot`, fingerprint template and data kept.
- Found and fixed on the way: twrp-backup.sh copied the whole disk twice (TWRP lists sda under by-name) and crashed
  without sdf; the Volume Up prompt never shows on the phone (now bell + spoken prompt on the computer); the 64 GB ZIP
  correctly refuses a phone with a full-size data volume (fresh install only); CI kernel job: macos-15 + GNU tar.
- Open: the release ZIP does not carry the Apple splash (installer supports it, tools/release never sets it); a fresh
  clone cannot build without the private inputs (docs/release/building.md); our own TWRP (feat/twrp-x1s).

## 2026-11-19 (B, ~22:15) — Touch ID UNLOCKS from the lock screen

- What: a finger unlocks the phone on the lock screen (user-verified). Made it work: Apple's phase 2 ("adjust your
  grip") is no longer ended by Apple's Cancel between steps (2-minute window), an accepted phase-2 touch is not
  followed by a "bad image" event, the sensor keeps listening while iOS waits (8 x 15 s runs, then stops until asked
  again), the ring shows during phase 2 and has a faint shadow for bright wallpapers. Re-enrol once after updating.
- Commits: 54c892f1, 279fef5d (main). Boot args: touchid=1 (default).
- Open: read the log of the first matches; the 5-try lockout counting 0x1d rejections.

## 2026-11-19 (A) — Touch ID: the lock screen is safe right after enrolment

- What: Apple's phase 2 ("adjust your grip") counts any touch on the sensor, without matching. Its window (2 min,
  renewed by every touch) outlived Settings: locking right after enrolment let any touch (any finger) unlock the phone,
  and the lock-screen fingerprint icon was hidden. Now locking the phone (com.apple.springboard.lockcomplete) ends
  phase 2 at once, and each 15 s sensor run re-sets the icon (userland/src/redwood-fpd.c, main a15c830d).
- Tested on the phone 2026-11-19 (user): enrol, lock -> icon shown, no unlock without a real check.
- Open: real matching on the lock screen (SENSOR MATCH) still mostly NO MATCH.

## 2026-11-19 (B) — Apple account relay survives reboots; Continuity groundwork (parked)

- What: the App Store / Apple account relay now starts at every boot (its job is in the image; rwsys is built). It
  used to sit in /var/jb/Library/LaunchDaemons, which our launchd never reads: "MID is invalid" after every reboot.
  sharingd sees the S20 as hotspot-capable (SIM ready, cellular data on); identityservicesd is in the system image and
  passes Anisette through the relay. Instant Hotspot itself is parked at Apple's activation wall (no APNs push token).
- Full write-up: docs/devices/x1slte/continuity.md.
- Tested on the phone 2026-11-19: App Store sign-in after the relay fix (user); sharingd "tethering support = YES".

## 2026-11-19 (B, from A's start) — Native Personal Hotspot

- What: iOS's own Personal Hotspot works on the S20. Settings → Personal Hotspot / Control Center switch it; "Jesvi IOS"
  with WPA2; devices get 172.20.10.x and internet over Jio (CLAT46 + NAT); the Dynamic Island shows the hotspot icon
  while devices are connected; Macs treat it as a phone hotspot (mobile data). Comes back by itself after a reboot.
- Pieces: driver AP + WPA2 host authenticator (redwood_bcm4375.c), apple80211 HOST_AP_MODE + STA arrive/leave + vendor
  element (redwood_apple80211.c), libredwood-mis (misd: CommCenter answers + network watcher), libredwood-mgcell
  (Settings row), hotspot_sb.h (island icon), /redwood/bin helpers (rwdhcpd, rwpf, clat-test, hotspot-up.sh, misctl).
  Handoff: docs/devices/x1slte/hotspot.md.
- Tested on the phone 2026-11-19: Mac + iPhone clients, WPA2, browsing, island icon, Wi-Fi page "Not Connected", Mac
  IsExpensive, after reboots.
- Open: battery/signal in a Mac's menu (Instant Hotspot = Bluetooth Continuity); airplane-mode toggle did not restart
  the modem once after a SIM swap (it recovered by itself a minute later).

## 2026-11-19 (B) — Cellular survives a slow modem; airplane mode resets the modem

- What: redwood-cellulard retried only the SPI boot; a firmware-download timeout (stage 5, no reply in 5 s, seen
  right after a thermal shutdown) left "no modem" until a reboot. Now a failed bring-up is retried as a whole (3
  tries, 15 s apart; then left off, so a real fault still cannot loop). Airplane mode now acts on the modem: on parks
  it (command pipe "airplane"), off starts a fresh bring-up (pipe "restart", or launchd WatchPaths
  /var/tmp/redwood-cell-kick when the service is not running). SpringBoard watches AirplaneMode (cell_sb.h).
- Tested on the phone 2026-11-19: airplane on -> modem parked; airplane off -> service started, Jio 3 bars + data
  in ~33 s.
- Open: Personal Hotspot (needs CommCenter tethering answers + IPv6-only Jio: 464XLAT/DNS64 design).

## 2026-11-19 (B) — Native Touch ID on the under-display sensor (boot argument touchid=1)

- What: Apple's own Touch ID UI drives the S20's ultrasonic sensor (QBT2000 in TEEGRIS). Settings → Touch ID & Passcode
  enrols a finger with Apple's screens (two phases); the lock screen offers "Touch ID or Enter Passcode", shows a faint
  ring over the sensor, captures, and shakes on a failed or rejected try; iOS's rules (passcode after boot, 48 h, 5
  tries) apply. The enrolled finger survives reboots (catacomb: master + user components, loaded on the first unlock).
- Pieces: RedwoodMesa (redfin's SweetMesa port, MIT), redwood-fpd, dsc-exec + the iPhone 8 dyld cache for libBKDM1,
  /arm-io/mesa, kernel fp prepare/cbge/run, libredwood-touchid-ui (ring). Handoff: docs/devices/x1slte/touchid.md.
- Boot args: touchid=1 — now ENABLED BY DEFAULT in x1slte.cmdline (2026-11-19). Remove the arg to disable.
- Tested on the phone 2026-11-19: enrolment to completion; persistence across a clean reboot; ring on the lock screen;
  shake on failure.
- Open: unlocking with the finger (Samsung's matcher returns NO MATCH / 0x1d), haptic on failure, Settings switch for
  the ring, screen-off unlock. Overheating with many back-to-back boots (PMIC reset at ~109 C).

## 2026-11-18 — App Store / Media & Purchases login WORKS
- Root cause of the long-standing empty-200 Store sign-in: the three Mescal proof headers (X-Apple-AMD, X-Apple-AMD-M, X-Apple-ActionSignature) were stitched from two independent borrowed pieces (store-amd-bridge fetched AMD for a bodiless request; signing done separately), so they were not mutually consistent with the request body. Apple's pre-account gate silently dropped the request (HTTP 200, 0 bytes). Proven by fixture probe: a request fully prepared on the owner Mac, replayed from the phone, reaches the account check (-5000 "check the account information"); body-swap re-breaks it, confirming proofs bind to the body.
- Fix: store-request-build-bridge hooks -[AMSURLSession _prepareRequest:properties:error:] in accountsd/amsaccountsd/appstored/itunesstored/Preferences/AppStore; for auth.itunes.apple.com /auth/v1/native[/fast] it takes the native request's body as parameters and calls the broker op build-store-request, where the Mac's native AMSURLRequestEncoder builds and signs the whole request as one consistent unit; the phone sends those exact bytes. Replaces store-amd-bridge for this endpoint. No new credential exposure (the body already crossed to the Mac for signing).
- Broker op added to native-media-provider (v4) + native-media-transport.py validation. Verified end-to-end with a throwaway fixture account (never the owner ID) and then a real sign-in: /auth/v1/native returns a real token (2304-byte 200), account isAuthenticated=true/isActive=true, Media & Purchases logged in, App Store browses and lists apps.
- NOT yet working: installing apps (albert.apple.com device-activation 500, commerce license errorNumber 3027 / AMSErrorDomain 307) and the account/profile tab; these need device authorization/FairPlay provisioning this phone lacks. Device shows as the borrowed MacBook Pro. Runtime-only (exec_inject) — does not yet persist across reboot. Tunnel runs over the USB cable; Hackintosh over link-local IPv6.
- Source: tools/apple-account/store-request-build-bridge.m, store-build-from-params.m, store-gate-probe.m, native-media-provider.m, native-media-transport.py. See memory s20-store-auth-empty-200 and sessions/B.md.

## 2026-11-16 — Shared Maps graphics and Photos/UI compatibility deployed
- User confirmed Apple/Google Maps portrait/landscape and Photos previews/albums after the corrected cold boot. SpringBoard, backboardd and mediaserverd retained PIDs 9/21/57 throughout the postboot checks; no core fault seen in those captures.
- Shared driver fixes cover blend state, packed formats, sparse argument IDs, bounded resource-slot allocation, numeric vertex conversion and depth uploads. Common Metal-layer presentation helper requires no Maps app list.
- Kernel filters discovery for incompatible core renderers and Photos without runtime IOKit hooks. On-phone normal/legacy tests passed direct, search and dictionary registry APIs plus actual Metal device creation. Startup requires this kernel policy; older kernels stay off.
- Source and signed userland/kernel prebuilts included. Kernel uses standard strip -x (15,295,672 bytes); loader read window checked before flashing. Recovery head/tail and standalone boot read-back passed; base system and user data preserved. Passcode, AOD, camera-image and graphics assembly checks pass. Phone reports no protected passcode bag, as before this deployment; repeat protected-bag unlock was not retested.
- Open: Weather wallpaper hits CFRelease(NULL); Aegir wallpaper rejects an extended-range layer. Wallpaper prewarming can restart these extensions. This is not universal Metal support or complete wallpaper stability. Native standalone GPS remains separate unfinished work.
- See docs/devices/x1slte/maps-rendering.md for evidence, loader-window correction and rollback.

## 2026-11-16 — Third rear sensor and native single-icon cycle
- Camera's single native lens icon cycles0.5x →1x →3x and closes the scale normally; user confirmed. No overlay or forced three-button layout.
- GW2 native4864x3648 capture, focus and digital crop above3x are enabled;5x/10x trials retained GW2. This is not5x optical zoom. Primary autofocus and the original front worker are preserved.
- Source, signed media/lens/kernel prebuilts and clean-data enable seeding are included. Permanent recovery/root and standalone boot installed with read-back verification; system/data and loader configuration preserved.
- GW2 first frame improved2651.4→1671.1ms in a cold probe with safe slow-upload fallback. Primary cold startup remains roughly3s; switching is not seamless.
- Protected keybag restored locked. See docs/devices/x1slte/camera/gw2.md for validation, limits and rollback; clean seeding is fixture-tested, not a wiped-device installation.

## 2026-11-16 — Native rear lens control and primary autofocus shipped
- Native Camera zoom selects ultra-wide below1x and primary at1–5x; front keeps its original processing. GW2 is identified but not exposed.
- Correct primary rail voltages and full scaled autofocus travel; user confirms close focus works. Rear zoom no longer reconfigures the capture graph during video.
- Source and signed prebuilts included in S20 root assembly: media, native dial, brightness hook, tweak loader and clean-data enable seeding.
- Permanent recovery/boot update installed and read-back verified. Keybag locked after reboot;36frames each from ultra-wide, primary and front passed. Full wiped-data install is fixture-tested only. See docs/devices/x1slte/camera/wrapup.md.

## 2026-11-15 (17:30, A) — UEFI line "- JESVI -"
- The line under the UEFI's logo reads "- JESVI -" (was "[JESVI]"); the menu title "JesviOS" (was "JesViOS"). Dev-mode build flashed.

## 2026-11-15 (17:20, A) — boot screen texts: JesViOS, [JESVI]
- Loader menu title "JesViOS" (was HackiOS) on the menu and every status screen; subtitle back to "Galaxy S20  -  iOS 16.5".
  The UEFI's line under its logo reads "[JESVI]" (was "[Volume Up] Boot Manager"), in both modes. Dev-mode build flashed.

## 2026-11-15 (17:10, A) — boot screen: no UEFI hint for end users; menu subtitle "... - Jesvi"
- End-user builds blank Mu-Silicium's "[Volume Up] Boot Manager" line under the UEFI logo (dev mode keeps it); the
  UEFI volume is always remade from the untouched image. Menu subtitle "Galaxy S20  -  iOS 16.5  -  Jesvi" (was "S20+");
  Log access screen names ./x1slte.sh. Dev-mode build flashed 17:08.

## 2026-11-15 (16:55, A) — dev mode: the HackiOS menu back for development
- `DEVMODE=1` (common/devices/x1slte.local.conf or env) builds the loader with a `devmode` payload entry: text and the
  HackiOS menu with its countdown, as before. Default off (iOS's logo, no menu); release builds force it off. On for
  the developer's phone (flashed 16:54; takes effect at the next cold boot).

## 2026-11-15 (16:40, A) — boot screen: iOS's logo from power-on, no loader text or menu
- Samsung's splash (up_param logo.jpg), the UEFI's logo (was Project Silicium) and the loader show iOS's own boot logo,
  same size and place; the loader prints nothing and boots iOS at once (volume key during the logo: the menu;
  `bootui=0`: old behavior). The kernel scales the loader's picture at the 1080x2400 switch so the logo does not jump.
  User-verified on the phone (a split-second tear at the 120 Hz switch kept by choice). Made at build time from the iOS
  root (tools/bootlogo); no Apple artwork in git. Loader can now write up_param (tar only). docs/devices/x1slte/boot-screen.md.

## 2026-11-15 (11:50, A) — screen-off / AOD power: fast cores asleep, fewer wake-ups
- Screen off or AOD: the scheduler runs on the four A55s only (`scroff_little=1`, live `kern.redwood.scroff_little`,
  state `kern.redwood.scroff_little_now`); all 8 cores back the moment the display comes on (before the wake boost),
  and while sound plays (music, a call's voice) plus 10 s. Measured in AOD: cores 4, 5, 7 asleep the whole 10 s with
  zero wake-ups, core 6 once a second (was ~250 wake-ups a second on each fast core); SoC 60-61 C (was 68-70 C).
  Audio gate verified live (sound -> all 8 at once, small cores again 10 s after).
- Governor/thermal tick 500 ms with the screen off (`dvfs_scroff_tick=50` = as before); flashlight thread sleeps until
  asked (was a 30 ms poll); Wi-Fi poll 100 ms when quiet with the screen off (`net_scroff_poll=0` = as before); USB
  tick 250 ms with no host and the screen off (was 20 ms); Wi-Fi on the battery with the screen off: PM_MAX + every 3rd
  DTIM (`wifi_scroff=0` = as before; not yet observed on battery: the test phone was on the cable); wake_stats names
  each thread call. Details: proposals/idle-power/RESEARCH.md. Kernel flashed 11:35 (kernel slot only).

## 2026-11-15 (11:10, A) — AOD at 30 % of the slider (user-chosen)
- AOD level is now set in slider terms: 30 % of the slider position at AOD start (100 % -> 30 %, 50 % -> 15 %,
  25 % -> 8 %), about 7 % of the light: clearly dimmer than the awake screen by the same step at every level.
  (30 % of the nits looked ~2/3 as bright; 35 % of the slider still too close; user chose 3/10: "good like this".)
  Live tuning without a reboot: `rwsys kern.redwood.poke "fb.aod PCT"` (10..60). Hold + 2x re-take unchanged.
  Kernel flashed 11:06 (kernel slot only).
## 2026-11-14 — Camera correctness fixes staged on feat/s20-camera-quality (not deployed)
- Frame-owned black/band correction and preview-cache identity remove reader/render global-state sharing.
- Rear tap exposure uses the same correction as rendering; normal rear exposure rounds to even lines.
- Rear timing metadata follows Samsung's pixel clock; failed exposure writes stop through normal cleanup.
- Host RAW10/concurrency sanitizer tests, I2C fault injection, 66000 exposure-bound cases, iOS arm64 media/diagnostic
  cross-build and isolated GICv2 kernel build pass. No phone access/reboot; fingerprint phone ownership preserved.
- See docs/devices/x1slte/camera/implementation.md for artifacts, limits and integration/testing requirements.

## 2026-11-14 — TEEGRIS AUX/service handshake succeeds; IRQ/config probe staged
- Second approved test boot: all 8 AUX registrations and service channel succeeded (result 0, masks 1/1); cached
  inspect confirmed no retry. Keybag remained protected/locked before first unlock. No fingerprint match attempted.
- Actual DT confirms TEE IRQ INTIDs 254/255. Next staged kernel has a manually gated bounded SYSCONF exchange,
  first-event IRQ masking, response validation and panic/error rejection; host tests and kernel build pass.
- Located a public Exynos990 client-library reference; local-only ELF analysis identifies TEEC identification
  command IDs 5/6/7. These binaries are not executed/deployed/committed and exact firmware compatibility is unproven.
- See docs/devices/x1slte/fingerprint.md for test results, remaining matcher/provider work and next-probe boundaries.

## 2026-11-14 — Fingerprint compatibility SMC returns zero; channels staged
- User approved the probe reboot and phone takeover. Exact current recovery kernel slot backed up locally before
  flashing; loader verified the new write. Protected keybag restored locked before first unlock (AKS 0x1).
- Explicit CHECK_VERSION returned x0=0/x2=0, subsequent inspect was cached. User confirmed passcode works after
  reboot and on the next lock/unlock. This proves no fingerprint match; native biometrics remain unavailable.
- Added fixed-command probe client and a separately staged AUX/service handshake: dedicated zeroed wired backing,
  4-KiB wire PFNs, per-CPU affinity, bounded cache maintenance, stop-on-first-error and no automatic retries.
- Channel host tests with sanitizers and kernel cross-build pass. New channel kernel is NOT flashed; another
  user-approved reboot is required. No IRQ/SYSCONF/TA loading, enrollment, matching or biometric unlock yet.
- Detailed evidence, deployment boundaries and traps: docs/devices/x1slte/fingerprint.md. Main/master remain unchanged.

## 2026-11-14 — Fingerprint research branch (not enabled, not shipped)
- `feat/s20-fingerprint` starts from accepted main/master `8d584cc`, including newer thermal/logging changes.
- Actual saved DTBO confirms QBT2000, GPA1 IRQ pins 3/1, SPI4 and VDD_BTP_1P8. Live LocalAuthentication reports no
  biometric capability (-1000); Mesa/Pearl drivers are absent. User re-enabled passcode; protected/locked confirmed.
- Added a read-only capability probe, allowlisted DTB inventory, host-only fallback policy prototype, and a manually
  armed one-shot TEEGRIS version diagnostic in the kernel debug port. No biometric match/unlock operation is added.
- Host policy/guard tests under ASan/UBSan, synthetic inventory, existing passcode build-integration checks and the
  Exynos GICv2 kernel build pass. Experimental 16-MiB kernel slot is prepared in ignored out/fingerprint; not flashed.
- Implementation/research, precise missing transport/matcher/provider work and traps: docs/devices/x1slte/fingerprint.md.
  The first compatibility test requires the shared phone lock and explicit user reboot approval. Keep main/master
  on accepted code; do not merge this prototype as working fingerprint support.
## 2026-11-15 (11:10, A) — AOD at 30 % of the slider (user-chosen)
- AOD level is now set in slider terms: 30 % of the slider position at AOD start (100 % -> 30 %, 50 % -> 15 %,
  25 % -> 8 %), about 7 % of the light: clearly dimmer than the awake screen by the same step at every level.
  (30 % of the nits looked ~2/3 as bright; 35 % of the slider still too close; user chose 3/10: "good like this".)
  Live tuning without a reboot: `rwsys kern.redwood.poke "fb.aod PCT"` (10..60). Hold + 2x re-take unchanged.
  Kernel flashed 11:06 (kernel slot only).
## 2026-11-14 (21:30, A) — per-device targets and mods (ExtremeROM/UN1CA layout)
- `devices/exynos990/platform.conf` + `<target>/target.conf` (identity, display, camera, modem, chips, boot-args),
  `patches/` (all 990), `<target>/patches/` (one phone), `mods/` (optional: island, softpasscode); `./build.sh target`.
- Release image builds take the boot line from the target (S20 output = x1slte.cmdline word for word) and apply
  mods' root/system overlays + customize.sh when JESVI_TARGET is set; release.conf `MODS_DISABLE`.
- Targets x1slte (builds), x1s (inherits), y2s, z3s, c1s, c2s, r8s (public facts from ExtremeROM, unmaintained).
- 55 Python tests + Go suite pass. Day-to-day x1slte.sh unchanged. No phone involved.

## 2026-11-14 (20:50, A) — release.conf, Android-ROM installs, Jesvi-iOS-Restore (host-verified only)
- `release.conf` + `./build.sh release` (targets, outputs twrp/preflight/restore/tar, mode, writes) builds
  `Jesvi-iOS_<ver>_<date>_<target>.zip` and `Jesvi-iOS-Restore_...zip`; CI builds restore ZIPs on public runners.
- Installer sizes now come from the bound device profile (other Exynos 990 targets once measured).
- From an Android ROM's TWRP: the installer unmounts super/userdata mounts and removes only the dm devices stacked
  on them, after confirmation; it saves Android's 1 MiB partition map at super - 2 MiB before writing iOS.
- Jesvi-iOS-Restore writes an empty, valid Android map (saved copy or android.json stock layout) and clears HFSX
  headers; then Format Data + any ROM ZIP. LP encoder matches the S20's real pre-iOS super head byte-for-byte.
- Tests: 48 Python, Go recovery suite incl. ROM-hold planning and install→restore round trip; both ZIPs built and
  self-inspected from `dist/local-validation`. NOT run on the phone: hardware trial of both ZIPs still required.

## 2026-11-15 (11:05, A) — AOD well visible on a bright screen
- AOD cap raised 12 -> 60 nits: the same 30 % of the brightness at AOD start everywhere (10 nits -> 3, 40 -> 12,
  80 -> 24, 120 -> 36, 200+ -> 60). User-verified at 40-50 % brightness: "well visible". Dark-room hold unchanged.
  Kernel e8499142 flashed 11:00 (kernel slot only, phone's loader/root kept). Boot arg `fb_aod_max` still overrides.

## 2026-11-15 (10:58, A) — AOD brightness held (no dark-room fluctuation)
- Kernel: the AOD level is 30 % of the brightness at the moment AOD starts (2..12 nits), then held; auto-brightness
  changes during AOD are kept for the wake but do not move it; only a 2x or 1/2 change of light re-takes it.
  Verified on the phone in a dark room: auto-brightness went 5.44 -> 4.82 nits, AOD held at 2.000 for the whole
  sample. Host test of the hold logic passes. Flashed 10:55 together with B's avconferenced root change (one reboot).
  Below ~6 nits normal brightness AOD sits at the panel's lowest step (2 nits); lower needs Samsung's panel AOD mode.

## 2026-11-15 (10:10, A) — AOD on by default
- Fresh installs: Always-On Display on, wallpaper and notifications in AOD off (user's choice). A missing or unreadable
  preference file reads as these defaults (`tools/aod/config.h` `rw_aod_default`); a saved choice always wins.
  Config regression (`tools/aod/test_config.m`) and AOD build tests pass. This phone already had on/off saved: no change.

## 2026-11-15 (10:05, B) — avconferenced back on demand
- mkjobs.py: avconferenced out of DENY (it crash-looped when the port had no camera/audio; both work now). Without it
  callservicesd and InCallService asked for it every 2 s during calls ("Server died. Attempt reconnect"). Feature-flag
  Mach services ({"#IfFeatureFlagEnabled", "#Then"}) are now resolved at build time to "#Then" (iOS 16.5 ships them on).
- Verified live (job bootstrapped by hand, gone at the next reboot until the next image): started on demand during a
  call (10:00), no crash, 0 % CPU idle, no reconnect spam. In the image from the next boot-image build.
- Calls/texts: the 09:08 redwood-ims survived A's 09:24 reboot (same binary), registered, reg-event subscribed + "active".
- Note: prebuilt/userland/init does not have seed_phone yet (only a PREBUILT=1 build uses it; a normal build compiles
  src/init.c). Refresh it with tools/update-prebuilt.sh at the next full build.

## 2026-11-15 (09:45, A) — AOD: wallpaper fades to black on lock (AOD wallpaper off)
- Locking from the home screen or an app no longer shows the lock screen wallpaper before AOD: a black layer fades in
  over it (0.35 s, under the clock) and goes on wake. From the lock screen itself iOS's own fade stays (user's choice).
  Commit 1295706d (tools/aod/alwayson.m; QuartzCore added to the AOD dylib's build lines). User-verified ("perfecto").
- Researched, left as is (user: not worth battery/complexity): a short black moment when waking from AOD after ~2-6 s.
  iOS ends AOD live rendering ("live-rendering assertion released", backboardd "Set Blanked: YES / Display 1 power
  off"; the panel keeps showing the AOD picture from its own memory, so AOD stays visible at no cost), then wakes with
  its fade-in-from-black animation (performWake: fade-in hard-coded on). Same for button and tap; quick wakes before
  the blank are smooth. Only fix: keep iOS live-rendering through AOD (battery).

## 2026-11-15 (09:35, A) — Always-On Display: panel dimming, lift-to-wake from AOD, Low Power Mode, permanent in x1slte
- AOD (native iOS Always-On, default off, wallpaper off) merged from feat/s20-aod-preview (codex-aod's work, finished by A).
- Kernel `kern.redwood.display_aod`: while AOD shows the panel runs at 30 % of CoreBrightness's level (2..12 nits;
  boot args `fb_aod_pct`, `fb_aod_max`), 600 ms fade in, 200 ms out, exact level back on wake. Before: AOD stayed at
  full brightness (40-55 nits). SpringBoard's AOD service sets it on the InactiveOn transition, clears it on wake/start.
- Framebuffer: fade reads under the DSIM lock (a stale fade-out can no longer paint black after a wake); swap
  signal/cancel no longer fall through into panel power.
- redwood-motiond: AOD counts as asleep, so raise-to-wake works from AOD; Low Power Mode suspends AOD and raise-to-wake.
- Verified on the phone (user): dim fade into AOD, pickup wakes at normal brightness, passcode still asked, Low Power
  Mode: screen fully off and pickup ignored. Flashed: phone's loader/boot line + x1slte kernel 03c2c41a-equivalent +
  phone's root with the new motiond (docs/devices/x1slte/root-swap.md method). The phone's fingerprint-research kernel
  (feat/s20-fingerprint, not in x1slte) was replaced by the main line.
- Not done: Samsung ALPM/HLPM panel mode and a 120->60 Hz drop in AOD (mode switch is one-way per boot today).

## 2026-11-15 (09:30, B) — calls/VoLTE/texts wrap-up: registration never drops for a refresh; everything in the image
- Registration refreshed in place at half its lifetime (Jio: 3600 of 7200 s) on the same connection and SAs: the ~110
  min full restart (10-90 s without calls or texts, the main cause of late texts) is now only the fallback (401 or no
  answer, never during a call). Subscribed to the reg event: a network-side deregistration starts a fresh registration
  at once instead of at the next health check. Granted lifetime read from the 200.
- Sign-off fixed: it carried REGISTER 2's digest under nc=00000002 (wrong digest, refused, old registration lingered);
  now built when sent with its own nc.
- Permanent by default: a fresh or wiped data volume now gets every call/text piece from the images at boot
  (userland/src/init.c seed_phone, /etc/seed.list from build.sh): /var/root/redwood-ims, /var/jb/redwood/bin/
  redwood-telephonyd, /var/jb/redwood/callservicesd (system image: /System/Library/Redwood/Phone), libredwood-calls,
  RedwoodSMS + RedwoodCellular (dylib + plist). Copied only where missing; a newer one deployed by hand is kept.
  Before, the jobs were in the image but these programs were only on this phone's data volume.
- Prebuilts refreshed: redwood-ims, redwood-telephonyd, libredwood-calls.dylib.
- Verified: engine and init build clean. Not yet verified on the phone: the first in-place refresh (log line
  "registration refreshed in place"), seeding (needs a fresh image + wiped data volume to see "put in place").
- Open: missed-call banner/badge (callservicesd sees the missed call and sets the badge count; iOS's notification
  delivery for Phone shows nothing -- not the call stack); avconferenced is denied (mkjobs) and callservicesd /
  InCallService retry it every 2 s; RCS needs a client of our own.
## 2026-11-15 — AOD and AOD wallpaper default off

Show Wallpaper now defaults off as well as AOD. Updated source, tracked clean-build
prebuilts and helper; explicit user opt-in still works. Both saved settings and
settled native AOD policy verified off on the current phone without refresh/reboot.
Config and AOD/passcode assembly regressions pass.

## 2026-11-15 — AOD clean-image inclusion

S20 root assembly now includes native AOD and its helper from tracked prebuilts,
independent of data-volume tweaks. Source and macOS GPU build paths refresh them;
missing inputs fail assembly. Injection is targeted, repeatable and preserves
other rules. Default off; existing user setting retained. Root/standalone modules
use distinct Settings classes and shared duplicate hook guards. iOS cross-build,
AOD assembly/config/token checks and required passcode assembly regression pass.
Full system/root images not rebuilt or flashed here. Physical panel ALPM remains
separate experimental work. Local only at user's request after upstream403.

## 2026-11-15 — native AOD service (feature branch)

Settings-controlled native Always-On Display, default off, pinned to this S20 /
iOS16.5 cache. Adapt native capability providers and use Apple's pseudo-flipbook
live-composition backend: CAFlipBook returned no frame and trapped SpringBoard at
BacklightServicesHost0x1eebbd068 (`renderedFrame != nil`). The fallback avoids that
unsupported capture path without bypassing assertions. User confirmed persistent
AOD with no restart; native state1 and unchanged SpringBoard PID independently
observed. Repeat passcode/AOD-cycle confirmation remains pending. No kernel reboot
or panel ALPM activation. Build, config/token regressions, package allowlist and
hash checks pass. Standalone installer/package prepared; clean image integration
still pending. See docs/devices/x1slte/aod-preview.md for limitations and rollback.

## 2026-11-15 (08:10, A) — texts react at once; outgoing-call delay measured; cellulard timing
- Texts: redwood-ims sends a text the moment Messages drops it in sms-out (kqueue), RedwoodSMS picks up received
  texts and send results the moment they land (vnode watch); the 1 s / 2 s polls are now only a fallback. RP-ACK goes
  to the SMSC (P-Asserted-Identity), and an SMSC retry of the same text is acknowledged but not shown twice.
  On the phone: /var/root/redwood-ims (active from its next restart, at the latest the ~110 min re-registration),
  /var/jb/usr/lib/TweakInject/RedwoodSMS.dylib (from the next imagent start). Backups: *.pre-sms, *.pre-watch.
  Copies: prebuilt/userland/redwood-ims, prebuilt/userland/RedwoodSMS.dylib. Not yet timed on the phone.
- cellulard: ms wall-time stamps on IPC lines and a decoded line for the IMS voice bearer (16/0b). It runs as
  /var/root/redwood-cellulard (the /redwood/bin copy re-execs it). A timed call showed no delay on our side: the
  bearer comes up 33 ms after the 183. Details: docs/devices/x1slte/native-calls.md "Open problems".
- RCS: not in iOS 16 Messages; Jio has its own RCS config server (config.rcs.mnc869.mcc405...). Next step if wanted:
  check whether it provisions a non-Google client before building anything.

## 2026-11-15 (02:16, A) — native calls permanent; status bar on island phones; Cellular settings live
- Calls: native only, on at boot. `feat/native-calls` merged into x1slte (ad93a151); redwood-telephonyd starts from the
  boot image and registers as `com.apple.coretelephony telephony=1`; RedwoodPhone no longer shipped. Engine review
  fixes (bounded SIP message building, 100rel, no 32 s give-up after 100 Trying, one INVITE dump), telephonyd bounded
  sets + cheap unchanged-file path. Docs: `docs/devices/x1slte/native-calls.md`.
- Status bar (island phones, device-tree detection): plain time (activity only in the island), no battery bolt
  (`bolt=1` keeps it), left padding removed (`lead=12`, `lead=0` Apple's). Docs: `docs/devices/x1slte/dynamic-island.md`.
- Settings > Cellular: VoLTE / Calls / Text Messages from the live engine state.
- Flashed three times by swapping only root.apfs inside the phone's own recovery image (loader, kernel and signature
  byte-identical): `docs/devices/x1slte/root-swap.md`. Verified after each boot: telephonyd at boot, VoLTE registered,
  island hook armed. Not yet seen on screen by the user: plain time while recording, no bolt while charging.
- Open: outgoing calls' late ringing (precondition / modem IPC 0x16), the privacy dot position, Personal Hotspot.

## 2026-11-14 — Default wallpaper: the user's photo poster ships with the image
- `92857ab9`: the user's Photos poster (their picture, lock-screen clock style, layout, paired home screen) is the
  wallpaper of a phone whose data volume has no poster store yet: a fresh install or a factory reset.
- `userland/defaults/wallpaper/PRBPosterExtensionDataStore/59` (3.5 MB: PosterBoard's store reduced to that one poster,
  snapshots left out, editing source as HEIC) -> system image `/System/Library/Redwood/DefaultWallpaper` (build.sh)
  -> `init` `default_wallpaper()` copies it (part name, renamed whole) to `/var/mobile/Library/Application Support`,
  owned by mobile. A phone with wallpapers of its own is never touched.
- Verified on the phone (user, 2026-11-14 22:40): the shipped store alone, swapped in over an Apple wallpaper and
  resprung, shows the photo with the same clock style on lock and home screen ("perfecto").
- Takes effect with the next system image (`userland/build-gpu-y2s.sh` rebuilds the prebuilt init too).

## 2026-11-14 — Persistent software passcode and clean-build defaults
- Source history: `368e80c` verifier/ABI foundation, `110f0f8` native persistence and keybag events,
  `50ce5b2` repeat-lock enforcement, empty-probe optimization and file-security refinements.
- User confirmed Settings stays enabled, initial boot asks for the code, and second unlock requires it after the
  display-off bridge was installed. The standalone bridge lives on persistent `/var/jb`; the same code is built
  into `libredwood-sb.dylib`, including the tracked prebuilt, for clean images without that tweak installation.
- Clean-build trap fixed: tracked `x1slte.cmdline` now includes `softpasscode=1` and `autounlock=0`; default payload
  uses that profile. `userland/build.sh` follows it for unprofiled/S20 builds, and x1slte's common profile sets
  `SOFTWARE_PASSCODE=1`. Keep native keybagd, the PID 1 restore handoff and immediate-lock policy together.
- Full implementation, ABI details, tests, staging/deployment boundaries, security limits and **Traps and recovery**:
  `docs/devices/x1slte/passcode.md`. Read before modifying authentication, boot args, root assembly or native keybagd.
- Host verifier/ABI/file-guard tests, clean root-assembly integration test, iOS helper/SpringBoard cross-build and
  Exynos GICv2 kernel build passed. A complete fresh IPSW/firmware build was not run; no new reboot in this step.
- Open: software-only UI authentication, reversible legacy key wrapping, volatile retry counters, no SEP security.
  Optimized kernel/hardened startup helper are staged, not flashed. UI negative-code/reboot tests of the standalone
  bridge remain separate validation tasks. The user accepted the swipe threshold; no gesture change is claimed.
- Local `main` and `master` contain this work. Remote publishing remains blocked by unavailable GitHub credentials;
  do not overwrite divergent remote main to bypass that problem.

## 2026-11-14 (13:25, B) — idle power, softer thermal steps, quieter log
- Wi-Fi broad scans coalesced (`kern.redwood.wifi_scan_gap`, 30 s): idle big cores 85-90 % asleep (was 19-32 %), 0.76 A from USB, 62-63 C.
- Thermal second step: GPU 455 MHz (was 377), A76 1482 MHz (was 1066) above 90 C.
- Log: DVFS changes only on failure (`kern.redwood.dvfs_log=1`), BT vendor lines capped, launchd quiet by default (`QUIET=0`).
- metaloff out of the boot list (mediaserverd stack overflow). Build: callservicesd job fixed with python (macOS build's plutil is a no-op).
- One clean build of everyone's work flashed (main = master = x1slte, all active branches aligned; devops pending). Details: docs/devices/x1slte/debug.md.

## 2026-11-14 (B) — Metal driver, wallpapers, call screen fixes
- Metal driver (CoreImage correct on the Mali, matches macOS on 7 filters): unbound texture/sampler placeholders,
  simdgroup barriers, dispatch-sized threadgroup memory, sampler 0 for read(), Apple's 32-wide SIMD width, GPU->IOSurface
  write-back, YUV 420f planes, float CoreVideo formats, ASTC, no-copy buffers/textures, app-memory IOSurfaces as copies.
- Per-process Metal: libredwood-metalon (Photos/Gradient/Emoji poster helpers) and libredwood-metaloff (PosterBoard,
  AegirPoster, WeatherPoster, mediaserverd) in the boot inject list; system-wide is `metal_props on` (live, not at boot).
- Wallpaper: Photos "Use as Wallpaper" works; gallery via duetexpertd (job allowed + libredwood-duet data-vault skip);
  parallax only for Apple posters, rests unless tilted 2.5 pt (GPU ~97% idle at rest); custom pictures stay still.
- Calls: callservicesd job forced to /var/jb/redwood (macOS builds packed the stock /System path -> no call screen);
  RedwoodPhone ignores the duplicate cold-launch dial (outgoing calls were cancelled at once).
- Details, switches and traps: docs/devices/x1slte/wallpaper-metal.md. Native-calls design (CommCenter identity): sessions/B.md.

## 2026-11-14 (10:40) — Real 120 Hz: scrolling "a lot better" (user-verified)
- iOS ran its frame clock at 60 while the panel scanned 120 (CADisplay.refreshRate 16.7 ms): backboardd now gets
  CA_HEARTBEAT_RATE=120 in fb_mode 1/3 (8.3 ms). Apps get the ProMotion opt-in (QuartzCore capped apps at 60): `app_fps=0` off.
- Touch scans at 120 Hz (sec_ts 0x94; was ~60 reports a second): `touch_scan=`. SpringBoard's own 20-30 fps animations
  raised to 60 (`sb_fps=0` off).
- Display preset FHD+ 1080x2400 @ 120 (`fb_mode=1`, 3x). At 720x1600 iOS draws at 2x (750x1624) with MobileGestalt's scale
  and the status bar fixed to match (`fbscale=` overrides). All presets: docs/devices/x1slte/display.md.
- Idle heat: USB mux and haptics threads no longer poll every 1/4 ms (big-core wake-ups ~8x fewer).
- Tools: `kern.redwood.ksample` thread sampler, tools/perf/objcmap (method names). Open: per-page icon building
  hitches (`sb_keepicons=1` experimental, broke the home screen), touch-to-screen latency trace switch.

## 2026-11-14 (05:10) — Native calls: iOS call screen, Phone app dialling, earpiece (user-verified)
- Incoming VoLTE calls ring on iOS's own call screen (lock screen included): answer / decline / end / mute; the caller's
  hang-up ends it here. Outgoing calls from the Phone app (keypad, Recents, Contacts) go out over VoLTE. Voice on the earpiece.
- RedwoodPhone.app (`tools/cellular/RedwoodPhone`, /var/jb/Applications, run by launchd as mobile): the CallKit provider;
  talks to redwood-ims through /var/tmp/redwood-cell/call-ui (state / cmd.* / alive). callservicesd (Apple's, left out of the
  system image) runs from /var/jb/redwood (install.sh puts it there); its job and RedwoodPhone's are in root.apfs.
- libredwood: MobilePhone's dials go to RedwoodPhone (TUDialRequest provider); InCallService/SpringBoard treat it as the
  telephony provider (full call screen, not the Dynamic Island).
- redwood-ims: outgoing INVITE (no 100rel: Jio 481'd our PRACK and the answer never came), ACK/CANCEL/BYE, early media;
  fixed IMS ports 50060/50062 (a restart replaces the registration: before, dead contacts made calls late or "busy");
  responses fall back to the registration connection; keep-alive on the P-CSCF's connections; call numbers unique across
  restarts; country code from the SIM's MCC; timestamps on call logs; hangs up cleanly on SIGTERM.
- Kernel: kern.redwood.snd_route (1 = earpiece: top amplifier only, snd_ear_below 18 dB down), snd_call; audio plug-in
  hands the mic to the call. cellulard registers RedwoodPhone with LaunchServices at boot (it forgets /var/jb apps).
- Open: speaker toggle on the call screen (iOS needs a receiver port from the audio plug-in), purple call-screen
  background (GPU), Dynamic Island mic waveform, Recents cleared, Messages button greyed, de-register on stop.

## 2026-11-14 — VoLTE calls with voice, both ways (user-verified on the phone, ~03:45)
- **Incoming VoLTE calls carry voice**: the S20 answers (opt-in auto-answer: `touch /var/root/cell-answer`), the caller is
  heard on the speaker, the S20's mic is heard by the caller; hang-up from either side; up to 1 h.
- How: the S5123's own voice engine does not take the RTP stream, so redwood-ims is the voice engine (`tools/cellular/ims-media.h`):
  RTP AMR-NB 12.2 (RFC 4867 bandwidth-efficient) <-> opencore-amr 0.1.6 (Apache-2.0, static; `tools/cellular/build-ims.sh`) <->
  /dev/redwood-audio (speaker, 48 kHz) and /dev/redwood-mic. Jitter buffer (60 ms), FIR 8<->48 kHz resampling, loss
  concealment on sequence gaps, mic AGC, real-time threads, host route to the media peer over the IMS interface.
- redwood-cellulard: the dedicated bearer's RTP arrives on SBD channels 10/11 (and possibly the data PDN) as bare IPv6:
  now handed to the IMS interface (it was only logged). Health line: `ims up (in N, sbd M)`.
- Fixes on the way: the SDP's c= address lost its first digit; a call answered but never ACKed stayed "answered" and every
  later call got 486 Busy (now: media from our 200 OK, give-up after 30 s with no ACK and no voice).
- Tested: several calls from another phone on Jio, 30 s to 2.5 min; counters in /var/tmp/redwood-ims.log every 5 s.
- Commits: x1slte 9de2da3 ce68be3 (prebuilt redwood-ims / redwood-cellulard updated), cellular branch bdee2b7 8ac445e.
- Kernel side (tried first, kept off): `snd=3`/`cpcall=1` maps ABOX's PCI voice region + modem doorbell for Calliope
  (fe4a46b 39d2768 e3589a1); not needed for calls. e3589a1 also fixes a panic in the Calliope fault report.
- Open: iOS call UI (ring / answer / decline instead of auto-answer), outgoing calls, earpiece vs speaker, echo cancellation.

## 2026-11-13 — cellular: data, VoLTE, texts in Messages (merge dfef1d3; verified on the phone)
- **LTE data** for all apps: redwood-cellulard (utun + configd, Jio DNS/DNS64), kernel packet window (CP_IPC_PP_FLAG);
  ~3 MB/s over LTE. Starts by itself at boot (~65 s to data). Watchdogs: registration (75 s), downlink (max 1 restart/15 min).
- **Status bar**: signal bars, Jio, LTE glyph (when Wi-Fi is off), satellite glyph hidden (boot-arg cellsat=0 keeps it).
- **Settings > Cellular**: live network/signal/SIM, working Cellular Data switch, VoLTE status (RedwoodCellular tweak).
- **VoLTE (IMS) on Jio**: redwood-ims registers (ISIM AKA, IPsec ESP), re-registers every ~110 min, one instance.
- **Texts in the Messages app**: send (Delivered on SMSC confirmation), receive with notification, long texts (parts),
  reactions as their SMS text (RedwoodSMS tweak in imagent; image inject.conf has imagent).
- Tested: end to end S20 -> own number -> SMSC -> back -> Messages; texts from another phone; sends from Messages;
  cold boot. Kill switches: /var/root/cell-nodata, cell-noims, cell-noimsclient, cell-nocommcenter;
  /var/mobile/Library/Preferences/com.redwood.sms-bridge-off.
- Open: Control Center data button (an attempt was removed; notes in the cellular docs), hotspot (IPv6-only Jio needs 464XLAT),
  MMS (pictures/groups), calls. A stalled first modem start after boot is restarted by the registration watchdog.
- Details: work/hackios-cellular/docs/devices/x1slte/cellular/download-plan.md, sessions/A.md.

### Cellular stability (2026-11-13 18:30, B) — `9f4c792`, `6a8ea5c`, `e005b29`, `d08bc68`
- VoLTE now starts after every reboot: a stale `/var/tmp/redwood-cell/ims.pid` (reused pid) no longer looks like a
  running client (it left the phone 34 min without VoLTE after the 18:00 boot).
- Downlink ring: every used buffer goes back to the modem (fore follows done) -- suspected cause of the "downlink
  silent" automatic modem restarts. A health line per minute in /var/tmp/redwood-cellular.log tracks it.
- Incoming calls get "486 Busy Here" instead of a bare 200 OK (which would have connected callers to silence).
- `echo restart > /var/tmp/redwood-cell/cmd` restarts the modem cleanly.
- Live on the phone (/var/root binaries) and in prebuilt/userland for the next boot image.

### Smoother phone, texts, sensors, status bar (2026-11-13 19:00, B) — `8d684ae`, `effab2b`, `9c742e6`, `573cce0`/`d1e21e4`, `99f1ff5`
- Lag: aggregated (powerlog's daily archiver) was making one archive per day since 1970 (12,691 files, a core at 92 %).
  Fixed on the phone (archive moved to /var/root/powerlog-backup-*); init now restores the saved time at boot and
  rwlogd saves it every minute, so the clock never starts at 1970 again.
- Texts over VoLTE: SIP + TCP keep-alive; the idle connection was dropped and texts got no answer. Sent and received
  texts verified 18:51.
- Sensor hub: light every 1 s while the display is on (auto-brightness needs steady readings); proximity in bursts,
  2 s every 30 s (its IR LED flashed 5 times a second); both fully off with the display off; pressure 1 s, motion 60 ms; hub debug text only with shub_text=1 (was 482
  log lines a minute). Kernel built in hackios-cellular (19:12).
- Cellular service log quiet by default (was 38 KB/min); tools/bg-status.sh and docs/devices/x1slte/background.md list every
  background service and debug switch.
- Status bar: the left side 3 pt nearer the edge (statusbar_shift.h, `lead=` in /var/jb/etc/redwood-statusbar.conf).
- Island: built-in pill 18pro-small (75 x 28), presets in tools/island-presets.
- All flashed and running on the phone (kernel 18:46, boot volume 18:57).

## 2026-11-12 — cellular IPC memory audit (built, not deployed)
- Loader emits an exact IPC reservation handoff only after ownership/cache recheck.
- Root `cp.memory` audits existing XNU translations and cache attributes without
  reading IPC contents or enabling DMA. Refuses absent certificate/outside aperture.
- Kernel and loader builds pass;42 Python tests and all C ASan/UBSan checks pass.
- Hardware validation pending a batched restart. No new phone restart or service claim.

# x1slte (iOS 16.5 on the Galaxy S20, SM-G980F) — shipping log

Local branch `x1slte` only (not pushed). One entry per **verified** change: what it does, the commit(s), the boot
args that control it (default in **bold**), and how it was tested on the phone. Newest first. When cutting a release,
this file is the changelog; "Open" lists what a release must mention as known issues.

### Status bar padding for real, island presets 1/2, pickup wake kept (2026-11-13 22:05, A) — `4eac8b5`, `88cadbd`, `06105a2`
- Status bar: the phone uses UIKit's notch layout (_UIStatusBarVisualProvider_Split1125: the screen is the iPhone X's),
  not the island one the earlier hooks targeted (they never ran). The time moves left by lead/2 through
  +leadingCenteringOffset (-0.67 -> -10.67), and the right side, which UIKit derives from the left, is given that back
  (+trailingCenteringOffset 1 -> 11); `trail=` adds right padding. User-verified ("padding is working").
- Island: preset 1 `punch-21` (built-in default), preset 2 `punch-live18` (81 x 36.67 while a Live Activity runs; on
  the phone now). Entry into the live size: the first animation (0.4 s UIKit, 04f9b79), the user's choice.
- codex-motion's table-pickup wake (2a082cf, only on x1slte-cellular) cherry-picked into x1slte (06105a2): running.
- Flashed 22:04; cellular, VoLTE and motion wake checked after the boot.

### Punch-hole island, taller for Live Activities; status bar time at the left (2026-11-13 21:05, A) — `b383af6`
- Island default is now the 21 x 21 pt punch hole, 6 pt from the top (preset `punch-21`). While any Live Activity /
  system element is minimal, compact or expanded (SAUI layout modes 1-3), the island is `lh` tall (32 pt) and at
  least that wide; back to the circle when they end. User-verified.
- Status bar: laid out around a cutout of at least 125 pt (`cut=`), so the time sits where an iPhone has it instead
  of centred in the wide space left of a small hole; `lead=` 20 by default. User-verified ("all worked").
- Sleep (`idle_lock=1`) confirmed on the phone: the display turns off 10 s after an untouched wake on the lock screen.
- Tunables, all in /var/jb/etc, applied by a respring: redwood-island.conf (`y= w= h= lh=`, `off`),
  redwood-statusbar.conf (`lead= cut=`), redwood-idle.conf (`mode=0|1|2`).

### Cellular restored, sleep = lock screen only, punch-hole island grows for Live Activities (2026-11-13 20:56, A)
- **Cellular/VoLTE back:** the two flashes from this tree (20:30, 20:40) built the loader without
  `CELLULAR_RESERVE_CP`, so the modem got "IPC map refused" (no LTE, no VoLTE, no texts). `x1slte.sh payload` now
  builds it with the reservation by default. Verified after the 20:56 boot: LTE data up (Jio, 3 bars), IMS registered.
- **Sleep:** `idle_lock` default 1 (was 2): the screen times out on the lock screen only (10 s after an untouched
  wake); unlocked it stays on. `/var/jb/etc/redwood-idle.conf` `mode=0|1|2` switches it, read at every wake.
- **Island:** B's geometry hooks (proposals/notch) in, so resizes reach every island getter (minimise animation);
  while a Live Activity shows, the compact/minimal island is `lh` tall (default 32 pt; conf `lh=`), widths Apple's.
  Conf keys are case-insensitive now. Presets `punch-21` (y=6 w=21 h=21, on the phone) and `favorite-pill`
  (w=75 h=28 at Apple's top gap: the pill the user liked). Live Activity height: **not yet seen by the user**.
- **Status bar:** `lead=` may now take the left area past the edge (the time is centred in it, so it moves by
  about half of `lead`); phone at lead=24.
- codex-motion's motion routing (CoreMotion fixes in backboardd and SpringBoard) kept in this root image.

### Auto-brightness back in Settings, faster; perceptual slider (2026-11-13 20:45, A + codex-brightness)
- **Auto-Brightness switch is back** (Settings > Accessibility > Display & Text Size): MobileGestalt's
  `ambient-light-sensor` asks IOKit for a {PrimaryUsagePage 0xff00, PrimaryUsage 4} entry, which the virtual HID light
  never had; the kernel now publishes `RedwoodAmbientLightSensor` once the hub finds the light sensor, and init rebuilds
  the stale MobileGestalt cache once. User-verified: the switch shows and auto-brightness follows the room.
- **Slider is perceptual** (`fb_bl_curve=1`, **on**; 0 = old linear): the slider's legacy value went to the panel as
  value x 420 nits (50 % = 210, 100 % = 420: "50-100 % feels the same"). Now 2 + 418 s^2.2: 10 % 4.7 nits, 25 % 22,
  50 % 93, 75 % 224, 100 % 420 (measured on the panel target). Auto-brightness's manual anchor uses the same curve.
- **Faster auto-brightness:** light sensor every 250 ms (`shub_light_ms=`, was 1000); brighten after 1.5 s, dim after 3 s
  (were 3 / 5), big changes (4x) after 0.5 / 1.5 s; wake hold 1 s (was 5); ramps 0.5 / 1.2 s (were 0.9 / 2). The manual
  anchor holds only within 1.5x lux (was 2x or 20 lux: a level set at 30 lux kept a dark room at 167 nits).
  Tests: tools/tests/brightness_policy.c. User: "feels much much better".
- **Proximity on demand only:** no more 2 s bursts every 30 s; the IR emitter turns on only for backboardd's native
  active/monitoring modes (calls), never for lock-screen passive/pocket checks, and both sensors are off with the
  display off (`shub.prox` poke shows the state). Idle: light on, proximity off -- verified. Call-time proximity: **not
  yet verified** (no calls yet).
- Kernel 20:40 + boot volume flashed 20:42.

### Files, Finder and fresh installs (2026-11-13, B) — `8cd6c3b`, `dd577f2`, `dc1cea2`, `c7e2856`
- **Why it broke:** after a restart LaunchServices no longer knew `org.hakios.LocalFilesHost`, the hidden app that carries
  Save to Files, the document picker and On My iPhone ("the requested app extension could not be found", no Finder sharing).
- **Boot job** `/redwood/bin/register_finder_host` (`com.redwood.finder-host`): registers the host natively
  (`registerApplicationDictionary:`, app + data container + 3 extensions, hidden), no uicache. On the phone's image; ran by
  itself at 06:35 and 06:45 ("Jesvi Files user registration: complete").
- **Finder shows the phone's storage:** the host's Documents is a link to On My iPhone (File Provider Storage); the Mac lists
  Downloads etc. through House Arrest. Checked from the Mac (S20 only: the user's real iPhone is on the same Mac).
- **Fresh install:** `userland/system.apfs` rebuilt (2630995968 bytes, `userland/system-only.sh`) with
  `/Applications/HakiosLocalFiles.app`; written to `super` and booted. Old image: `system.apfs.bak-nov7`.
- **Kernel `c7e2856` (on the phone since 06:50, kernel 06:45:20; booted, 90 s stable, hand-back to Log access worked at 06:49 on the old kernel by luck, camera flips still to be tried by the user):** camera ring freed under a reader (panic in camdev_read); printf with
  interrupts off in `redwood_usbmux_link` (panic on every hand-back to the loader: `/menu?to=1` bounced into iOS); link
  restart once when lockdownd first answers (Finder lost the phone after every restart).
- **Camera off at boot (`b494677`):** `device_request_streams` started frames at every reservation; mediaserverd reserves
  both streams at start just to list the cameras, so every boot powered the rear sensor and streamed the front one.
  Now only a stream with a PixelBufferHandler begins. On the phone's image since 07:00: "only listed ... its sensor
  stays off" at boot; Camera.app 365 pictures of 366 frames on the dev copy.
- Phone at 07:03: kernel 06:45:20, root with the helper + this media library, system image with the host; Mac: 0 refused
  connects after the restart, 1 file-sharing app. Rollback images: `proposals/finder-host/deploy/`.
- Not done: grey backup/media tabs in Finder; `tools/run-visible.sh` did not start its command in this session (check before
  trusting build/NAME.log).

## 2026-11-13 — Cellular: the S20's modem runs under iOS; signal and operator in the status bar (user-verified)
- The S5123 modem (external, PCIe) is brought up by our own service at boot (`com.redwood.cellular`,
  `/redwood/bin/redwood-cellulard`, built in branch `x1slte-cellular`): SPI BOOT, PCIe link, hypervisor
  S2MPU grant, firmware download (TOC/MAIN/APM/VSS/NV/FIN) through the reserved shared memory, Samsung's
  INIT_START/PHONE_START handshake, packet-processor info and SBD rings, then Samsung IPC.
- The modem reads the SIM and camps on the network; the service writes its signal, operator and
  registration to `/var/tmp/redwood-cell/state`, and SpringBoard's telephony provider answers from it
  (`userland/src/cell_sb.h`): bars + "Jio" in the status bar and on the lock screen (user-verified).
- Waits for the system to settle after boot (BOOT's byte transfer misses the modem ROM's window under load),
  retries BOOT; a newer `/var/root/redwood-cellulard` runs instead of the image copy.
- Off switches: `cellular=0` (modem stays off), `cellbar=0` (status bar hook off).
- Not yet: mobile data, SMS, calls (calls on Jio need VoLTE/IMS). Commits 98e8abe, 75bbaf7, 6f00bc0, 72bda73
  (x1slte) and 2b0f509..fdd886d (x1slte-cellular).

## 2026-11-12 — OpenGL ES games run (Angry Birds Space, Leo's Fortune); screen sleeps on its own (user-verified)
- **OpenGL ES games draw.** iOS's OpenGL ES runs on Metal and gives Metal its shaders in Apple's private GLIR form,
  which only Apple's GPU compiler reads: games showed black or died at once. `libredwood-eagl`
  (`userland/src/eagl_app.m`) replaces EAGLContext in the games named in build.sh `EAGL_APPS` (default
  `AngryBirdsSpace Leos_fortune`): a Mesa OpenGL ES 3 context on the Mali (panfrost), iOS's gl* functions reach
  Mesa through the table in TSD slot 30, and each presented frame is copied on the GPU (turned upright) into
  IOSurfaces the layer shows. Angry Birds Space: 97% -> 14% CPU with the GPU copy. Commit 09b9956.
  Doc: docs/devices/x1slte/games-gl.md.
- **Screen sleep by default**: our idle timer (`inject.c`) is now on unless `idle_lock=0`: lock screen dark after
  10 s untouched (`idle_lock_s=`), unlocked after Settings' Auto-Lock time ("Never" = stays on), not while sound plays.
- How tested: both games launched from the home screen, played (user: "works flawlessly"); frames grabbed from the
  game (`eagl-shot`), CPU from `ps`. Leo's Fortune looks stretched: the game itself draws at 1280x960 (4:3) and
  scales up (an old IPA); not ours.

## 2026-11-12 — rear camera: video recording saves, frame rate (session A, verified on the phone)
- **Rear recordings in H.264.** Camera.app asks the back camera for HEVC; the only HEVC encoder here is Apple's
  software one (registered for photographs), too slow for video: rear movies were never finished or ~8 fps.
  `inject_media.c` answers `-[FigCaptureMovieFileRecordingSettings videoSettings]` hvc1 as avc1 (OpenH264, as the
  front). Verified: complete files, video and audio in sync, 20.7 fps (IMG_0169, IMG_0170). Commit 7c3b2ee.
- **Rear stats every other frame**, rear render below the encoder's priority (`camera_device.c`): the phone
  throttles at 91-96 C; rear preview ~20 fps hot, ~28 fps cool.
- Also in 7c3b2ee: front/rear switch waits for the asked sensor; sensor asked for before the first frame; lens
  shading per sensor; rear 12 MP black measured per frame. 12 MP rear (cam_s20_rear_full=1) stays off: row-1912 band.
- How tested: Camera.app Video on the rear; files checked for moov, codec, durations, frame count.

## 2026-11-12 — Dynamic Island pill moved up over the camera hole (user-verified on the phone)
- Pill width 81 pt ("iPhone 18 Pro" look, user-chosen). Known issue: charging banner cuts the "100" of "100%".
- In libredwood-sb (`userland/src/island_sb.h`, with `island=1`): pill top gap 5 pt instead of Apple's 11.33,
  so it covers the top of the hole. Width/height tunable (`/var/jb/etc/redwood-island.conf`, respring only).
  Cold boot verified: no SpringBoard aborts, hook in. (As a /var/jb tweak it crash-looped SpringBoard: doc.)
  Live Activities: Clock timer and stopwatch show in the island (user-verified; the first one after boot can be slow).
  Same boot image: `init` clears MobileGestalt's cache when `island=` changes (verified: marker written). Commit e9a23d6. Doc: docs/devices/x1slte/dynamic-island.md.

## 2026-11-12 — Dynamic Island over the camera hole (user-verified on the phone)
- iOS's own Dynamic Island (SpringBoard SystemAperture, Pill status bar), no overlay: the loader reports the
  iPhone 14 Pro subtype (`artwork-device-subtype` 2556) and, as a 14 Pro's tree does, `/buttons home-button-type` 2.
  Without the second, BaseBoard counts only subtype 2436 as buttonless and iOS acts like a home-button iPhone.
  `island=` **1** (in x1slte.cmdline). Commits 1b25aa4, 7f188ae.
- Tested: MobileGestalt ArtworkDeviceSubType 2556, HomeButtonType 2; pill shown, home-bar swipe and touch normal.
- Switching it: just the flag (`island=1` on, leave it out for the iPhone X notch). `init` removes MobileGestalt's
  cache when the flag changed since the last boot, so iOS rebuilds its answers (needs the next root image build).
  Doc: docs/devices/x1slte/dynamic-island.md.

## 2026-11-12 — camera: videos save with sound; front camera tuned (user-verified on the phone)
- Videos finish and save with sound: the audio source node saw a 430-million-frame "gap" in the HAL plug-in's
  times on every buffer and zeroed 1.7 GB per buffer, so no sound reached the movie writer and every recording hung
  (mediaserverd SIGTRAP "Timed out waiting for the capture graph to stop"). Its gap limit is set out of reach
  (`inject_media.c`). Commits 552fd8a.
- Orientation: preview a mirror, photos and videos as others see you (`cam_turn=` **5**, kernel default; user-checked).
- Speed: drawing on its own thread, 48 ISP pieces, no sensor restart on start/flip (preview 22 -> ~28 fps, sensor-bound).
- Camera.app opens front + Photo after its reset time (libredwood in Camera). Rear camera black for now.
- Image: generic phone colour matrix (tunable on the phone), clipped highlights stay white, white balance starts where
  the last stream left it, smoother AE steps (user: "auto exposure is perfect").
- Tap-to-expose (meters the tapped spot), the exposure slider (ExposureBias, a multiplier with 256 = 1) on the picture
  at once, AE lock. Front screen flash (SpringBoard white window + full backlight, photo 10 frames into the light; user: "perfect").
- Encoder: 1.6x iOS's bitrate, OpenH264 low complexity, 6 threads.
- Kernel: REDWOOD_CAMERA_SENSOR's mode chooses full/binned for the S20 front sensor (stream stays full: binned looked worse).
- Dev loop: `tools/camera/media-dev.sh` loads libredwood-media from /var/redwood without a reboot.
- Commits 552fd8a..41db5a2. Tested by the user in Camera.app throughout.
- Open: Live Photo makes no movie (staging node "no pre-roll", -16958); filters greyed (no Metal in mediaserverd);
  portrait and slo-mo (no depth, no 120 fps mode); video ~20 fps when recording (CPU ISP + encoder under thermal
  throttling); two whole-phone freezes while streaming (kernel log just stops; cause unknown).

## 2026-11-12 — video encoding: our own H.264 encoder for VideoToolbox (the S20 has no AVE)
- `userland/src/h264_encoder.c`: a VideoToolbox encoder (interface reverse-engineered from AppleVideoEncoder, see
  tools/camera/CHECKLIST.md) over OpenH264 2.7.0 (BSD, `work/third-party/openh264`, `make OS=ios ARCH=arm64`), registered
  in mediaserverd by libredwood-media (iOS runs compression sessions there; `h264=0` leaves it out). Baseline profile,
  constant SPS/PPS, 4 slices / 4 threads, key frame every second, ~12 Mbit/s at 1080p30.
- Front camera's video format 1920x1080 (was 3840x2160).
- Tested: `vt_enc_test 1920 1080 90` (public VTCompressionSession API on the phone): 90 of 90 frames at 52 frames/s;
  the stream decodes 90/90 on macOS VideoToolbox with the test pattern intact. Camera.app recording: to be confirmed
  (set Settings > Camera > Formats > Most Compatible: there is no HEVC encoder).

## 2026-11-12 — front camera orientation default (`cam_turn=2`)
- User-confirmed orientation (preview and photos): libredwood-media's turn plus a mirror; default from boot now
  (`cam_turn=` **2**), live via `tools/poke 'cam.turn N'` (read at stream start). Built (kernel 04:36:50), not flashed yet
  (phone with codex-cellular).

## 2026-11-11 — front camera autofocus, steadier exposure, anti-flicker, orientation
- Autofocus: the front lens motor (AK7374, ID 0x0e at 0x0c on HSI2C24, gpp8-0 SCL / gpp8-1 SDA, x1slte DT fragment@18) is
  driven from `redwood_camera_s20.c` (Samsung's is-actuator-ak737x.c sequence: standby, ID, first position 250, active;
  soft landing on stop). libredwood-media's contrast AF and tap-to-focus move it through REDWOOD_CAMERA_FOCUS.
- AE: frames are tagged with the exposure asked two frames earlier (when the sensor applies it), so AE no longer
  overshoots and swings (seen in photo mode); 50 Hz anti-flicker exposure steps (`camera_isp.c`); front camera turned
  180 degrees in libredwood-media (user: upside down and mirrored without it).
- Tested (kernel 22:03:35): kernel log "front focus: AK737x (id 0xe) up"; `camera_test raw 60`: 60 of 60 at 3648x2736.

## 2026-11-11 — front camera at full 3648x2736, 30 frames/s, no tearing
- The stream runs Samsung's `3648x2736_30fps` mode with the FULL MIPI PLL (`cam_s20_full=` **1**; 0 = binned 1824x1368).
  Double-buffered 12.5 MB frames take two adjacent display-pool slots (`MiniIOSurface.cpp`).
- Tearing fix: the finished buffer is chosen from the DMA frame-start/frame-end status (a frame start flagged together
  with a frame end is the next frame's), not just the active slot read after the fact.
- `kern.redwood.camera_turn` (boot-arg `cam_turn`): bit 0 makes libredwood-media turn the front camera's pictures half
  round, read at each stream start, to settle orientation without a new image.
- Tested (kernel 21:53:59): `camera_test raw 60`: 3648x2736, stride 4560, 60 of 60 frames ~33 ms apart; kernel frame
  copy ~1-5 ms.

## 2026-11-11 — front camera at 30 frames/s
- WDMA1 writes MIPI RAW10 directly (DMA format 7, lines padded to 16 bytes = the `/dev/redwood-camera` layout), read
  through a cached mapping (on by default, `cam_dma_cached=0` / `cam.s20.cached 0` to disable): one ~1 ms copy per frame
  instead of packing 2.5 M pixels from write-combined memory. No turning in the kernel any more; `camera_device.c`
  front camera `turned = false` (the S20's module is the right way up).
- Tested (kernel 21:47:05, root.apfs with the rebuilt libredwood-media.dylib): `camera_test raw 90`: 90 of 90 frames,
  33-35 ms apart; kernel log: frame copy 0.36-1.2 ms.

## 2026-11-11 — camera survives a cold start: S2MPB02 buck voltages set at boot
- After a hard reset (forced power-off) the camera PMIC has BUCK1 at 1.6 V (reg 0x0c = 0xc0) and BUCK2 at 0.8 V
  (0x0e = 0x40); Samsung's kernel programs both to 1.3 V (0x90) and those values had survived warm restarts since the
  last Samsung reference boot, which is the only reason the camera worked. `redwood_s2mpb02_torch.c` now writes
  0x0c/0x0e = 0x90 and 0x10 = 0x48 (buck-boost 3.5 V) before enabling BUCK1, the buck-boost and LDO18 (`cam_pmic_base`).
- Tested (kernel 18:54:31, first boot after a forced power-off + Download Mode flash): `cam.pmic 1` reads
  0b=d8 0c=90 0e=90 0f=da 10=48 2f=ec; `camera_test raw 45`: 45 of 45 frames, 33.9 ms apart.
- Also in this kernel: `cam.s20.cached [0|1]` (debug port, off at every boot) reads stream frames through a cached
  mapping; the kernel turns frames 180 degrees (`cam_s20_rot=1` default) to undo the old userland's `turned`.

## 2026-11-11 — live front camera for iOS (`cam=5 cam_s20=1`)
- `/dev/redwood-camera` now serves the S20's real front SAK3J1 instead of the chart: `redwood_camera_s20.c` keeps the
  proven sequence running (WDMA1 alternates two buffers over its 32 slots), `redwood_camera.c` `s20_session()` packs each
  frame (RAW10, turned 180 degrees: the module is mounted upside down; `cam_s20_rot=0` = as sent) into the ring the iOS
  bridge reads. Exposure/gain/frame length from the userland AE reach the sensor (0x0340/0x0202/0x0204, 0x20 = 1x).
- `x1slte.cmdline`: `cam_fake=1` replaced by `cam_s20=1` (`cam_fake=1` still gives the chart).
- Tested (kernel 13:56:21): `camera_test raw 45` on the phone: 45 of 45 frames, AE converges (32 ms, 9x indoors), white
  balance R 1.27 B 1.84, Bayer GB/RG after the turn; user saw live frames in Camera.app (before the turn: upside down).
- User verified in Camera.app: live preview and **photo saves to Photos**.
- Open (roadmap in `tools/camera/CHECKLIST.md`): video does not save and has no mic; ~15 frames/s; binned 1824x1368
  only and no colour tuning (soft, dated look); rear cameras, zoom switching and flash not done.

## 2026-11-11 — front camera link works: first real sensor frame under XNU
- **Root cause of "sensor programs, zero CSI packets":** the camera PMIC (S2MPB02) supplies BUCK1 (1.3 V), buck-boost
  (3.5 V) and LDO18 (3.3 V) are always-on/boot-on in Samsung's x1slte device tree but off after S-Boot; XNU never
  enabled them, so the SAK3J1 answered I2C but never started its MIPI link. Now enabled at boot in
  `redwood_s2mpb02_torch.c` (`cam_pmic_base=` **1**, 0 = leave as the bootloader did). Registers 0x0b/0x0f/0x2f read
  d8/da/ec = Samsung's.
- Tested on the phone (kernel built 13:40:44): `cam.s20.front.stream 14 0 4` and `6 0 0` → 17 frames in 500 ms, frame
  start/end, PHY active; `cam.s20.dma.test 1 14` → 1824x1368 RAW10 frame, DMA clean, guards intact; decoded picture in
  `logs/camera/pmic-base-boot-20261111/frame-1-0.png` (real scene). Proof on Samsung's kernel:
  `logs/camera/baseoff-split-20261111/` (same XNU sequence: supplies on 15 frames, off 0).
- Diagnostics added (opt-in third argument bits of `cam.s20.front.stream`: 4 line probe, 8 273-register dump, 16 match
  Samsung leftovers, 32 CSIS power-domain cycle — once per boot only, 64 base supplies per run), `cam.pmic 1` full map,
  shared register list + `tools/camera/compare-regdump.py`, Samsung reference split modes in `tools/reference-boot/`.
- Not yet: live preview in Camera.app (still the synthetic bridge), rear cameras, exposure/colour tuning.

## Open (known issues)
- S20 LTE/calls remain unimplemented. Cellular is isolated on `x1slte-cellular`;
  see `docs/devices/x1slte/cellular/s20.md`. Native Settings UI is installed/user-confirmed.
  CP memory reservations/live cache policy, SPI BOOT64496-byte transfer with
  safe shutdown, and9 PCIe UEFI mappings passed on phone. Nov12 live CP-off
  root-port identity access/cleanup passed without reboot. Endpoint link,
  remaining modem firmware, SIM/network/packet service and VoLTE are pending.
- Camera: front only, photos only. Video recording, microphone in video, full resolution, colour tuning, rear
  cameras, zoom switching and flash are open (`tools/camera/CHECKLIST.md`, section ROADMAP).
- Camera: rear 12 MP mode off (row-1912 band; binned 2016x1512 used), ultra-wide/tele not brought up
  (`tools/camera/CHECKLIST.md`, OPEN ITEMS).
- OpenGL ES games work only when named in build.sh `EAGL_APPS` (executable name); others still use iOS's
  GL-on-Metal and show black. ES 1 (fixed function) games are not covered. PVRTC textures are not decoded yet.
- Proximity is registered/enabled but near/far operation is not verified; raw diagnostics are available through `shub.prox`.
- True Tone is a fixed mild warmth approximation, not an ambient colour measurement. User can disable it normally.
- CoreMotion raw gyro samples work; the combined device-motion/attitude stream still returns no samples.
- The screen timeout is our stand-in (backboardd), not iOS's own idle timer: native fix is TASKS R1b.
- Mic: the kernel's diagnostic peak counter reads full scale on every capture although recorded files do not clip (only
  with `snd_debug=1`).
- The thermal fix below was confirmed on one cold start after the flash (peak 83 C, no restart); watch the
  `reset: last run ended by ...` line at the top of each boot log.

## 2026-11-08

### Compass heading from the phone's magnetometer (locationd, software fusion)
- locationd took the iPhone X's capability table at its word and waited for the motion coprocessor's fused
  streams (HID page 0xFF0C), so heading stayed at north. libredwood now runs inside locationd too
  (src/inject_location.c) and clears the coprocessor-fusion bits 3/4/5/7 of CLPlatformInfo::motionCaps() right
  after locationd computes them (dispatch_once GOT hook, data only: patching code drops CS_VALID and CloudKit
  then kills locationd). locationd now builds its software compass ("Legacy compass") and AP sensor fusion.
  Boot-arg loc_swfusion=0 turns it off.
- Phone verified live (runtime kern.exec_inject, no reboot): locationd stable, CLLocationManager heading
  calibrates (accuracy +-30 deg) and reads 88-89 deg with the phone pointed east (user's reference).
- Apple's Compass app: apps read device motion in-process through CoreMotion, which has its own copy of the
  table (motionCaps() at cache 0x1919a4580). libredwood now also loads into Compass and pre-fills CoreMotion's
  cached word (0x1e20a7290, token 0x1e20a7288) without the SPU bits, so CMDeviceMotion comes from locationd.
  User verified: the Compass app dial works. Other apps that want CMDeviceMotion need their process name added
  to the libredwood INJECT group in userland/build.sh.
- Gyro units for locationd's fusion: it took our degrees/s as radians/s, so resting noise turned the heading by
  ~2 deg/s and a reopened Compass drifted off. IOHIDEventGetDouble/FloatValue are hooked through locationd's GOT
  and convert gyro X/Y/Z only (apps' own CoreMotion conversion untouched). Boot-arg loc_gyro_rad=0 disables.
  Verified: heading steady for 40 s at rest; after a figure-8 calibration the Compass app read 94-99 deg with the
  phone hand-aligned east (user).
- Open: magnetometer scale. A figure-8 sphere fit gives Earth's field as ~11.4 "uT" at sens_mag_k=15000 (expected
  ~41.5 uT here), i.e. ~0.054 uT/count. Try boot-arg sens_mag_k=54000 at the next reboot; it should give the
  calibrator a field it trusts (better accuracy than +-30 deg).
- Flashed (recovery, 2026-11-08 10:02): locationd + Compass hooks permanent; magnetometer default now 0.054 uT/count
  (sens_mag_k=54000; 15000 restores). The hub sends Samsung's *calibrated* field (accuracy byte), so Apple's
  calibrator on top learned bogus biases (8 stale rows in consolidated.db CompassCalibration, cleared) and the app
  read 317 deg pointing north. CompassIgnoreCalibration=1 (mobile's com.apple.locationd via prefset at boot; also
  set in root's) makes iOS take the field as calibrated: user says Compass works, but turns slower than the phone.
- Open: dial lags turns -> check gyro scale (sens_gyro_k=61) with a stationary quarter-turn test (tools: b-gyrocmp);
  native alternative: feed Samsung's uncalibrated stream (GEOMAGNETIC_UNCALIB_SENSOR) to Apple's calibrator.
- Every app now (kernel): kern.exec_inject words starting with "/" match a path prefix; libredwood goes into
  /Applications/ and /var/containers/Bundle/Application/ apps, whose CoreMotion then gets device motion from
  locationd (verified: an unnamed program in the app folder got device motion; one outside got none).
  locationd's hook sets CompassIgnoreCalibration itself at every start (root's and mobile's prefs; boot-arg
  loc_magcal=1 lets Apple's calibrator work). Flashed 10:2x; after the reboot the user confirms Compass shows a
  real heading and moves, nothing set by hand.

### Optional mild True Tone and corrected gyro units
- True Tone's Settings toggle controls a Samsung DQE CGC 3x3x3 colour cube, R100% / G98% / B94%. No per-frame
  CPU/GPU filter, no edits to the panel's factory gamma/AOR tables. Preference persists; initially off.
  The earlier gamma-LUT attempt corrupted colours and was replaced. Generated neutral cube nodes match all81
  values in Samsung's bypass reference; runtime updates run between presentations with scanout briefly masked.
- Phone verified: toggle on/off leaves backboardd PID16 unchanged; OFF restores control0x80, path0xb1, size0.
  User confirmed "Mild warmth, other colours normal" on the colour-cube build. The original unsupported selector
  crash is fixed; a dedicated bounded display_warmth sysctl lets mobile backboardd apply the preference safely.
- Gyro bridge sends degrees/s to Apple's HID gyro service. CoreMotion itself converts to rad/s; removing the
  duplicate conversion fixes app readings that were57.3x too small. sens_gyro_k still specifies millionths of
  degrees/s per raw count (default61). Existing accelerometer/auto-rotation path is unchanged; user confirms it works.
- Verified with public CMMotionManager: timestamped live gyro samples, near-zero at rest, substantial response to
  user rotations (observed peak12.36rad/s). tools/sensors/gyro-probe.m checks samples, rates and fused stream status.
  Combined attitude/device-motion still provides no samples and is not claimed working.

### Adaptive brightness — `00f5122`, `1f8099b`
- Native Auto-Brightness setting controls the port-side policy. Stable light is accepted after 3 seconds brighter /
  5 seconds darker (user-adjusted from 5/8), then ramped smoothly. Manual slider changes anchor the preference at the current lux;
  substantial ambient changes blend back toward the base curve. Anchors persist after 10 seconds without edits.
- Wake holds the current level for at least 5 seconds; screen-off and stale-sensor gates stop brightness writes.
  Settled sampling is 2 Hz, inactive 1 Hz, with up to 20 Hz during ramps. No GPU brightness filter.
- Phone boot and matching native/panel nits verified. User confirmed brightness responds to cover/light tests and
  the True Tone option appears. User subsequently accepted auto-brightness as done. Host policy tests cover
  debounce, spikes, manual anchors, wake, monotonicity and bounds.
- Download Mode recovery: stop `tools/serial-watch.sh` and its `cat /dev/cu.usbmodem...` child before Heimdall;
  they hold the serial interface. Stock Heimdall then flashes successfully without administrator privileges.

## 2026-11-07 (evening)

### Display: FHD+/HD+ modes up to 120 Hz, factory brightness, smooth power-off — `469ca87`..`b37e85a`
- Resolution switch fixed (DECON refused every frame: the window's source still delivered 1440x3200): live dev
  command `fb.mode N 1` (1 = 1080x2400/120, 2 = 1080x2400/60, 3 = 720x1600/120, 4 = 720x1600/60); boot `fb_mode=`.
  iOS reports 120 (UIScreen.maximumFramesPerSecond) after the timing metadata refresh. Apps' actual fps varies.
- Factory brightness: the panel's own gamma/AOR tables from its SPI flash (60 and 120 Hz), checksum-verified,
  on by default (`fb_cal=1`). User: range, direction and colours correct.
- Power-off: the picture fades (DECON window alpha) instead of the backlight; then display off; frames stopped
  while dark (trigger masked) so the screen-off profile (memory 421 MHz) engages. User: "much much better".
- Dev knobs: `fb.fade [dd [offms [alpha]]]`, `fb.factory 0|1`, `fb.mode`.

## 2026-11-07

### Sleep: screen timeout, screen-off power profile, Low Power Mode
- **Screen timeout works** (it never did): SpringBoard's idle timer depends on AttentionAwareness, which hands it an
  untagged lost-attention event here ("attention client lost event tag is not a number") and it never dims. Our own
  idle timer in backboardd (`userland/src/inject.c`) presses the lock button after **10 s** when woken and not touched
  (`idle_lock_s=`), or the **Auto-Lock** time once touched (SpringBoard's lock state is unreliable here). **Off by default** (user, 18:55): `idle_lock=0` off, `1` only an untouched wake, `2` also Auto-Lock (Settings value; Never = off), not while sound played in the
  last 5 s (`kern.redwood.snd_quiet_ms`). `idle_lock=0` turns it off. Tested by the user: lock screen 10 s, home screen 60 s. Known gap: a silent video unlocked still locks.
- **Screen-off profile** (automatic, no switch): memory 421 MHz (was 2730 always; 2.7 vs 15.7 GB/s measured), every
  CPU cluster at its low step, GPU lowest (`bsd/dev/redwood_exynos_acpm.c`, `dvfs_sleep=0` off). Full clocks again
  the moment the display comes on (before the first frame).
- **Low Power Mode** (Settings switch, also iOS's automatic one): `lpmd` (`userland/src/lpmd.c`, job
  `com.redwood.lpmd`) passes it to `kern.redwood.lowpower`: memory 1352 MHz, clusters at their first step, GPU 572
  (`dvfs_lpm=0` ignores it).
- Tested: lock screen idle -> "idle timer: 10 s ... display off" -> "power: screen off: memory 421 MHz"; Low Power Mode
  on -> "memory 1352 MHz, clusters and GPU capped".
- Unused here but kept: backboardd's `com.apple.private.attentionawareness` entitlement lookup answers yes if it
  comes back empty (`aw_ent=0` off).

### Fix: restarts at the lock screen after a cold start / flash — thermal throttling
- Cause (measured): the big M5 cluster reached 108-111 C during boot (all clusters at top clock); the SoC's thermal
  trip reset the phone through the PMIC (`RST_STAT` PINRESET, S2MPS19 PWRONSRC WTSR, OFFSRC 0), 2-3 times, until a boot
  started cool enough. The governor read temperatures once a second, throttled at 86/96 C and went back to full clock
  6 C below the mark.
- Now: temperatures every 250 ms; CPU steps at 80/90 C, GPU 82/92 C; full clock again only 10 C below the mark
  (`bsd/dev/redwood_exynos_acpm.c`; `dvfs_hot=` still overrides). Idle is 55-60 C, so normal speed is unchanged.
- Tested: cold start after flash: peak 83 C, throttled at 80 C, back to 2730 MHz at 69 C, no restart.
- Every boot now logs why the last run ended, decoded with Samsung's names:
  `reset: last run ended by RST_STAT ... ; PMIC on ... off ...`.
- Log access / back-to-menu no longer panics ("Console I/O from interrupt-disabled context"): `redwood_dsay` and
  `snd_log` skip the console when interrupts are off (the sensor hub's stop message did it).

### Audio: debug off by default, self-checks, quieter speaker and vibration — `26dfb5b`
- `snd_debug=` **0**: routine log lines (amps on/off, 60 s stats, mic start/stop), mic per-sample peaks and the
  adapter's per-write peak scan + report only with `snd_debug=1`. Errors are always logged.
- Self-checks: amplifier fault -> reset and set up again (3 per boot; before: off until restart); every quiet minute
  both amps must answer on I2C, else recovery (3 per boot); mic WDMA4 stuck 200 ms -> capture path restarted; playback
  DMA stalls counted. `rwsys kern.redwood.poke snd.health` reports it all (memory only, safe while playing).
- Speaker maximum 70 % of before: `snd_below=` **109** (1/8 dB under the amps' full level; was 84).
- Vibration maximum 60 % of before: `haptic_max_pct=` **34** (was 56).
- Debug command table 32 -> 48 entries (it was full).
- Tested: tone, 4 s record with vibration, `snd.health` clean, log shows `109/8 dB` and `at 34 %`.

### Microphone: faster start, no idle CPU; amp I2C fix — `ac6c313`
- Mic worker sleeps while nothing records (was waking every 10 ms), polls every 4 ms while recording, 20 ms settle:
  capture starts in ~40 ms (was 100-120 ms).
- GPIO: mic power / haptic reset pins share a bank with the speaker amps' bit-banged I2C; a neighbour's
  read-modify-write could set the I2C lines' output latch high. Lines are now driven low explicitly every time.
- Tested: record, record + vibration, record + system sounds (6 s, 0 dropped frames), both amps answer after.

### Microphone works in iOS — `2dc3b49` (and `d206416`..`976b8d7`)
- `/dev/redwood-mic` (48 kHz, 16-bit mono) fed from the three DMIC converters (VTS S_LIF -> UAIF6 -> NSRC4 -> WDMA4);
  iOS sees "iPhone Microphone"; Voice Memos records. `mic_enable` (default on with `snd=1`).
- Watchdog WDT_CLUSTER0 switched off at boot (`wdt_off=` **1**): it restarted iOS at the lock screen (`36fc39a`).

### Speaker sound works — `150a7cd`, `4ffbc65`, `4ed2fa0`, `976b8d7`
- ABOX RDMA3 -> ASRC -> UAIF2 -> two CS35L41 amps, Samsung's register set (`snd=1 snd_samsung=1`). Amp start
  retried at boot and recovered on the playback worker.

## Earlier (2026-11-06, see git log)
Charging (watchdog kick, PD/AFC 9 V, limits), live battery %, USB serial + USB networking + SSH, flashlight, Wi-Fi
signal and power save, Samsung DVFS tables, iOS-only storage (system on `super`, 108 GB `/var`), UFS TurboWrite
(`ufs_tw=1`, `6dbc8c5`), rotation (`shub=6`), vibration (`haptic=5`), Location Services, Bluetooth audio fixes.

## 2026-11-20 publication review (A, user decisions)
- Prebuilt programs (prebuilt/userland, kernel/loader prebuilts): published, each by SHA-256 in
  tools/release/publication-reviewed.json; tools/release/audit.py skips a reviewed file only while its content is
  unchanged, and never skips a credential match.
- Default wallpaper: the user's own image, deliberately the single clean-install default (stock gallery otherwise).
- docs/research: not published; untracked and git-ignored, kept on the developers' Mac.
- Deleted: legacy y2s UEFI DTBs, an unused fingerprint image, an empty job file. (The 64 GB HFS kit looked unused but is the release's: targets.kit_path computes its name from IOS_DATA_GB; restored.)

## 2026-11-20 (A) — v0.1.2 built (tag v0.1.2 = 4fe87765)
- Package: Jesvi-iOS_0.1.2_20261120_x1slte.zip, 2,723,860,609 bytes, sha256 6c035d83387d8556ca56cc595cf0412225755953fc80ee17129f0f39d2447d6a,
  `SHARE=1 ./build.sh local` in the clean checkout (source_dirty false); kept on the SSD (iOS-Jesvi/builds), not on GitHub.
  Checked in recovery.img: charging limits, SD driver, temperature protection, dyncap, GPU layer, telephonyd job;
  boot line ufs_userdata_all=1. Not yet installed on a phone from the package (installer_validated false).
- Draft release v0.1.2 on GitHub: restore ZIP from Actions.
- Fixed on the way: camera build scripts' ldid path (clean checkouts), the 64 GB HFS kit restored.

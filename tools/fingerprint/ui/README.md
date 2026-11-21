# Touch ID rejection preview

This staged native UIKit interface connects to the real Samsung enrollment and
identify loops. It adds **Settings → Touch ID Preview** and an opt-in,
noninteractive fingerprint prompt on the lock screen. An actual `NO MATCH`
changes the prompt to **Try Again**. Presence alone does not produce that result.

This is a separate preview entry, **not Apple's stock Touch ID & Passcode page**.
It does not register a BiometricKit/LocalAuthentication device, enable application
biometric prompts, add native identities, or grant authentication. Those provider
and credential-binding integrations remain future work. Passcode unlock stays
in place. The preview is not installed or device-tested yet.

## Build and package locally

From the repository root:

```sh
tools/fingerprint/ui/build.sh
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/fingerprint/ui/test_model.c -o out/fingerprint/ui-test-model
out/fingerprint/ui-test-model
```

Run the read-only preflight separately from the host:

```sh
python3 tools/fingerprint/ui/preflight.py
```

Exit2 means prerequisites are missing; exit1 means the check could not complete.
It starts no TEE operation, installs nothing and never reboots. Current phone
preflight confirms the saved template, factory slot files, driver directory,
storage-daemon binary and passcode tweak are present. The running kernel lacks
`reject-v1` and TEE is not started, so the backend test is not ready. File presence
alone is not runtime validation.

The build creates signed arm64 iOS 16 binaries in `out/fingerprint/ui/`; it does
not deploy or restart anything. Build requires the local iOS 16.5 SDK and existing
XNU clang wrapper. `package.py` produces a reviewed tar bundle and hash manifest
from those binaries. This is a developer preview bundle, not a recovery/TWRP ZIP.

The kernel must include this branch's automatic legacy storage-path restoration,
cooperative `fp stop`, and status marker:

```
fingerprint ui: reject-v1 (no authentication)
```

The broker refuses to operate on older kernels. It requires TEEGRIS and the driver
TAs to be started, `teetsd` running, and the phone's own factory CBGE files already
installed privately. Use the documented existing TEE initialization procedure as
the phone-lock holder. The preview does not start the secure OS/driver TAs itself;
after a reboot this initialization is still a developer step. Never distribute
this phone's templates, calibration, firmware, passcode or keybags in the bundle.

## Coordinated installation and removal

Check the shared phone lock first. Installing a new kernel requires the user's
reboot approval. The exact compiled kernel and UI bundle have not been boot-tested.

Copy/extract the preview bundle privately on the S20, then run
`sh install-on-phone.sh` as root from the extracted directory. The installer
checks the kernel marker before copying files, refuses replacement of an existing
installation, starts the root broker with launchd, and **does not** restart
SpringBoard or reboot. Relaunch Preferences to load the Settings entry. Loading
the SpringBoard filter needs a coordinated SpringBoard restart; preserve current
keybags and the separate software-passcode/relock tweak throughout.

Remove the preview without changing biometric data or passcode:

1. Boot out `system/com.redwood.fingerprint-ui` with the phone's launchctl.
2. Remove only `RedwoodFingerprintPreview.dylib` and its plist from
   `/var/jb/Library/MobileSubstrate/DynamicLibraries/`.
3. Remove `/var/jb/Library/LaunchDaemons/com.redwood.fingerprint-ui.plist` and
   `/var/jb/redwood/bin/redwood-fingerprint-ui`.
4. Relaunch Preferences and coordinate a SpringBoard restart to unload the tweak.

Stop the preview broker before using the independent fingerprint research tools.
Do not run a kernel flash or another TA command session concurrently with it.

## Behavior and boundaries

- Setup must be requested explicitly from Preferences while MobileKeyBag reports
  unlocked. This is a development gate, not completed challenge-bound enrollment
  authorization. It never collects a passcode. There is one preview fingerprint.
- Setup confirms replacement. The broker first preserves the existing encrypted
  template in a new root-only `/var/root/fp-template.before-ui.<uptime>.bin` file.
  It refuses setup if that backup fails. These backups are private and need
  deliberate later retention/cleanup; removal does not delete them.
- Setup shows real kernel progress. `100%` alone is insufficient: the page shows
  **Fingerprint Added** only after the kernel reports a successful file save.
- Between 15-second enrollment segments the broker sends Init again, continuing
  the existing enrollment. It never sends Prepare between touches/segments.
- Comparison uses the saved template and the actual TA result. Only `NO MATCH`
  produces Try Again. A protocol error is unavailable; an unverified candidate
  produces Use Passcode, never success.
- Lock-screen preview is off by default and enabled explicitly in Settings. It
  resets when the broker restarts. A wake starts at most one 15-second comparison
  window; the prompt then directs the user to passcode. No background endless scan.
- UI lease expiry (three seconds), leaving the page, cancellation, screen-off,
  or keybag locking during enrollment cancels owned work. Enrollment is capped at
  five minutes. Stop is cooperative: an in-flight secure-world call or requested
  delay must return before the worker can exit. No second TA command races it.
- The overlay is not a key window and does not receive touches. It never invokes
  SpringBoard authentication or dismissal methods. Position uses the documented
  sensor center (panel y1860/2400) and still needs handset verification.
- The root-owned UNIX socket accepts only mobile UID501 peers whose kernel-reported
  executable path is Preferences or SpringBoard. Only Preferences may start
  enrollment or enable the preview. Replies contain UI state only, no templates,
  calibration, raw kernel log, HAT or credential material. This is local port
  isolation, not a substitute for a production signed biometric provider.
- No capability spoofing, fabricated failure result, injected authentication
  success, match-threshold change, stored PIN, Gatekeeper change or keybag write.

## Local UIKit compatibility review

The local iOS16.5 Preferences cache confirms `PSViewController` implements the
specifier, parent-controller and root-controller setters used by Settings. The
preview now inherits that class; a plain UIViewController did not implement this
contract. The root list refreshes after installing its hook, covering a cached
Settings root. The SpringBoard prompt also supports ordinary UIWindow instances
when there is no UIWindowScene. Both changes cross-compile; actual page navigation
and overlay rendering remain device tests.

## Required device validation before calling the UI integrated

1. Old kernel: installer/broker refuses operation before any TA write.
2. Unauthorized socket peer: no state or command response.
3. Settings while locked: enrollment refused. Existing template backed up before
   replacement; progress and successful save correspond to actual device logs.
4. Cancel mid-touch, leave page, display-off, lock during setup, and kill the UI:
   owned capture stops, progress history is not replayed, no extra scan starts.
5. Enable preview, lock and wake: marker aligns with sensor; real no-match shows
   Try Again. Unknown/error/candidate states never display authenticated.
6. After testing, passcode unlock → lock → passcode unlock works again. No
   passcode, saved bag, native biometric identity or native auth state is altered.

Host model tests and cross-builds do not establish those device results. The
remaining system-wide Touch ID provider work must be separately implemented and
verified; this preview does not mean “everything except matching” is complete.

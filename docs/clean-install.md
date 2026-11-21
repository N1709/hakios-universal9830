# Clean install: why features went missing, and the rule that stops it (2026-11-20)

## What happened

The phone has two parts:

- **The image** (kernel, root file system, `/redwood/...`): built from this repo, flashed, the same on every install.
- **The data volume** (`/var`: `/var/root`, `/var/mobile`, `/var/jb`, preferences): survives updates, but a clean
  install starts it empty.

Several features worked only because files had been copied onto the data volume by hand during development. They
were never in the image or made at first boot, so a clean install (2026-11-19/20) lost them all at once: SIM and
data (the modem files), Location Services (so Compass and Maps), Files' "On My iPhone", keyboard haptics, Live Photo
default, and hooks in `/var/jb`. The code was all on `main`; what each feature needed on the data volume was not.

## The rule (every session)

**Anything a feature needs at run time must come from the image, or be made by the phone at first boot.**
Never leave a feature depending on a file you copied to `/var` by hand. When you `scp` something to the phone to
make a feature work, the same change goes into one of these before the work counts as done:

| Kind | Where it goes |
|---|---|
| Our own program or library | the image (`userland/build.sh` → `root/bin`, `root/lib`) |
| A file that must live on the data volume (`/var/jb`, `/var/root`) | `root/etc/seed.list` (init copies it when missing) |
| A preference default (Location Services, haptics, camera) | the first-boot defaults (see "Defaults" below) |
| Device firmware we may not ship (modem, NV, calibration) | made on the phone from its own partitions, read-only (example: `tools/cellular/cell-provision.c`) |

Test it the honest way: move the files aside (or use a clean data volume), reboot, and see the feature come back by
itself. Write in `docs/SHIPPING.md` that it survived a clean install.

## Status of what went missing on 2026-11-20

| Feature | Needed on the data volume | Status |
|---|---|---|
| SIM, data, VoLTE | `/var/root/cell-*` modem files | **made automatically, clean-install tested** by `cell-provision` from RADIO + EFS ([cellular/provisioning.md](devices/x1slte/cellular/provisioning.md)) |
| Location Services (Compass, Maps, GPS) | locationd's switch | **first-boot default, verified on a clean install 2026-11-20** (`redwood-defaults`: `+[CLLocationManager setLocationServicesEnabled:]`, entitled; the preferences key alone is ignored by iOS 16.5 and Settings then hides the row) |
| Keyboard haptics on | `com.apple.keyboard.preferences` `KeyboardVisceral = 2` | **first-boot default** (`redwood-defaults`) |
| Live Photo off | `com.apple.camera` `CAMUserPreferencePreserveLivePhoto = true`, `..DesiredIrisMode = 0`; seed `..ForcedLivePhotoOnTenTwo` and `..DidPerformAutoLivePhotoUpgrade` so Camera's first launch does not turn it back on | **once**, including repair of v1 installs (`redwood-defaults`, `.redwood-camera-defaults-v2`); later user choices persist |
| Files: On My iPhone | LaunchServices record of `org.hakios.LocalFilesHost` **with its 3 extensions** (`com.apple.FileProvider.LocalStorage`) | **fixed**: `register_finder_host` checks each extension and registers the whole record when one is missing (a plain `registerApplication:` of the app in `/Applications` left them out on a clean volume). Verified on the phone 2026-11-20. The host is registered as a **System** app (hidden tag honoured; as a User app SpringBoard kept a dead "Jesvi Files" icon), generated at first boot from Apple's native extensions (codex-files-resume) |
| Touch ID: the fingerprint TA set | `/var/root/tee` (Samsung vendor/tee: FINGE TA + driver TAs) | copied by `redwood-fpd` from the system image (`/System/Library/Redwood/Fingerprint/tee`, from `device/firmware/vendor-tee`); **local builds only, never `SHARE=1`** (Samsung firmware). Public images: to do, capture the phone's own `/vendor/tee` at install time, before `super` gets placeholders |
| Touch ID: sensor calibration | `/var/root/fp-calib/cbge_0.dat`, `cbge_8.dat`, `sensor_id` | **made automatically** by `cell-provision --fp` from the phone's `sec_efs` (`biometrics/meta`), read-only; byte-identical to Samsung's (2026-11-20) |
| Touch ID: the sensor module | `/usr/lib/libBKDM1.dylib` | in the image: `tools/touchid/fetch-bkdm1.sh` ports the iPhone 8's onto the iPhone X cache ([touchid-port.md](devices/x1slte/touchid-port.md)) |
| Touch ID enrolment | `/var/root/fp-template.bin` (made by enrolling) | by design: enrol after a clean install |
| Apple account layer | `/var/jb/usr/lib/libredwood-account-*`, boot job | needs the image (sessions/B-apple-account-handoff.md) |

## Defaults

Preferences that iOS keeps on the data volume are set once per data volume by a first-boot step:
`redwood-defaults` (boot job `com.redwood.defaults`), once per data volume (marker `/var/root/.redwood-defaults-v1`),
so the user's later choices are never overwritten. A new default: add it there and bump the marker's version.

### Files registration follow-up (2026-11-20)

Framework-only registration regressed: `registerPlugin:` returned success but
left the local extension's entitlements and container metadata empty. A proxy
existed, while the local FileProvider domain did not. The helper now prepares a
hidden registration host from the image's framework bundles automatically, and
registers the full app/extension/container record. `LocalFilesHost` is included
in `/redwood/bin`; nothing must be copied to `/var` by hand on a clean install.
Both the bundle and its LaunchServices record carry the `hidden` app tag.

Check all three plugin records, including LocalStorage's group mapping and
entitlement, then open Files and confirm On My iPhone. Compilation is checked;
phone repair and a generated-host recreation check remain pending.

# Adding an Exynos 990 board

The first five families are S20, S20+, S20 Ultra, Note20 and Note20 Ultra. S20 FE
Exynos is a later candidate. Only `x1slte`/SM-G980F currently has a measured profile;
the other directories are invitations to maintain a port, not compatibility claims.

## Contributor workflow

1. Fork the source repository; create a branch/worktree. Run `./build.sh devices`
   and `bash ci/check.sh`. Read the source conventions and preserve attribution.
2. Own an exact target model and a working stock-restore path. Declare the model,
   region, bootloader, board revision, RAM/storage sizes and maintenance contact.
   Do not publish serials, IMEI/IMSI, calibration dumps or personal backups.
3. Fill `devices/exynos990/<board>/device.json` and an evidence document. Model and
   bootloader allowlists must enumerate verified values. Never replace them with
   wildcard `SM-G98*` or a marketing-name substring.
4. Collect the table below. Cite the Samsung kernel DTS/driver file and line or a
   sanitized measurement for each hardware claim. Label unknowns explicitly.
5. Add a real board adapter and UEFI/loader configuration; update cache inputs,
   image validators and fixtures. The current executable adapter intentionally
   supports only x1slte. Adding JSON does not make `build.sh images` work on a board.
6. Build from a fresh isolated checkout. Validate output geometry without touching
   the phone. Bring up serial/logging and a recoverable loader before drivers.
7. Submit hardware evidence separately for boot, display, touch, storage, charging,
   modem, cameras and runtime. A visible home screen is not storage/thermal validation.
8. Perform first-install/update/rescue qualification. Only then submit a change
   to the status and installer qualification policy, with evidence tied to an
   exact release candidate. The validator currently rejects flipping
   `installer_validated` to true because no installer backend has been qualified.

## Required board facts

| Area | Evidence required | Why shared Exynos 990 is insufficient |
|---|---|---|
| Boot chain | Header version, page size, Samsung DTBH/DTBO, UEFI target, AVB behavior | sboot checks image structure and model data |
| RAM | Physical banks, secure/CP/ABOX/framebuffer reservations, early allocation windows | 8/12 GB variants have different limits; overlapping modem memory breaks runtime |
| Storage | PIT/by-name map, partition byte sizes, logical metadata, sector size | userdata and super windows are currently exact-capacity assumptions |
| Display | Panel ID, resolution/DSC/timings, GPIO/regulators, backlight | Hubble panel variants and Note panels are not interchangeable |
| Touch/buttons | Controller, I2C/SPI, IRQ/GPIO, firmware and calibration ownership | Firmware and board wiring differ |
| Wi-Fi/BT | Chip/module revision, NVRAM/CLM, address provisioning | Never ship the developer's MAC/NVRAM identity or network config |
| Audio/haptics | DSP windows, firmware source, codec/amplifier/waveform IDs | Board routing and vendor calibration differ |
| Cameras | Actual sensor/module IDs, clocks, GPIO, calibration references | S20, Ultra and Note camera combinations differ |
| Power/thermal | Charger/fuel gauge, regulators, temperature policy | Booting does not prove safe charging or suspend |
| Sensors/GNSS | Hub firmware, SPI map, GPIO/interrupt and protocol | Different module combinations need explicit testing |
| Cellular | CP memory map/firmware, modem transport, carrier behavior | Jio success on one device does not establish all carriers/VoLTE |
| Note extras | S Pen/digitizer path and UI behavior | Separate functionality, not inherited automatically |

## Layering rules

`devices/exynos990/platform.json` contains family-level policy, not board offsets.
Each board document owns model/geometry/evidence. Shared driver fixes remain in
XNU/userland; board constants should gradually move to validated configuration or
loader handoff rather than branching by display name throughout the drivers.

The present `common/devices/x1slte.conf` and `x1slte.sh` remain the working legacy
adapter. Keep them alongside the machine-readable profile until a generic adapter
has been tested. Do not rewrite functioning board drivers just to make directory
names prettier before release. Add parity checks when moving each value.

For a new model/capacity inside a family, create a distinct board profile if any
geometry, memory reservations, firmware or boot-chain behavior differs. Never
broaden an existing allowlist to make a validation failure disappear.

## Maintainer responsibilities

Maintain a tested firmware baseline; reproduce clean builds; own the hardware
checklist; keep rescue images and backups privately; triage model-specific bugs;
state which carriers/features were tested; update known issues for each release.
A maintainer without current hardware access should mark the profile unmaintained.
Source contributors can still improve common code without claiming device support.

## Experimental S20+ and S20 Ultra kernel builds

The Exynos Galaxy S20+ (SM-G985F and SM-G986B) and S20 Ultra (SM-G988B)
have a manual source-build entry point for port contributors. On macOS:

```sh
./build.sh kernel --device y2s --experimental
./build.sh kernel --device z3s --experimental
```

The output under `out/experimental/<board>/` is the shared GICv2 Exynos 990
kernel plus its build manifest and checksum. The target name records which
bring-up it was built for. No board-specific settings are applied by this
kernel build. Boot on either model is unverified, and there is no installer
or GitHub Actions build for these models.

For first boot, bring up a board-specific UEFI and loader configuration from
the model's own boot header, DTB/DTBO, RAM reservations and framebuffer handoff.
The S20+ has a [legacy UEFI starting point](../../devices/exynos990/y2s/legacy/uefi-y2s/).
Its historical flashing script encodes fixed storage offsets. Measure the
target's own partition map before assembling any images. Bring up early logs
first, then the display and touch. Camera support can follow after boot;
modules and calibration need separate hardware validation.

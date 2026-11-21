# Jesvi iOS

iOS 16.5 running natively on the **Samsung Galaxy S20 (SM-G980F, Exynos 990)**.


[![Latest Release](https://img.shields.io/github/v/release/jesvi-eulah/Jesvi-iOS?label=Download&style=flat-square&logo=github&color=brightgreen)](https://github.com/jesvi-eulah/Jesvi-iOS/releases/latest)
[![All Releases](https://img.shields.io/badge/View-All%20Releases-blue?style=flat-square&logo=github)](https://github.com/jesvi-eulah/Jesvi-iOS/releases)
![Model](https://img.shields.io/badge/Model-SM--G980F%20%7C%20x1slte-555555?style=flat-square)
![iOS](https://img.shields.io/badge/iOS-16.5-007AFF?style=flat-square&logo=apple&logoColor=white)
![Status](https://img.shields.io/badge/Status-Beta-orange?style=flat-square)
[![Telegram](https://img.shields.io/badge/Telegram-@eulah__rom-26A5E4?style=flat-square&logo=telegram&logoColor=white)](https://t.me/eulah_rom)




This tree builds the XNU kernel, the boot loader, the drivers and the user-space glue. The iOS system comes from
Apple's own iOS 16.5 firmware, downloaded at build time. Nothing of Apple's or Samsung's is in this repository.


[![Jesvi iOS 16.5 Demo](https://raw.githubusercontent.com/jesvi-eulah/Jesvi-iOS/main/docs/IMG_5690%202.jpg)](https://www.youtube.com/watch?v=-4pcSUe0ny0)<br>
[Watch on YouTube](https://www.youtube.com/watch?v=-4pcSUe0ny0)


## Info
| | |
|---|---|
| **Version** | 0.1.3 in preparation (last tag: [v0.1.2](https://github.com/jesvi-eulah/Jesvi-iOS/releases)) |
| **Phone** | Galaxy S20 LTE, SM-G980F (`x1slte`), bootloader G980FXXSNHYB7 |
| **iOS** | 16.5 (20F66), iPhone X system image |
| **Kernel** | XNU 8796.121.2 (Darwin 22.5) with our drivers |
| **Install** | TWRP ZIP: fresh install or update, 64 GB iOS data volume |

## What works

| Area | Status |
|---|---|
| Display | All, FHD+/QHD/FHD, 60 and 120 Hz, True Tone, auto-brightness, Always-On Display, Touch, buttons |
| Graphics | Accelerated, Metal on the Mali-G77, OpenGL ES through Mesa |
| Cellular | All, 4G/LTE data, VoLTE calls in and out, carrier SMS |
| Connectivity | Wi-Fi 2.4 and 5 GHz, Bluetooth, Bluetooth audio, hotspot |
| Cameras | All, camera, Front and three rear lenses |
| Audio | All, Speaker, earpiece, microphone |
| Sensors | All, Motion, compass, proximity, gyro, ambient light; GPS, Fingerprint, thermals, Haptics, flashlight |
| Power | All, PD Charging, battery level and temperature, Tuned CPU/GPU governor, thermal manageament |
| Storage | All, Full speed peristent UFS storage, Ram allocate max & at full speed, microSD (exFAT, read/write) in Files |
| USB | Networking, Finder file access, serial console, tethered |
| Security | Passcode; Touch ID enrolment, Apple-Id (Requires Hackintosh) |

**Partial or Not yet:** 
 - Touch ID unlock (enrolment works, matching does not), 
 - App Store downloads (FairPlay DRM needs to be eumulated/cracked yet), 
 - iMessage and FaceTime (Not possible), 
 - Standalone Apple ID sign-in (it does work but currently requires a peer hackintosh to set it up)
 - Bugs & glithces 

Per-part detail: [`device.conf`](devices/exynos990/x1slte/device.conf). Every change: [SHIPPING.md](docs/SHIPPING.md).



## Downloads
**[Download Latest Release](https://github.com/jesvi-eulah/Jesvi-iOS/releases/latest)** | [View All Releases](https://github.com/jesvi-eulah/Jesvi-iOS/releases)

Download the latest build, SHA-256 checksum, and build information from the release page.

## Install

Full steps in the **[install guide](docs/release/install.md)**. In short, with the phone in TWRP 3.7.1 or newer:

```sh
tools/install/twrp-install.sh --erase Jesvi-iOS_<version>_x1slte.zip   # fresh install (erases the phone)
tools/install/twrp-install.sh Jesvi-iOS_<version>_x1slte.zip           # update (keeps your data)
```

From a phone already on Jesvi iOS, `tools/install/flash-twrp.sh` puts TWRP back and boots it, with no buttons.
Back to Android: the restore ZIP, then *Format Data* and any ROM ([rollback](docs/release/rollback.md)).

## Build

On macOS:

```sh
make doctor     # what this Mac still needs
make check      # source checks and tests (what CI runs)
make kernel     # kernel only -> out/kernel/mach.x1slte
make release    # install ZIPs -> dist/release/
```

Settings live in [`release.conf`](release.conf); `./build.sh --help` lists every command. A clean build fetches
iOS 16.5 and the ported Touch ID module from Apple, and takes the phone's Samsung firmware from private `fw-*` folders.
Guides: [building](docs/release/building.md), [local build](docs/release/local-build.md),
[CI and releases](docs/release/operations.md), [clean-install rules](docs/clean-install.md).

## Layout

| Path | Contents |
|---|---|
| `xnu/` | Kernel: patch on Apple's `xnu-8796.121.2` plus our files (`xnu/files/`) |
| `xnuloader/` | UEFI loader that starts XNU |
| `userland/` | System image: daemons, injected libraries, boot jobs, first-boot defaults |
| `devices/` | Exynos 990 layer and one folder per phone (`x1slte/`) |
| `tools/` | Per-subsystem tools: `install/`, `release/`, `camera/`, `cellular/`, `touchid/`, ... |
| `docs/` | `release/` install and build, `devices/x1slte/` feature notes |
| `build.sh`, `Makefile`, `x1slte.sh` | Entry points; `x1slte.sh` is the developer flash loop |

## Other Exynos 990 phones

Only the SM-G980F boots. The S20+ (`y2s`) and S20 Ultra (`z3s`) kernels build
(`./build.sh kernel --device y2s --experimental`) but have never booted; each needs its own boot chain, device
tree and storage layout ([porting](docs/release/porting.md)). Snapdragon models are not supported.

## Credits

Built on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel). Apple's open-source XNU (APSL 2.0), Mu-Silicium,
Mesa and the others in [NOTICE](NOTICE) and [third-party licenses](docs/legal/THIRD_PARTY_LICENSES.md).
Not affiliated with Apple or Samsung. [Legal](docs/legal/LEGAL.md).


Please feel free to use this project for further developement, Just make sure to add me in credits, Cheers

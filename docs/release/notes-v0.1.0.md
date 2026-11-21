# Jesvi iOS 0.1.0 — Galaxy S20 (SM-G980F)

The first packaged build: iOS 16.5 on the Exynos 990 Galaxy S20 LTE, installed from TWRP.

## Files

| File | What |
|---|---|
| `Jesvi-iOS_0.1.0_<date>_x1slte.zip` | install / update iOS from TWRP (keeps your iOS data when it is already installed) |
| `Jesvi-iOS-Restore_0.1.0_<date>_x1slte.zip` | back to Android: puts an empty Android partition map back, then flash any ROM |
| `*.sha256` | checksums |

## Install

Tested on the S20 on 2026-11-18 ([record](qualification-x1slte.md)).

1. TWRP **3.7.1 or newer** (the official 3.7.0 is too old for current bootloaders); boot it.
2. Cable **straight into the computer**, then: `tools/install/twrp-install.sh Jesvi-iOS_0.1.0_<date>_x1slte.zip`
3. Hold **Volume Up** when the phone asks. An existing Jesvi iOS keeps its data.
4. `adb reboot`: the Apple logo, then iOS.

Only the SM-G980F with bootloader `G980FXXSNHYB7` is accepted; the installer checks before it writes. EFS, modem,
bootloader and param partitions are never touched. The restore ZIP is run the same way.

## Works

Display at 120 Hz with auto-brightness and AOD; Metal and OpenGL ES games; 4G data, VoLTE calls and SMS;
front and three rear cameras with video; Wi-Fi, Bluetooth audio, USB networking and Finder; touch,
buttons, vibration, charging, sensors and compass, passcode, thermal control; the screen sleeps on the
lock screen and stays on while you use the phone.

## Not yet

Fingerprint, full GPS, App Store purchases.

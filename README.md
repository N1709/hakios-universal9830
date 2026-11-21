# HackiOS on the Galaxy S20 Ultra (z3s)

Experimental iOS 16 / XNU port for the **Galaxy S20 Ultra 5G SM-G988B, Exynos 990
(universal9830)**. This is the z3s tree. The Snapdragon sources that the project started from
remain in the repository for the userspace glue and the driver layout, but they are not the
boot chain of this board.

The z3s boot chain is S-Boot. `sboot` verifies and loads an Android boot image v2 from the
`boot` partition; the port packages the XNU payload into that format. Hardware support is
incomplete: no iOS boot has been completed on this board yet. See
[the z3s hardware and boot notes](docs/Z3S.md).

## Current state

- `common/build.sh z3s` targets the kernel and userland. `common/devices/z3s.conf` sets
  `BOOT_CHAIN=none`, so the build skips a loader that does not exist yet.
- S-Boot leaves the framebuffer at `0xE9000000`, 1440 x 3200, stride 5760.
- Touch is the TM2 controller on HSI2C_15 (`0x108D0000`, address `0x48`, IRQ `gpa1-0`); the
  driver is not ported yet.
- Volume up, volume down and power are on `gpa0-3`, `gpa0-4` and `gpa2-4`; the key driver is
  not ported yet.

The Snapdragon reference port reached its home screen with display, touch, GPU and Wi-Fi on its
own hardware. Those results do not transfer to the Exynos board.

## Build and private inputs

Use `common/build.sh z3s` in the Linux cross-build environment described by the source scripts.
A macOS host can use an ARM64 Linux Docker container. The toolchain, iPhoneOS SDK and iOS
extraction must be supplied locally. Build the kernel from source when the prebuilt lacks the
z3s changes.

The source upload excludes Apple system images/SDKs, device-extracted firmware, private keys,
Wi-Fi credentials/MAC files, logs, partition backups and newly extracted UEFI binaries. The
phone has no fastboot: the boot image goes to the `boot` partition from TWRP. Do not use another
device's BOOT backup or partition geometry.

## Reference sources

The Snapdragon tree supplies the XNU userspace glue, the Wi-Fi stack and the driver layout this
port started from. Its notes stay in the repository for reference; its firmware packages are
not used by the Exynos boot chain.

## Licensing

Preserve per-file copyright notices. `LICENSE` is APSL 2.0 for the XNU portion;
`LICENSE-userland` retains the upstream MIT license for the userland portion. Other imported
source files retain their own notices.

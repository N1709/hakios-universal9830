# Galaxy S20 Ultra 5G (z3s, SM-G988B)

Exynos 990, 12 or 16 GB RAM, 1440x3200 60/120 Hz, 5G, ultrasonic fingerprint.
This profile is the first target of the unified Exynos 990 tree. It is a
bring-up profile: the kernel and loader build, but **nothing has been tested
on hardware yet**, and its partition table has not been measured.

## Status

| part | state |
|---|---|
| device tree facts | read from the stock `G988BXXUBDUI5` boot image |
| XNU kernel | builds from the shared Exynos 990 sources (GICv2) |
| xnuloader | builds; selects `platform_exynos990` by the `0xF1000000` display window (verify on z3s) |
| z3s UEFI | FD builds from `uefi-z3s/` (2 MiB, SHA-256 `e749b567…65fa7`); not run |
| boot image | not built: no recovery size, GPT not measured |
| iOS userland | not built: needs the private Apple inputs |
| installer | none: the GPT is not measured |

`TARGET_STATUS=unmaintained` is deliberate: the identity and board facts are
measured, but the partition map is not, and nothing has booted. The kernel is
still buildable through the bring-up path. The release tools skip the target
for installers because `installer_validated` is false.

## Identity

- models: `SM-G988B` (Europe/global, Exynos)
- reference firmware: `G988BXXUBDUI5` / `BL_G988BXXUBDUI5_CL22520152_QB44083655_REV01_user_low_ship_MULTI_CERT.tar.md5`
- RAM: 12 GB measured from the stock DTB; 16 GB variant not measured
- storage: 128/256/512 GB (capacity not measured on this unit)
- panel: `s6e3hab` (`hubble3` tuning), 1440x3200, 60/120 Hz
- touch: Samsung `sec_ts` `y79a_z3` firmware on HSI2C_15
- PMIC/gauge: MAX77705; haptics: CS40L25A; torch: S2MPB02
- Wi-Fi/BT: Broadcom BCM4375 (brcmfmac, no ath11k)
- modem: Exynos 5123, 5G

## Boot chain

S-Boot is Samsung's bare-metal bootloader. It has no usable `fastboot boot`,
so the phone starts an Android boot image v2 from a partition (the x1slte port
uses `recovery`). The image carries BootShim, the z3s UEFI and the XNU loader
payload:

```
sboot -> recovery (boot.img v2)
          embed@0:        BootShim + z3s UEFI (FD at 0xE8000000)
          payload:        xnuloader + XNU
       -> UEFI -> xnuloader -> XNU
```

The stock boot header of this phone:

| field | value |
|---|---|
| kernel load address | `0x10008000` |
| ramdisk load address | `0x11000000` |
| tags | `0x10000100` |
| DTB | `0x10000000` |
| page size | 2048 |
| header version | 2 |

The z3s UEFI package is a copy of the y2s S5E9830 package with the z3s DTB,
SMBIOS strings, BootManager hook and BootShim. It builds with
`uefi-z3s/build.sh`; the `build-z3s-uefi` GitHub Actions workflow produces the
FD. It has not been booted. See
[`devices/exynos990/z3s/uefi-z3s/README.md`](../../../devices/exynos990/z3s/uefi-z3s/README.md).

## Measured hardware facts

From the stock device tree and boot image:

| block | address |
|---|---|
| GICv2 distributor | `0x10101000` |
| GICv2 CPU interface | `0x10102000` |
| MCT timer | `0x10040000` |
| UFS HCI | `0x13100000` |
| DECON_F | `0x19050000` |
| PMU | `0x15860000` |
| USB DRD | `0x10E00000` |
| debug UART | `0x10540000` |
| touch I2C (HSI2C_15) | `0x108D0000`, address `0x48` |
| S-Boot splash framebuffer | `0xE9000000`, stride 5760, 32 bpp |
| UEFI display reservation | `0xF1000000`, size `0x1194000` |

Memory (12 GB):

| bank | base | size |
|---|---|---|
| 0 | `0x80000000` | `0x3AB00000` |
| 1 | `0xC1200000` | `0x1EE00000` |
| 2 | `0xE1900000` | `0x1E700000` |
| 3 | `0x880000000` | `0x27E800000` |

Keys: volume up `gpa0-3`, volume down `gpa0-4`, power `gpa2-4`. The y2s
KeypadDeviceLib uses the same pins, which is why the z3s package derives from it.

## Build

```sh
# shared kernel + loader (no Apple SDK: prebuilt kernel)
make kernel                 # z3s is the default kernel target
SKIP="images boot" common/build.sh z3s

# one kernel and one boot line per alias
make unified                # plan only
make unified-build          # macOS + theos; -> out/unified/

# full boot image, once the GPT is measured
UEFI_FD=/path/to/SILICIUM_UEFI.fd common/build.sh z3s
```

Without `UEFI_FD` set (and with `BOOT_CHAIN=none`), the boot-image step is
skipped. The kernel build is the same Exynos 990 kernel used by x1slte and y2s:
one kernel, one loader, per-device profiles. `out/unified/manifest.json` records
the status, board config and boot line of every alias.

## Before flashing

1. Dump the GPT (`sgdisk --backup`) and record `recovery`, `super`, `userdata`,
   `boot` and `dtbo` sizes. Fill `partitions` and `layout` in `device.json`,
   and `SUPER_TAIL_START` in `common/devices/z3s.conf`.
2. Build and boot the z3s UEFI on the phone with a serial log first.
3. Only then assemble an installer. Until step 1 is done, `SUPER_TAIL_START=0`
   makes the loader refuse every write inside `super`, and the log goes to
   `recovery+16 MiB`.

## Known gaps

- The UEFI FD builds, but there is no hardware test log and no display/touch test.
- Camera profile not written: the Ultra's sensors differ from the S20's.
- Fingerprint (ultrasonic) and 5G modem are not brought up.
- 16 GB RAM variant needs its own high-bank and EL2 reservation.
- No installer qualification: `installer_validated` stays false.

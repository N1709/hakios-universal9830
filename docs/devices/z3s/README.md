# Galaxy S20 Ultra 5G (z3s, SM-G988B)

Exynos 990, 12 or 16 GB RAM, 1440x3200 60/120 Hz, 5G, ultrasonic fingerprint.
This profile is the first target of the unified Exynos 990 tree. It is a
bring-up profile: the kernel and loader build, but **nothing has been tested
on hardware yet**. The partition sizes are measured from the stock PIT; the
phone's GPT has not been dumped.

## Status

| part | state |
|---|---|
| device tree facts | read from the stock `G988BXXUBDUI5` boot image |
| XNU kernel | builds from the shared Exynos 990 sources (GICv2) |
| xnuloader | builds; selects `platform_exynos990` by the `0xF1000000` display window (verify on z3s) |
| z3s UEFI | FD builds from `uefi-z3s/` (2 MiB, SHA-256 `e749b567…65fa7`); not run |
| boot image | builds structurally (6,789,120 bytes, placeholder identity); not flashed or booted |
| partition map | PIT `PICASSO3_EUR_OPEN.pit` from the `G988BXXUBDUI5` CSC; GPT not dumped |
| iOS userland | not built: needs the private Apple inputs |
| installer | none: PIT sizes are known, but the GPT is not dumped and nothing has booted |

`TARGET_STATUS=unmaintained` is deliberate: the identity, board facts and PIT
sizes are measured, but the GPT is not dumped and nothing has booted. The
kernel is still buildable through the bring-up path. The release tools skip
the target for installers because `installer_validated` is false.

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

## Partition map (PIT)

Source: `CSC_OMC_OXM_G988BOXMBDUI5_CL22520152_QB44083655_REV01_user_low_ship_MULTI_CERT.tar.md5`,
file `PICASSO3_EUR_OPEN.pit` (43 entries, SHA-256
`a1c7842cd03fcae5231588e602827e7e219dccdf2aeffaf8283c78b472d2b392`). The PIT
metadata names `SM-G988B` and `G988BXXUBDUI5`, so it matches the profile's
bootloader. Offsets and sizes are in bytes; PIT blocks are 4096 bytes.

| partition | offset | size |
|---|---:|---:|
| `dtbo` | `0x7180000` (119,013,376) | 8,388,608 |
| `boot` | `0x7990000` (127,467,520) | 61,865,984 |
| `recovery` | `0xb490000` (189,333,504) | 69,009,408 |
| `super` | `0x19400000` (423,624,704) | 9,437,184,000 |
| `up_param` | `0x3400000` (54,525,952) | 8,388,608 |
| `vbmeta` | `0x173e0000` (389,939,200) | 65,536 |
| `metadata` | `0x17400000` (390,070,272) | 33,554,432 |
| `userdata` | `0x2b7e00000` (11,674,845,184) | remainder |

`userdata` has no fixed PIT size: it fills the rest of the disk, so its size
depends on the 128/256/512 GB variant and is not recorded until the GPT is
dumped. The z3s image is 6,789,120 bytes, so it fits `recovery`
(69,009,408 bytes) with room to spare.

The PIT is Samsung's firmware map. It does not replace the phone's GPT dump:
the GPT confirms the live partition names, the disk total and the AVB footer.

## Boot chain

S-Boot is Samsung's bare-metal bootloader. It has no usable `fastboot boot`,
so the phone starts an Android boot image v2 from a partition (the x1slte port
uses `recovery`). The image carries BootShim, the z3s UEFI and the XNU loader
payload:

```
sboot -> recovery (boot.img v2)
          embed@0:        BootShim + z3s UEFI (FD at 0xE8000000)
          FD+0x100000:    loader.efi XNUP (UEFI hook)
          after the FD:   kernel + cmdline XNUP -> 0xF0000000 (BootShim)
       -> UEFI -> xnuloader -> XNU
```

`xnuloader/identity.h` is generated from the builder's own IPSW device tree
and is not committed. The current image uses `xnuloader/mkidentity.sh
--placeholder` (no Apple data), so it can test the boot chain but iOS user
space will not identify the phone as an iPhone. Rebuild with a real
`DeviceTree.<board>.im4p` before expecting an iOS boot; CI builds the same
placeholder and the `z3s-boot` artifact is structural only.

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

# full boot image (laptop or CI; never the phone)
PAYLOAD_BASE=0xF0000000 PAYLOAD_MAX_SIZE=0x00F00000 PANIC_LOG_BASE=0xF0F00000 \
    ./xnuloader/build.sh
MU=/path/to/Mu-Silicium ./devices/exynos990/z3s/build-boot.sh \
    /path/to/SILICIUM_UEFI.fd out/z3s-boot
```

Without `UEFI_FD` set (and with `BOOT_CHAIN=none`), the boot-image step is
skipped. The kernel build is the same Exynos 990 kernel used by x1slte and y2s:
one kernel, one loader, per-device profiles. `out/unified/manifest.json` records
the status, board config and boot line of every alias.

The image has two payloads: `loader.efi` in the FD tail at `0xE8100000`
(1 MiB slot), and the kernel + `cmdline` appended after the FD, which
`BootShim` copies to `0xF0000000` (15 MiB slot). `build-boot.sh` packs both,
writes `z3s-recovery.img` plus an Odin `z3s-recovery.tar.md5` containing
`recovery.img`, and records `build.json` and `SHA256SUMS`.
`tools/z3s-boot-verify.py` checks the shim constants, both payloads, the
loader's `PAYLOAD_BASE`/`PANIC_LOG_BASE` immediates and the kernel Mach-O
header without a phone. The packer is deterministic (`gzip` mtime 0 and a
fixed tar mtime). The UEFI FD and the PE header timestamp still differ between
build machines, so compare the payload hashes and the verifier result rather
than only the whole-image hash. The GitHub Actions `z3s-boot` artifact was
downloaded and re-verified this way.

## Before flashing

1. The PIT sizes are measured (see above), but the phone's GPT has not been
   dumped. Dump it with root or TWRP:
   `sgdisk --backup=gpt.bin /dev/block/sda` (or `dd` of LBA 0-33). This
   confirms the live partition names, the disk total and the AVB footer.
2. Build and boot the z3s UEFI on the phone with a serial log first.
3. Only then assemble an installer. `SUPER_TAIL_START=0` makes the loader
   refuse every write inside `super`, and the log goes to `recovery+16 MiB`.

The image is 6.8 MB and fits `recovery` (69,009,408 bytes), but the z3s chain
follows x1slte and starts from `recovery`; the Odin tar contains
`recovery.img`. Do not flash it until the GPT confirms the partition name and
size. A `boot.img` target is a different experiment, not this chain.

The payload window at `0xF0000000` overlaps the stock `cp_rmem`
(`0xF0000000` + 8 MiB, `exynos,modem_if`). The port does not start the modem,
matching x1slte, and the UEFI memory map labels the 16 MiB range "XNU
Download". Do not enable the modem or its IPC until the payload is relocated
and the CP regions are reserved; the x1slte cellular notes apply here.

## Known gaps

- The UEFI FD and the boot image build, but there is no flash or hardware boot
  log, and no display/touch test.
- Camera profile not written: the Ultra's sensors differ from the S20's.
- Fingerprint (ultrasonic) and 5G modem are not brought up.
- 16 GB RAM variant needs its own high-bank and EL2 reservation.
- No installer qualification: `installer_validated` stays false.

# z3s UEFI port (Galaxy S20 Ultra 5G, SM-G988B)

This is the board package for the Exynos 990 S20 Ultra. It is derived from the
in-tree `y2sPkg` (Galaxy S20+ 5G), which has the same SoC, the same 1440x3200
display class and the same key GPIOs. Nothing here has run on a z3s yet.

## What is here

| path | purpose |
|---|---|
| `Platforms/Samsung/z3sPkg/` | Mu-Silicium platform: DSC/FDF, memory map, keypad |
| `BootManagerLib/` | `XnuLoader.c`/`.h` and the BootManager hook that starts it |
| `BootShim.S` | the shim that copies the FD to `0xE8000000` and jumps to it |
| `Resources/Configs/z3s.toml` | BootShim / Android boot image v2 settings |
| `Resources/DTBs/z3s.dtb` | stock z3s device tree from the phone's own `boot.img` |
| `z3s.patch` | review diff of the shared BootShim/BootManager changes |
| `LICENSE.Mu-Silicium` | licence of the Mu-Silicium pieces |

The DTB is the FDT inside the Samsung DT table of the reference boot image
`G988BXXUBDUI5` (kernel `0x10008000`, ramdisk `0x11000000`, tags
`0x10000100`, DTB `0x10000000`, page size 2048, header v2). The table starts
with `d7b7ab1e`; its FDT begins at offset 64. The extracted FDT's SHA-256 is
`ffcad4d6011d108329f801abb9ca9ee48cffd6ccfc2704ea47d5e8f24507e22b`.

## Memory map (12 GB board, from the stock DTB)

| region | base | size |
|---|---|---|
| DDR bank 0 | `0x80000000` | `0x3AB00000` |
| DDR bank 1 | `0xC1200000` | `0x1EE00000` |
| DDR bank 2 | `0xE1900000` | `0x1E700000` |
| DDR bank 3 | `0x880000000` | `0x27E800000` |
| UEFI FD | `0xE8000000` | `0x200000` |
| display reserved | `0xF1000000` | `0x1194000` |
| EL2 early memory | `0xAFE800000` | `0x1800000` |

The 16 GB SM-G988B variant has not been measured; its high bank and EL2
reservation differ. Do not reuse this map for it without a dump.

## Building

The package is built inside a Mu-Silicium tree that carries `S5E9830Pkg` and
the Samsung silicon support, the same tree the y2s port uses:

```sh
MU=/path/to/Mu-Silicium
cp -a Platforms/Samsung/z3sPkg "$MU/Platforms/Samsung/"
cp -a BootManagerLib/. "$MU/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/"
cp -a BootShim.S "$MU/BootShim/BootShim.S"
cp -a Resources/. "$MU/Resources/"
cd "$MU"
python3 build_uefi.py -d z3s -r RELEASE
# -> Build/z3sPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

The same build runs in `.github/workflows/build-z3s-uefi.yml`. Download the
`z3s-uefi` artifact from that workflow instead of building on the phone.

The overlay already contains the applied BootManager/BootShim files, so
`z3s.patch` is only a review record of the shared changes; do not apply it on
top of the copied files. `common/build.sh z3s` consumes the FD through
`UEFI_FD`, or the release build finds it under `Mu-Silicium/Build/z3sPkg/`.
The XNU loader payload is the same `xnuloader/loader.efi` used by x1slte.

## Boot chain

S-Boot verifies an Android boot image v2 in the `recovery` partition, the
BootShim copies the UEFI FD to `0xE8000000` and jumps to it, the UEFI starts
`xnuloader`, and the loader hands over to XNU. This is the x1slte chain; z3s
still needs its own recovery partition size and AVB footer, measured from the
phone's GPT, before an image can be flashed.

## Payload slots

The FD artifact is 2 MiB and its `0x100000..0x200000` tail is erased. The
BootManager hook reads the `XNUP` payload at `0xE8100000` (FD + `0x100000`) and
starts `loader.efi` from it. The loader then reads the kernel `XNUP` from
`0xF0000000`, where `BootShim` copies the payload appended after the FD.

`devices/exynos990/z3s/build-boot.sh` builds both slots:

| slot | contents | limit |
|---|---|---|
| FD + `0x100000` | `loader.efi` only, for the UEFI hook | 1 MiB (`XnuLoader.h`) |
| after the FD | `kernel` + `cmdline`, copied to `0xF0000000` | 15 MiB |

The loader must be built for the second window:

```sh
PAYLOAD_BASE=0xF0000000 PAYLOAD_MAX_SIZE=0x00F00000 PANIC_LOG_BASE=0xF0F00000 \
    ./xnuloader/build.sh
```

A default loader (`PAYLOAD_BASE=0xE8100000`) would read the FD tail instead of
the kernel; `tools/z3s-boot-verify.py` rejects that build. The verifier also
checks the shim constants, the FD-tail `loader.efi` hash, the appended kernel
hash, the Mach-O magic and the `cmdline` entry. It is a structural check only.

## Status

The FD builds with `build.sh` and in the `build-z3s-uefi` GitHub Actions
workflow: 2 MiB, SHA-256
`e749b56787c003856ef33646416ac733b9760fc0d82f94455d86bd1aa7065fa7`.
REA's `inspect-uefi-firmware` maps it with complete coverage (2 volumes, 60 FFS
files) and the `[XNU]` hook is present in `BdsDxe`. The boot image now builds
with both payloads and passes the structural verifier, but it has not been
booted on a z3s, and the phone's partition map is not measured. Treat every
address above as a starting point to verify on hardware, not as a working
configuration.

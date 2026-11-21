# Galaxy S20 Ultra 5G (z3s, SM-G988B)

This is the Exynos 990 port. The existing port is the z3q (SM-G988N, Snapdragon 865); the two phones
share the 1440 x 3200 panel class and the S20 Ultra body, but almost nothing below the display.

The z3s boot chain is the open problem. There is no Mu-Silicium UEFI for Exynos, so the z3q's
`BootShim + SILICIUM_UEFI.fd` path cannot be reused. Until a z3s loader exists, `common/build.sh z3s`
builds the kernel and userland only (`BOOT_CHAIN=none` in `common/devices/z3s.conf`).

## What the bootloader does

The reference firmware is `BL_G988BXXUBDUI5_CL22520152_QB44083655_REV01_user_low_ship_MULTI_CERT.tar.md5`
from `azROM_net_BL_G988BXXUBDUI5_CL22520152_QB44083655_REV01_user_low.rar`. It contains `sboot.bin`,
`up_param.bin`, `ldfw.img`, `tzsw.img`, `tzar.img`, `harx.bin`, `ssp.img`, `keystorage.bin`, `uh.bin`
and `vbmeta.img`.

`sboot.bin` is Samsung S-Boot, a bare-metal AArch64 bootloader that reads the GPT, verifies the
Android boot image, draws a logo and starts the kernel. It is not UEFI and it has no usable
`fastboot boot` transport:

- `FASTBOOT MODE`, `do_fastboot`, `fastboot_init`, `getvar:` and `reboot-fastboot` strings are present,
  but there is no `boot:`, `flash:`, `erase:` or `download:` command string. It behaves like the
  fastbootd stub, not like a Qualcomm fastboot that can load an arbitrary image.
- The normal path is a Samsung boot image in the `boot` partition, written by TWRP or Odin.

The known-working Android 4.19 boot image for this phone has the standard Samsung layout:

| field | value |
|---|---|
| kernel load address | `0x10008000` |
| ramdisk load address | `0x11000000` |
| tags | `0x10000100` |
| DTB | `0x10000000` |
| page size | `2048` |
| header version | `2` |

The XNU image has to be packaged into this format, or loaded by a Linux kernel that S-Boot already
knows how to start.

## Hardware facts from the stock device tree

From the stock 4.19 device tree, extracted from the phone's known-working boot image:

| block | address |
|---|---|
| GICv2 distributor | `0x10101000` |
| GICv2 CPU interface | `0x10102000` |
| GICv2 optional (hyp/virt) | `0x10104000`, `0x10106000` |
| MCT timer | `0x10040000` |
| UFS | `0x13100000` |
| DECON_F | `0x19050000` |
| DECON_S / DECON_T | `0x19060000`, `0x19070000` |
| PMU | `0x15860000` |
| USB DRD | `0x10E00000` |

The GIC is `arm,cortex-a15-gic`, so XNU's GICv3-only interrupt driver needs a GICv2 counterpart.
PSCI is `arm,psci-1.0` with `method = "smc"`. The timer is 24 MHz (`0x18cba80`), with
`use-clocksource-only`; the MCT is `use-clockevent-only`.

The CPUs are numbered in MPIDR cluster 2 (`0x100`):

- cores 0-3: Cortex-A55 (`ananke`)
- cores 4-5: Cortex-A76 (`enyo`)
- cores 6-7: Cortex-A76 (`lion`)

The bootloader adds the `/memory` node at run time (`memory@%lx` in `sboot.bin`); the stock DTS has no
`memory` node. Bootargs include `corememsize=8G`, so the kernel is limited to the first 8 GB of a
12 GB board. The exact DRAM base still needs to be read back from a running phone; `0x80000000` is the
likely base from the DTS reservations.

## Framebuffer

`sboot.bin` builds the command line `s3cfb.bootloaderfb=0x%x` with `0xe9000000`, and adds the
`bootloader_fb` node with `compatible = "sboot,fb"` and a `reg` of base `0xe9000000`, size `0x1194000`.
That size is exactly 1440 x 3200 x 4 bytes. The older `0xe8000000` region (16 MB) is the bootloader's
logo/splash buffer, not the kernel framebuffer.

So the first HackiOS display target on z3s is the same simple model as the other ports: draw into the
bootloader framebuffer at `0xE9000000`, stride 5760, 32 bpp. Driving DECON/DSIM directly is a later
step.

## Loader options

1. **Linux-assisted (recommended first).** Keep the stock 4.19 kernel as the known-good boot
   environment, add a small initramfs, and kexec XNU with a hand-built `boot_args` and device tree.
   UFS, display, USB and PMIC are already initialized by Linux, which removes the largest pieces of
   bare-metal work. This needs `CONFIG_KEXEC=y`; `arch/arm64/configs/exynos9830_defconfig` currently
   has `# CONFIG_KEXEC is not set`.
2. **Bare-metal loader in the boot partition.** Replace the Android kernel with a loader that brings
   up UFS, the display and USB itself, then starts XNU. This is closer to BootShim, but it is a large
   driver project on Exynos and should wait until the Linux-assisted path has proved the kernel.

Either way, the XNU side needs the platform name `z3s`, the GICv2 driver, and a way to describe the
Exynos peripherals in the device tree the kernel receives.

## Kernel work

- **Interrupts:** add a GICv2 driver next to `osfmk/arm64/gic_v3_irq.c` and select it for z3s.
- **Storage:** the first boot can use a ramdisk. A root on UFS needs an Exynos UFS driver; the z3q's
  `redwood_ufs.c` is Qualcomm-specific.
- **Display:** start with the `0xE9000000` framebuffer; DECON/DSIM later.
- **Touch:** the S20 Ultra uses a Samsung touch controller, not the z3q's `sec_ts` path; a new driver
  is needed before `touchscreen=1` means anything.
- **Buttons, PMIC, cpufreq, watchdog:** Exynos PMIC/GPIO numbers and PSCI power states.
- **Platform gating:** `redwood_platform_is()` currently knows `y2q` and `z3q`; add `z3s` where the
  hardware is shared and keep the Exynos-specific paths separate.

## Current status

`common/devices/z3s.conf` exists with `BOOT_CHAIN=none`; `common/build.sh z3s` therefore builds the
kernel and userland but skips the loader and the boot image. No z3s loader has been written yet.
The v4.20 kernel test image (`boot-v420-af2fe698b.img`) is staged on the phone and still awaits a
manual flash.

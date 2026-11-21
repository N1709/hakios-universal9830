# Boot screen: one Apple logo from power-on to iOS (A, 2026-11-15)

What the user sees: Samsung's splash, the UEFI, our loader and the kernel's first moments all show iOS's own boot
logo (ProgressUI `apple-logo@3x~iphone.png`, white on black), centered, 246x300 on the 1440x3200 panel (9.4 % of the
height; an iPhone X's is 9.7 %). No loader text, no menu, no countdown. Samsung's "bootloader unlocked" warning stays.

| Screen | Where the logo comes from | Made by |
| --- | --- | --- |
| Samsung's splash | `up_param` partition, `logo.jpg` (1440x3120, baseline 4:4:4 JPEG like Samsung's own) | `out/up_param-apple.img` |
| UEFI (was "Project Silicium") | the BootLogo BMP inside the UEFI's LZMA firmware volume, swapped in place (same 300x300 8-bit BMP, every offset and size unchanged), always remade from the untouched `build/uefi/Mu-x1slte.img`; its `[Volume Up] Boot Manager` line under the logo reads `- JESVI -` (both modes; `mkbootlogo --dev` keeps Mu's line) | patched in `out/x1slte-embed.img` |
| Loader | `xnuloader/applelogo.h` (RLE), drawn by `boot_logo()` | loader.c |
| Kernel until iOS's first frame | the loader's picture, scaled 1440x3200 -> 1080x2400 in place when fb_mode switches the panel (175 ms, frames paused) | MiniIOMobileFramebuffer.cpp `bootPictureScale` |

`tools/bootlogo/mkbootlogo.py` makes all of it from the iOS root (`sysimage/rootfs`) at build time; `./x1slte.sh payload`
runs it. The artwork is Apple's: every output is git-ignored, nothing of it is committed. Without an iOS root the
loader's screen is black and the UEFI/splash keep their old pictures.

## Switches

- **Dev mode** (`DEVMODE=1`): the loader's text and the JesviOS menu with its 3 s countdown at every boot, for
  development. Put `DEVMODE=1` in `common/devices/x1slte.local.conf` (this Mac only, git-ignored) or the environment
  of `./x1slte.sh payload`; it becomes a `devmode` entry in the loader's payload table (not the boot line, which is
  full). Without it (the default) end users get iOS's logo and no menu. Release builds (`tools/release`, which sets
  `JESVI_TARGET`) never get dev mode, whatever the local config says. `bootui=0` in the cmdline does the same.
  In the local conf write `DEVMODE=${DEVMODE:-1}` so `DEVMODE=0 ./x1slte.sh payload ...` still builds an end-user image.
- The menu: title `JesviOS`, subtitle `Galaxy S20  -  iOS 16.5` (loader.c `menu_draw`); every on-screen text is in loader.c
  (`menu_draw`, `menu_items`, `menu_status` calls).
- A volume key pressed while the loader's logo shows (0.7 s): the menu (Boot iOS, Log access, Download Mode, Reboot,
  Power off). Log access asked by a tool (`rwlogd /menu?to=1`, x1slte-auto.sh) works as before.
- `fb_bootpic=0`: no scaling of the loader's picture at the mode switch (the logo then jumps a third bigger).
- A split-second tear at the panel's switch to 120 Hz remains; the user likes it and keeps it.

## Installing

- Loader, UEFI, kernel: the usual `tools/x1slte-auto.sh --recovery` (recovery and boot partitions).
- Samsung's splash: `UP_PARAM=out/up_param-apple.img tools/x1slte-auto.sh --recovery`, or from Log access
  `fastboot stage out/up_param-apple.img; fastboot oem write up_param 0`. The loader accepts up_param only as a tar
  from offset 0 (loaders before 2026-11-15 refuse it). Download Mode works too: `./x1slte.sh hlogo`.
- Back to Samsung's splash: write the stock `up_param` (device/backup/20261105-203534/up_param.img) the same way.

## Traps met

- macOS Download Mode / Log access over a hub or a fresh cable can need "Allow accessory to connect" on the Mac.
- A Download Mode flash of the whole recovery partition overwrites the last run's log (16..32 MB of recovery): read
  it first (`./x1slte.sh ulog`) when the phone can still reach Log access.

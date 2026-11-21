# Third-party licenses

Every third-party component HakiOS redistributes or fetches at build time, with its origin,
license, and status. Where "origin uncertain" or "to verify" is written, the item is **not**
asserted to be cleared for redistribution; treat it as pending review (see `COMPLIANCE.md`).

This supersedes the summary in `NOTICE`, which stays as the short, canonical list.

## Redistributed in this repository

| Component | Path | Origin | License | Notes |
|---|---|---|---|---|
| Samsung Exynos9830 EVT1 PCIe PHY sequence | `tools/cellular/s5123-phy-evt1.h`, combined `live-pcie` helper | [ExtremeXT kernel, pinned69515fbb](https://github.com/ExtremeXT/android_kernel_samsung_exynos990/blob/69515fbb7a4395898c05a8624f76a12afbac11c5/drivers/pci/controller/dwc/pcie-exynos9830-rc-cal.c) | GPL-2.0-only | Samsung Electronics2019; ordered124-operation CP-off diagnostic sequence, see `tools/cellular/LICENSE.GPL-2.0`; no vendor firmware included |
| Files / HFS+ compatibility | `userland/src/inject_files.c` | [Angelpro09xd/ios-sapphire](https://github.com/Angelpro09xd/ios-sapphire/commit/04f94d5c6577d65bf9e112ed801c8b1fce9fff47), `userland/tweaks/fileprovider/fpfix.c` | **to verify** | author signed the upstream commit; no file-specific license notice; nested `userland/hackios/LICENSE` does not establish coverage of this sibling directory |
| XNU modifications | `xnu/hakios.patch`, `xnu/files/` | derivative of Apple XNU | APSL 2.0 (`LICENSE.APSL`) | base tag `xnu-8796.121.2`, fetched by `xnu/apply.sh` |
| libdeflate | `xnu/files/bsd/miscfs/apfs/libdeflate/` | Eric Biggers | MIT | see its `COPYING` |
| libzbitmap | `xnu/files/bsd/miscfs/apfs/libzbitmap/`, `xnu-deps/build/lzbitmap/` | Corellium LLC (port of eafer/libzbitmap) | GPL-2.0+ OR MIT | used under MIT |
| LinuxKPI / Linux headers | `wifi/lx/include/linux/`, `wifi/lx/include/net/` | Linux / FreeBSD LinuxKPI | GPL-2.0 (Linux) / BSD-2-Clause (FreeBSD) | small compatibility headers; see each file's notice |
| Linux DRM shim | `tools/mesa/hakios-drm/`, `xf86drm.h` | Linux / X.Org | MIT | build-time shim for Mesa |
| AIR / Metal-library tooling | `tools/mesa/air/` | HakiOS (Darwin on redwood contributors) | MIT | our own; LLVM bitcode layout is a published format |
| FocalTech touch firmware header | `xnu/files/bsd/dev/hakios.focaltech_fw.h` | FocalTech (device firmware blob) | vendor's — **to verify** | candidate to move to pull-from-device |
| Apple host headers (build only) | `xnu-deps/hostinc/mach-o/`, `.../mach/` | Apple (open-source cctools/xnu headers) | APSL 2.0 | host-side Mach-O headers for the cross build |
| Pre-built kernel & user space | `prebuilt/` | built from this repo | APSL 2.0 / MIT (as source) | reproducible from source here |

## Fetched at build time, never committed

| Component | Fetched by | Origin | License |
|---|---|---|---|
| iOS 16.5 IPSW (user space of the system image) | `sysimage/fetch.sh` | Apple | Apple's — user supplies, user's own copy |
| iPhoneOS 16.5 SDK | `xnu-deps/setup.sh` (github.com/theos/sdks) | Apple (headers) via theos | Apple's / theos |
| Apple open sources (AvailabilityVersions, libdispatch, libpthread, cctools, bootstrap_cmds) | `xnu-deps/setup.sh` | Apple | APSL 2.0 |
| MacOSX13.3 SDK (Kernel.framework DriverKit headers only) | `xnu-deps/setup.sh` | Apple via joseluisq/macosx-sdks | Apple's |
| lzfse | `xnuloader/mkidentity.sh` (when no decoder present) | Apple, github.com/lzfse/lzfse | BSD-3-Clause |
| ath11k (Wi-Fi driver) | `tools/mk-ath11k.sh` | Linux | BSD-3-Clause-Clear |
| Mesa (GL=1 images) | `tools/mk-mesa.sh` | Mesa project | MIT and others |
| Procursus / Sileo packages | `tools/mk-jb.sh` | Procursus team | their own licenses |

## How to rebuild the inventory

- Tracked non-text/binary files: `git ls-files -z | xargs -0 file --mime-type | grep -v text/`
- License mentions in tracked files: `git grep -li "SPDX-License-Identifier\|GPL\|BSD\|MIT\|APSL\|copyright"`

If a component's origin is uncertain, it is marked **to verify** above and must be treated as not
yet cleared for redistribution until confirmed with its owner.

S20 live modem link helper: tools/cellular/s5123-live-link.h adapts Samsung
Electronics (2019) pcie-exynos-rc.c IA/link sequence and DesignWare root setup
from the same pinned Linux source as s5123-phy-evt1.h, GPL-2.0-only. See
tools/cellular/LICENSE.GPL-2.0. Combined live-modem binary is GPL-2.0-only;
original standalone SPI/lifecycle portions retain MIT source license.

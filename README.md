# HakiOS Universal 9830

iOS 16.5 for Samsung Exynos 990 phones.

This tree is based on [Jesvi-iOS](https://github.com/jesvi-eulah/Jesvi-iOS),
which has the working Galaxy S20 (`x1slte`) boot chain. On top of it, the
Exynos 990 boards share one kernel and one loader, with a small per-board
profile.

`z3s` (Galaxy S20 Ultra 5G, SM-G988B) is the first new target.

## Status

| board | model | state |
|---|---|---|
| `x1slte` | Galaxy S20 LTE, SM-G980F | boots; the only hardware-validated target |
| `z3s` | Galaxy S20 Ultra 5G, SM-G988B | bring-up; kernel/loader sources and UEFI package in tree, **not booted**, GPT not measured |
| `y2s` | Galaxy S20+ 5G | experimental kernel only; not booted |
| `x1s` | Galaxy S20 5G | experimental kernel only; not booted |
| `c1s` | Galaxy Note20 | experimental kernel only; not booted |
| `c2s` | Galaxy Note20 Ultra | experimental kernel only; not booted |
| `r8s` | Galaxy S20 FE | experimental kernel only; not booted |

Nothing here is a promise that a phone will boot. Only `x1slte` has boot
evidence. `z3s` has no boot log, no measured partition map and no installer.

## Quick start

```sh
make check          # source checks and tests
make devices        # list boards and their state
make kernel         # shared Exynos 990 kernel, z3s by default
make unified        # plan one kernel + boot line for every alias
make unified-build  # build that kernel once (macOS + theos)
make release        # x1slte install ZIPs -> dist/release/
```

`make kernel DEVICE=y2s` builds another board. `make release` and `make doctor`
use `RELEASE_DEVICE=x1slte`, the only installer-qualified board.

A full boot image needs macOS, the iPhoneOS 16.5 SDK, an iOS 16.5 IPSW and the
phone's private firmware. A z3s boot image also needs a built z3s UEFI FD; see
[`devices/exynos990/z3s/uefi-z3s/`](devices/exynos990/z3s/uefi-z3s/).

## Unified Exynos 990

The kernel and `xnuloader` are shared by every alias. A board only adds:

- `common/devices/<board>.conf` — boot chain, memory, panel, storage
- `devices/exynos990/<board>/target.conf` — identity and boot args
- `devices/exynos990/<board>/device.json` — profile and evidence

`make unified` prints the plan. `make unified-build` writes
`out/unified/mach.exynos990`, one `bootargs.txt` per board and a
`manifest.json`. It never builds an installer for an unmeasured board.

## z3s notes

- Reference firmware: `G988BXXUBDUI5`
- Panel: `s6e3hab_hubble3`, 1440x3200, 60/120 Hz
- Touch: `sec_ts` `y79a_z3` on HSI2C_15
- SoC: Exynos 990, GICv2, 12 GB measured (16 GB variant not measured)
- UEFI: `z3sPkg`, derived from the S5E9830 `y2sPkg`

Read [`docs/devices/z3s/README.md`](docs/devices/z3s/README.md) before
flashing anything.

## Credits

- [Jesvi-iOS](https://github.com/jesvi-eulah/Jesvi-iOS) — base tree and the
  working `x1slte` port
- [Mu-Silicium](https://github.com/Mu-Silicium/Mu-Silicium) — UEFI platform
- Apple — XNU and the iOS user space
- Everyone who measured, tested and documented a board

## License

The kernel changes are under the Apple Public Source License 2.0. The rest of
the project is MIT. See [`LICENSE`](LICENSE), [`LICENSE.APSL`](LICENSE.APSL)
and [`NOTICE`](NOTICE).

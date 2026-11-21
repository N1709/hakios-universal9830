# S20 cellular tools

Branch `x1slte-cellular`; status and resumable checklist:
[docs/devices/x1slte/cellular/s20.md](../../docs/devices/x1slte/cellular/s20.md).

Latest Nov12 15:08: live BOOT + PCIe L0 + actual Type0 modem endpoint PASS,
verified CPoff cleanup. Combined reserved loader/SPI8 kernel installed. LTE/voice
pending; see [download backend plan](../../docs/devices/x1slte/cellular/download-plan.md).
Older pending-install notes below are historical. SIM OUT.

Firmware/memory checks run offline. The separate UI installer contacts the
phone only when its caller already owns the normal phone lock; it never flashes
or reboots. See [Cellular UI](../../docs/devices/x1slte/cellular/ui.md) for installation status.

From this worktree:

```sh
bash tools/cellular/check.sh
python3 -B tools/cellular/shannon.py /path/to/own/radio.img --verify-crc
python3 -B tools/cellular/memory-audit.py --boot-log /path/to/saved/xnulog.txt
```

`shannon.py` prints metadata only. It never extracts firmware or reads NV from
EFS; the NV TOC entry is an external object, even though its offset is zero.
The SIPC5 parser is importable for future IPC/capture validation. It does not
implement SIM queries, packet sessions, Samsung IPC commands or voice calls.

`memory-audit.py` uses the branch's loader/audio sources plus saved-log XNU,
display and Wi-Fi allocations. Use `--root OTHER_WORKTREE` to inspect another
source tree. Dynamic staging also requires its logged address, and deployed
firmware reservations require `cellular memory: reserved 3/3`. It reports source
candidates even if audio is disabled at runtime, using only the latest boot.
Exit codes: 0 = no identified conflict/unknown; 1 = conflict; 2 = incomplete
evidence or input error. This is a partial audit, not proof of safe modem DMA.

Validated 2026-11-11:

- Local S20 RADIO: BOOT/MAIN/APM/VSS/INFO CRCs all pass. NV bytes never read.
- Existing saved boot: IPC overlaps loader staging by 8 MiB; packet region
  overlaps display pool by 64 KiB. Audit exits 1 as intended.
- Host tests: **42 Python tests passed** plus **163,840** C allocator boundary
  comparisons under ASan/UBSan (`bash tools/cellular/check.sh`). Synthetic
  fixtures contain no vendor bytes.
- Full ARM64 loader boots on the original UEFI. Opt-in `CELLULAR_RESERVE_CP=1`
  reserves CP memory through existing boot services and applies Normal NC through
  DXE. Hardware ownership/live translation checks pass 3/3 before and after iOS;
  saved-log audit exits 0. The separately rebuilt native FD failed and must not
  be deployed as working; it is unnecessary for this tested reservation path.
- Cellular UI compiles with warnings as errors, is ad hoc signed, and passes
  strict signature verification. Installed after the normal expired-lock takeover;
  corrected page is visible and user-confirmed. No image flash or reboot.

Bounded loader tests were installed and removed afterward; the phone currently
had its original BOOT/recovery copies restored after each test. Other agents have
since updated the phone; refresh the baseline before the next deployment.
Internal SPI loopback and reset/power LOW-only GPIO writes are proven. Actual CP power/BOOT transmission has since passed:64496 bytes, safe shutdown
and exact BOOT restore. No modem DMA, NV access, LTE or call proof yet.
UI remains installed. Keep generated JSON/private logs outside the source tree.

Additional tools:

- `prepare-m0.py`: offline verified image packaging; `--keep-installed-fd`
  preserves working firmware. Preflight requires `--with-installed-fallback`.
- `preflight-report.py`: bounded RAM report reader; phone lock required.
- `s5123-gpio.py --read`: six read-only CMGP GPIO banks; phone lock required.
- `s5123-clocks.py --read`: 13 read-only clock/USI/GPIO/PHY-isolation words; lock
  required. Default is plan only. No SPI/PCIe controller access.
- Memory audit requires both `reserved 3/3` and `normal-nc 3/3` from the latest
  boot. Boot-services memory descriptors alone did not prove active cache policy.

### Guarded CP BOOT probe (phone SPI transfer and shutdown passed)

`s5123-boot.h` sequences CRC validation, ownership checks, power/reset, bounded
SPI and shutdown. The UEFI adapter requires all memory/MMIO/loopback/GPIO
preflight flags plus `CELLULAR_CP_BOOT_PREFLIGHT=1`. Normal builds omit it.
Packaging requires explicit `--cp-boot-radio PATH` with original firmware and
installed-loader fallback; only CRC-checked BOOT is embedded, never NV/EFS.

Shutdown must verify reset/power LOW, settle delay, controller/pad/clock restore
and PHY isolation before releasing CP RAM. Otherwise the loader retains RAM
and exposes cleanup-only `oem cp-stop`. It never retries power-on/transmission.
Critical reset/power pins stay owned output-low after an attempted power-up.
A subsequent probe must account for that ownership rather than blindly reuse it.

Validation: 23 ownership/cache cases, 163840 allocator comparisons, eight PIO
streams/six PIO failures, and 22 injected boot-lifecycle failures under sanitizers.
Even a fully transmitted BOOT is not proof of CP acceptance, SIM or LTE.

`boot-audit.py --manifest candidate/manifest.json --report probe-report.txt`
checks one complete powered probe report offline. It requires memory/MMIO/GPIO
evidence, exact expected byte/packet counts, lifecycle completion and verified
shutdown/fallback. Missing, mixed, short or unsafe reports fail. A pass means
controller transfer and shutdown only; CP acceptance, SIM, LTE/VoLTE and
independent BOOT restoration remain explicitly unproven.

The compiled powered-probe image was packaged end-to-end offline against the
historical backup: AVB verified, firmware/fallback preserved, BOOT64496 bytes
CRC checked, footer unchanged. This validates packaging, not hardware. The
private directory is explicitly named `cp-boot-OFFLINE-OLD-BASELINE`; do not
deploy it against the newer installed camera/Files/USB kernel. Fresh baseline
required. The manifest now records standalone BOOT size and both candidate
and original-prefix SHA256 hashes for deployment/readback checks.

### Current backup and PCIe prerequisites

`capture-baseline.py --output out/cellular/NEW-DIRECTORY` requires the cellular
phone lock and reads current recovery twice, compares BOOT to its prefix, checks
kernel/BOOT stability, and verifies AVB. It writes baseline.json last. Use a
visible Terminal job; output is private/ignored. Revalidate before deployment.
Historical m0 and earlier-current backups must not overwrite newer agent work.

`s5123-pcie-baseline.py` defaults to a plan. `--read` takes21 individually
whitelisted CMU/GPIO/PMU snapshots; isolated controller addresses are refused.
Phone readings: PCIe gates enabled, PHY isolated, GPF3 pins input.

`CELLULAR_PCIE_MAP_PREFLIGHT=1` requires memory/MMIO/fallback preflights and
excludes powered CP BOOT. Nine UEFI PCIe regions passed actual live Device
identity checks; iOS returned, BOOT restored exact, final iOS64s. No controller
access in that test. `CELLULAR_PCIE_ACCESS_PREFLIGHT=1` additionally prepares
a separate root-port identity read using a saved/restored PHY input clock,
with CP off and no link/DMA request. See session/docs for hardware result.

**PCIe root-access probe failed on hardware.** `CELLULAR_PCIE_ACCESS_PREFLIGHT`
is now disabled in build.sh. Do not deploy its saved artifact. Mapping-only
preflight passed; exact failing MMIO in the access attempt is unknown. See
cellular-s20.md for recovery evidence and prerequisites to revisit.

**Battery correction:** user reports battery exhaustion, so the previous PCIe
access timeout is inconclusive. An explicit retest flag builds the revised
isolation-aware probe with persistent pre-access checkpoints. battery-gate.py
--require-ready enforces10%/3700mV and active external charging. See latest
cellular-s20.md entry/session for dispatcher and result; do not use old artifacts.

`phy-plan.py --source PINNED-pcie-exynos9830-rc-cal.c --output NEW.json`
extracts the110 ordered EVT1 PHY writes for offline review. Unknown statements,
unbounded expressions and out-of-window/unaligned offsets are refused. This is
only the calibration subsection, not an executable initializer: common reset/
PCS/controller stages remain separate. Repeated writes must not be deduplicated.
Source attribution/license are retained in the output.

### Live debugging (preferred; notify user before any restart)

```sh
bash tools/cellular/build-live.sh
python3 tools/cellular/live-run.py --status
# Separate, mutating CP-off root-port diagnostic after status/ownership review:
python3 tools/cellular/live-run.py --root-id
```

Both device commands require the cellular phone lock. The wrapper uploads one
signed helper, saves per-phase output under ignored logs/cellular, and removes
it after confirmed completion. It has no reboot or flash path. --root-id checks
charge, uses only four fixed write addresses, and restores saved settings.
Signals request cleanup; a kernel MMIO stall can still prevent cleanup. Never
retry a timeout without inspecting transcript and current phone state.

Current checkpoint: builds/signature valid,41 Python tests and sanitizer models
pass, including58 live-helper fault injections. Hardware execution pending.
See cellular-flow.md for the updated batched-restart strategy.

Nov12 live ROOT probe now passed: host identitya5a5144d and verified cleanup,
no restart. `live-run.py --phy-test` is the new CP-off PHY calibration/restore
diagnostic.124 ordered operations,105 snapshots;726 PHY failure injections and
58 ROOT cases pass on host. Combined helper includes Samsung GPL-2.0-only
sequence, attribution/license supplied. PHY hardware execution pending phone
slot; root access is not endpoint/link proof. User removed SIM for now.

### Live SPI and combined base (pending installation)

The optional prepare-m0.py --kernel changes only the fixed16MiB AP-kernel segment
in addition to loader/head; it preserves the current root image and payload args.
It requires ARM64 Mach-O, the known partition layout and original firmware.
Explicit per-segment rollback files/hashes are produced.

SPI8 kernel hooks spi8.tx/spi8.rx are small64-byte bounded FIFO operations. The
normal loader with CELLULAR_RESERVE_CP plus this kernel is the combined live
base; candidate verified, actual install has NOT run (phone handed to A).

After that base is installed, no reboot is needed for:

```sh
bash tools/cellular/build-live.sh
python3 tools/cellular/live-spi-run.py --bytes 16
# Only after small loopback passes:
python3 tools/cellular/live-spi-run.py --bytes 64496
```

Generated internal loopback only, CP off/pads input. Exact byte-stream host
cases and142 I/O faults pass under sanitizers. Physical loopback awaits device
time and installed SPI8 API. Wrapper refuses missing hooks before upload.

### Nov12 14:52: installed/live milestone supersedes pending notes above

Combined reserved-memory loader and SPI8 kernel installed/verified. CP-off ROOT,
PHY124-operation restore, internal SPI16 loopback16/64496B and real CP power/
SPI BOOT64496B all passed LIVE. Real BOOT691328us, input first2/last6, cleanup1.
Phone never restarted for live tests. Modem acceptance/PCIe endpoint pending.

```sh
# Explicit private certificate/firmware inputs; requires phone lock + charge:
python3 tools/cellular/live-modem-run.py \
  --base out/cellular/live-batch-island-20261112 \
  --radio /path/to/local/radio.img
```

Do not repeat merely because it is available. One bounded power/BOOT attempt,
four-second input observation, verified shutdown; no MAIN/NV/DMA or reboot.
Wrapper compares running BOOT/kernel segment/loader evidence to completed
installed certificate before upload. GPIO input DAT reads actual pad, so output
sequence preloads latch without pad-level check, selects output, then verifies
level. Host model includes pulled-high input behavior and1486 I/O failures.
SPI adapter allows external use only through the guarded modem lifecycle;
standalone live-spi remains generated INTERNAL loopback.

### Live PCIe endpoint confirmation — Nov12 15:08

--link now passed on hardware: L0, root Type1 with BMEoff, separate Type0
endpointa5a5144d, EP BME cleared only, cleanup1. CP powered off afterward.
One bounded training poll; no blind native10 retries. No DMA/MAIN/IRQ enable.
Do not rerun it merely to reproduce the milestone; next is the queue backend.

s5123-live-link.h preserves Samsung PHY/IA/link ordering and active pinctrl,
saves before writes, parks LTSSM/PERST/IA before CPoff, then restores reverse
snapshots. Root BMEoff/Type1 guards, actual EP Type0 proof; sole EP write is
COMMAND low16 with BMEclear and statushigh0. No arbitrary config/endpoint writes.
Combined helper GPL2; see NOTICE/license. Host combined model2612 fault cases
plus clear-only/alias/root-BME/stuckBME/timeout scenarios pass under sanitizers.

### External SIPC UDL (host only)

`s5123-udl.h` provides bounded channel215 packet encoding and strict ACK checks.
`python3 tools/cellular/udl_plan.py /private/path/radio.img` verifies CRCs and
prints the reviewed S20 stage order. Output contains metadata only; no EFS/NV
reads, device commands, or claim of modem acceptance. NV and final completion
remain blocked until their prerequisites are resolved. See the download plan
for pinned Samsung source and host tests.

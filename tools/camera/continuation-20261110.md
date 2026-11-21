# S20 camera continuation — 2026-11-10

The exported conversation `logs/m2m/ios_camera.md` ended after booting the
07:38:08 kernel with the front-camera voltage correction. The voltage retest
had not run. This continuation performed that test and the comparisons below.

Current status and repeat-test decisions: [evidence checklist](CHECKLIST.md).
The dated sections below are chronological; later results supersede earlier ones.

Physical capture under XNU is still unproven. Camera.app continues to use synthetic
frames; a passing receiver pattern is not an optical-camera success.

## Measured results

Evidence is in `logs/camera/readback-20261110/` unless otherwise stated.

| Test | Result |
| --- | --- |
| Voltage correction, physical sensor + DMA | Zero CSI frames and zero changed DMA bytes; `../voltage-retest-20261110.json` |
| Repeated mode-register validation | 25 stable registers read three times: 75 reads, no mismatch or I2C error |
| Complete global microcode validation | All 200 burst words at indirect page 0x2000, addresses 0x90c8–0x9256, match the imported table |
| Receiver hardware pattern | 320×240, 153600 changed bytes, frame-end observed, no DMA error or guard corruption, buffer released |
| PMIC restoration | Before/after snapshots match byte for byte |
| Samsung reference, I2C tracing enabled | 446 events captured, clean VC0/VC1 frame start/end, helper exit 0, saved HackiOS BOOT restored successfully |
| Samsung timing replay, probe mode 5 | Frame length 0xb18d, shift 10, internal counter reaches 1; zero CSI packets |
| Explicit normal timing, probe mode 6 | Frame length 0x0597, frame/exposure shifts 0, internal counter reaches 16 during 500 ms; zero CSI packets |

`reference-main.raw` preserves the protected Samsung log. Its decoded text is
`reference-main.txt`. `reference-flash.log`, `reference-restore.log` and
`reference-manifest.json` identify the reference image and saved restore image.
The reference kernel still does not expose the CAL clock-debug interface.
Prior reference logs show `/dev/mem` unavailable with errno 6 even when its
device node exists. Do not repeat that failed clock-read method unchanged.

## What changed

- Normal streaming explicitly clears Samsung's frame-length and exposure
  shifts (`0x0702`, `0x0704`). This recovered internal sensor progress in the
  measured state. It has not recovered CSI reception.
- Corrected the intermediate CSI lane-enable write: v5.3 `ENABLE_DAT` is bits
  1–4, while `ENABLE_CLK` is bit 0. The sequence is now 0x1e then 0x1f.
- Matched Samsung's PHY skew write order: its loop updates the preceding
  lane, including the already enabled clock lane on iteration zero. The
  previous implementation had the same final values but different ordering.
- Camera MMIO helpers use the barriers corresponding to Linux's ARM64
  `readl`/`writel` (`dsb ld` after reads, `dsb st` before writes).
- Streaming verifies the complete microcode burst and 75 direct-register
  reads before enabling capture. Errors identify the first address, expected
  value and observed value, then enter cleanup.
- Expanded the debug-command registry from 48 to 96 entries and names from
  24 to 32 bytes. `cam.pmic` had silently failed to register; the long DPHY
  configuration command was truncated. `cam.pmic` is now verified on-device.
- Fixed the idle DPHY snapshot's eight reads into a seven-element array.
- Removed LDO12 enable writes from the legacy front ID/mode probes. The
  phone's DTBO identifies LDO12 as WIDESUB analog, not a front-camera supply;
  those probes also did not restore that enable bit.
- `x1slte.sh kernel` invokes XNU's `build` target. The default `all` target
  additionally exports SDK symbolsets and was failing after linking the
  kernel. The kernel-only target succeeds through the normal checked wrapper.

## Comparing the captured I2C transactions

From the kernel repository root:

```sh
python3 tools/camera/compare-3j1-trace.py logs/camera/readback-20261110/reference-main.raw
```

All 353 global/PDAF-mode transactions match the captured transaction lengths
and visible bytes. Linux truncates the 402-byte burst payload to 64 displayed
bytes; the full 200 words are independently verified on the live iOS sensor.
The 15 additional pre-stream transactions include embedded-data disable,
frame duration, shift, gains, MIPI PLL selection and dual-sync configuration.
See `trace-comparison.json` for exact values. The comparison tool was checked
with this real capture, an altered register write, and a missing stream-on.
It rejects the two damaged inputs.

## Resume commands and remaining boundary

Run from the kernel repository root, after checking that iOS SSH is reachable:

```sh
Y2S_IP=10.77.0.2 ../../tools/poke cam.pmic
Y2S_IP=10.77.0.2 ../../tools/poke 'cam.s20.dma.test 1 6'
Y2S_IP=10.77.0.2 ../../tools/poke 'cam.s20.dma.test 0'
Y2S_IP=10.77.0.2 ../../tools/poke cam.pmic
```

Mode 5 reproduces the captured Samsung Vision timing; mode 6 uses the PDAF
table and stock gains with explicit normal exposure timing. Normal probes
also clear exposure shifts. The bounded optical DMA window is already
5000 × 100 µs (500 ms); an early commentary in this continuation incorrectly
suspected 50 ms and was corrected after reading the current source.

Only download a frame after the matching result says `exported: true`:
`ssh s20 /var/root/s20-frame > FRAME.raw`, then decode with
`python3 tools/camraw.py FRAME.raw PREFIX`. The receiver pattern produces a
synthetic chart. An optical result must additionally have real CSI progress,
clean DMA completion, plausible pixels, and intact guards before any iOS
application integration.

The remaining boundary is the physical sensor-to-receiver path: a running
sensor and verified setup still produce no CSIS packet interrupts and
`PHY_STATUS` stays 0xf1. Do not describe matching registers as proof that the
PHY is receiving. An active Samsung-vs-iOS clock/PHY comparison is still
missing. Preserve the known unsafe-register exclusions in README.md.

## Final verification

Kernel boot: **2026-11-10 12:58:33 IST**. `final-optical.json` (mode 6)
and `final-normal.json` (mode 2) both passed 200 microcode reads and 75
mode-register reads, but captured zero CSI/DMA frames. `final-pattern.json`
passed with 153600 changed bytes, zero errors, intact guards and released
memory. `final-pmic-before.json` and `final-pmic-after.json` match exactly.
`final-summary.json` records source and image hashes with the measured results.

The checked kernel build, shell syntax checks, trace-comparator checks and
`git diff --check` passed. Ordinary libtool empty-object warnings remain;
there were no camera compiler errors. Changes are local and uncommitted.
The phone is left in iOS, reporting **3% and charging** after these tests;
charge it before another reference boot or flash. No restore task is pending.


## Active register comparison, resumed at 13:13

The clock-trace run at the end of `logs/m2m/# Codex conversation.md` did **not**
capture clocks: `reference-main.txt` says `dynamic kprobe events unavailable`.
The saved reference config explicitly disables KPROBES, FUNCTION_TRACER,
CMUCAL_DEBUG and DEVMEM. Do not repeat those interfaces on this kernel.

A supported alternative was found in Samsung `sec_debug_test.c`:
`/sys/module/sec_debug/parameters/force_error` accepts `SFR ADDRESS` for a
single register read. Supplying a third token would write; the new C helper
contains only fixed, documented addresses and never supplies a write value.
It reads them synchronously while the camera stream owner keeps the domain on.

The new reference run completed with helper exit 0, one clean VC0/VC1 frame,
and automatic BOOT restoration. Evidence: `logs/camera/sfr-reference-20261110/`.
All 30 requested SFR reads were captured, including active CSI1 frame count 1
and PHY status 0. `registers.json` holds the address/value map;
`comparison.json` compares it with XNU's failed baseline. Concrete differences:

| Register | Samsung active | XNU baseline |
| --- | --- | --- |
| TOP CSIS bus source `0x1a33104c` | 1 (PLL_SHARED4_DIV2) | 2 (PLL_SHARED0_DIV4) |
| TOP CSIS bus divider `0x1a331848` | 0 (divide by 1) | 5 (divide by 6) |
| CIS_CLK1 QCH `0x1a333008` | 6 | 3 |
| CIS_CLK1 gate `0x1a332020` | 0x200000 | 0x300000 |
| CSIS QCH at 0x3058/0x3064/0x3078/0x307c | 0x3f0002 | 0x3f0003 |

Probe 7 matches the four CSI QCH settings; probe 8 also matches MCLK. Both
were tested on the 13:22:42 kernel and still got zero CSI packets and zero
changed DMA bytes, with 16 sensor-internal frames and intact guards.
Probe 9 additionally matches the dedicated bus source/divider, with bounded
BUSY waits and verified restoration. Probe 10 changes the bus while retaining
the original QCH/MCLK settings. Both were tested on the **13:25:35** kernel: zero CSI packets and zero
changed DMA bytes, with `bus_clock_restored: true`. No shared PLL was
reprogrammed. This tests the branch settings; it does not establish that
the selected upstream PLL was locked during the probe.


## Power/reference follow-up and next prepared build (18:20)

A second bounded Samsung run captured **56 distinct SFRs and 21 camera-PMIC
control registers**, full regulator summary and seven sub-PMIC registers.
It again recorded a clean VC0/VC1 frame, helper exit 0, and successful
restoration to the 13:25:35 XNU kernel. Evidence is in
`logs/camera/power-reference-20261110/`; `registers.json` and
`pmic-registers.json` decode the raw log.

Confirmed matches with XNU's active probe:
- S2MPB02 BUCK2/LDO16/LDO11/LDO6/LDO5: d8/d8/de/b0/a4.
- S2MPS22 LDO5S/LDO6S: cc/ec (850 mV / 1800 mV in Samsung's summary).
- PHY isolation, reset, bias, CSI common control and lane enable.

Samsung's additional active measurements:
- PLL_SHARED4 CON3 `0x1a33028c = 0xa0cd0400` (STABLE bit 29 set).
  The earlier XNU **idle** read was `0x80cd0400` (STABLE clear).
  These are different phases; this is a lead, not proof of the cause.
- GPC0 CON/DAT/PUD/DRV: `0x000f1021/2/0x00111101/0x00002202`.
- GPM11 CON/DAT/PUD/DRV: `1/1/1/2`.
- All four PHY data lanes' ANA_CON0/ANA_CON1 are zero; ANA_CON4 is 0x4000.

The telemetry build was subsequently deployed at 18:19:56, followed by the
full indirect-verification build at **18:27:14 IST**. Results are preserved in
`logs/camera/pll-active-20261110/`. During active mode 9/12 probes PLL_SHARED4
reads `0xa0cd0400` (locked), and PLL_SHARED0 reads `0xa1480400`. The idle lock
bit was not the cause. Mode 11 using the other source also received no frames.
Mode 12 matched Samsung's MCLK drive nibble (`GPC0 DRV=0x2202`) without restoring
CSI reception. All 200 microcode words, 75 direct reads and 178 final indirect
configuration values passed. Sensor internal counts advanced, but CSI stayed
at zero and PHY_STATUS stayed 0xf1. See `indirect-mode-9.json`,
`indirect-mode-12.json`, `stream-12.txt` and `stream-5.txt`.

## Foundation isolation: sensor sequence on Samsung receiver

The next reference boot completed and restored iOS. Evidence is in
`logs/camera/foundation-audit-20261110/`, including `isolation-result.json`.
After Samsung's initial single frame, the helper stopped the sensor, replayed
XNU's global/PDAF setfiles and normal timing/stream-on sequence over hardware
I2C, and sampled after 500 ms. The sensor counter reached 15 and the **physical
CSI1 frame counter advanced from 1 to 16**. PHY_STATUS was 0xf0, showing the
clock lane out of stop state. This demonstrates real reception under Samsung
with XNU's sensor-write sequence; no optical frame was exported to iOS.

This is a warm restart with Samsung power, clocks and receiver configuration.
It omits XNU's verification reads and uses hardware I2C instead of GPIO I2C.
Those differences and XNU receiver setup remain candidates. The test does
not establish that XNU's full cold-start sequence is correct.

## Receiver recipe isolation and XNU startup-order tests

`logs/camera/receiver-replay-20261110/isolation-result.json` records three
successful Samsung-hosted stages: sensor-only replay (CSI1 1 to 16), XNU PHY
recipe followed by sensor replay (16 to 31), and XNU CSI core plus PHY recipe
followed by sensor replay (0 to 15 after the CSI reset). Sensor counters reached
15 each time. The fixed-address reference writer read fresh SFR results through
`/dev/kmsg`, required sensor standby, and checked receiver enable/reset readbacks.
No clock, power, DMA or unsafe SYSREG +0x504 writes were made. iOS restored.
This tests the register recipes under Samsung's platform state; sysfs operations
are slower than native XNU MMIO, and the sensor power state remains warm.

Kernel **18:47:11 IST** adds modes 13 and 14, both based on mode 12:

- Mode 13 configures the receiver before programming the sensor.
- Mode 14 additionally omits pre-stream verification reads, explicitly reporting
  `prestream_verify: false` and zero validation-read counts.

Both no-DMA stream tests still saw zero CSI frames, sensor count 17 and
PHY_STATUS 0xf1. Reported cleanup succeeded; PMIC snapshots match byte for byte.
Evidence, before-images, build and verified-flash logs are in
`logs/camera/start-order-20261110/`. All prior mode selections retain their
original setup order and verification behavior. Camera.app remains synthetic.

## Resumed cold-rail isolation, 2026-11-11

Recovered the interrupted reference boot from Download Mode using the saved,
SHA256-matching BOOT image. Retrieved its log in cold-power-detail-20261110.
BUCK2 register 0x0d initially read d9 after writing d8; cleanup read d8.
The helper now polls exact readback for at most ten additional 1 ms intervals,
without rewriting or masking mismatches, and logs the initial mismatch.
Static AArch64 compilation with -Wall -Wextra -Werror and five host mock cases
passed (immediate match, transient mismatch, persistent mismatch, write/read
failure). git diff --check passed.

Hardware retest: logs/camera/cold-power-settle-20261111/. BUCK2 reached d8
on the first retry. Cold-rail setup and restoration passed; sensor count was
15 and CSI1 advanced from 0x1e to 0x2d (15 received frames), PHY_STATUS f0.
This validates this rail-cycle replay with Samsung platform state retained;
it does not establish XNU cold-boot platform correctness. Saved iOS BOOT was
restored and SSH returned. No restoration is pending. The platform-clock
telemetry kernel remains built but not deployed. Optical capture under XNU
remains unresolved; next compare the prepared platform-clock telemetry.

## Platform telemetry batch, 2026-11-11

Deployed prepared telemetry kernel (build 00:00:19 IST) with verified recovery
and BOOT writes. Evidence: logs/camera/platform-batch-20261111/. Four sequential
probes, modes 6/12/13/14, sampled nine platform controls. Every value matches
the Samsung cold-power-settle reference: six gates 0x200000, two QCH controls
0x3f0002, shared bias +0x10 zero. comparison.json labels physical addresses.
Sensor counters 17/18/18/18; CSI frame counts and IRQs zero, PHY_STATUS f1.
All cleanup flags true; PMIC snapshots byte-identical. Phone remains in iOS.
The initial host parser rejected trailing diagnostic text after valid JSON;
it was corrected to parse the first object and the saved mode-6 result reused,
without repeating the hardware test. Image/source hashes saved in manifest.json.

This closes the pending platform-readback comparison. No new mismatched static
value was found. Next investigation must distinguish native initialization
transitions/timing or unobserved platform state; do not repeat these four modes
without new telemetry or a specific changed hypothesis.

## Native timing and stream transitions, 2026-11-11

Added opt-in arguments to cam.s20.front.stream; existing calls preserve their
behavior. Argument 2: 0 unchanged, 1 readback+100 us after each PHY/reset write,
2 readback+1 ms. Argument 3 bit 0 defers immediate sensor stream/counter reads;
bit 1 primes stream for 500 ms, stops and waits for standby, then restarts
with receiver, rails and clocks alive. Each measurement remains 500 ms.
Pacing applies to established PHY addresses and reset, not every CSI core write.

Seven tests completed across two verified deployments. Evidence directories:
logs/camera/phy-pacing-20261111 and logs/camera/stream-transition-20261111.
Each contains before-images, build/pack/deploy logs, kernel identity, per-probe
raw reports, PMIC snapshots and summary.json. All tests received zero CSI
frames/IRQs; sensor internal counts advanced 17–18, PHY_STATUS f1. Cleanup
reported true and PMIC snapshots matched. Both checked kernel builds passed.
The current kernel is 00:22:48 IST; final source/image hashes are in the second
directory's manifest.json. iOS is running, no restore is pending.

These tests did not find the cause. Do not infer that timing is universally
excluded. The tested PHY pacing, immediate post-stream reads and warm stream
cycle did not repair reception. Remaining investigation: unobserved platform
state or whole-domain initialization absent from the isolated recipes. Avoid
another arbitrary delay sweep without a specific source/measurement basis.

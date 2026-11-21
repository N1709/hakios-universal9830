# Galaxy S20 camera port: evidence checklist

Updated 2026-11-11. **Read this before proposing another camera experiment.**
Scope: Exynos 990 / x1slte, front SAK3J1 first. This is the current status;
older dated sections in README and continuation notes are historical snapshots.

**Current blocker:** XNU programs a running sensor but receives zero physical
CSI packets. Samsung-hosted replays receive frames. XNU's receiver-generated
DMA pattern and iOS synthetic preview work. No optical image has been exported
to iOS. Do not turn these separate successes into an end-to-end success claim.

## How to use this checklist

- `[x]` means the stated check was completed, within its stated scope. It does
  not mean the whole subsystem is finished. `[ ]` is still an open milestone.
- Before a repeat, name the row below and what changed: code, hardware, boot
  state, measurement method, or contradictory evidence. An unchanged failed
  experiment is not a new diagnostic.
- After a test, update its row with the result, exact evidence directory,
  kernel/build identity, limitations, and the next decision. Keep raw logs.
- A brief connection/boot check and cleanup/PMIC checks are legitimate per-run
  checks. Do not rerun the full investigation just because a session changed.
- Historical passes need regression testing when relevant code changes; they
  are not permanent guarantees. Unknown or inconclusive results stay open.

## Completed checks by area

| Done | Area / check | Measured result and scope | Evidence |
| --- | --- | --- | --- |
| [x] | Hardware mapping | Front SAK3J1 on HSI2C1, GPP0-2/3, CSIS1. Use CSI v5.3 counter at +0x500. x1slte uses camera chain v8.1.0. | [Hardware notes](README.md#hardware-gates) |
| [x] | Sensor identity | Physical front ID 0x30a1, revision 0xa2 reads under XNU. | [ID and CSI history](README.md#2026-11-08-s20-csi-status) |
| [x] | Sensor rails | Active BUCK2/LDO16/LDO11/LDO6/LDO5 match Samsung: d8/d8/de/b0/a4. PHY rails LDO5S/LDO6S cc/ec. Readbacks, not oscilloscope measurements. | [Power reference](../../logs/camera/power-reference-20261110/) |
| [x] | Full global microcode | All 200 words match; indirect reads use 0x602C/0x602E. | [Readback evidence](../../logs/camera/readback-20261110/) |
| [x] | Mode configuration | 75 direct reads and 178 final indirect values pass. | [Full verification](../../logs/camera/pll-active-20261110/) |
| [x] | I2C sequence comparison | 353 global/mode transactions match visible trace bytes; full burst independently verified by readback. Captured burst text is truncated. | [Trace comparison](../../logs/camera/readback-20261110/trace-comparison.json) |
| [x] | Exposure timing correction | Clear 0x0702/0x0704 shifts: internal counter advances about 16 times in 500 ms. Internal activity alone does not prove CSI output. | [Timing tests](../../logs/camera/readback-20261110/) |
| [x] | Sensor recipe isolation | XNU sensor writes over Samsung hardware I2C: CSI1 1→16. Warm Samsung platform retained. | [Foundation test](../../logs/camera/foundation-audit-20261110/isolation-result.json) |
| [x] | Software I2C isolation | Samsung-hosted GPIO replay: 376 transfers, no NACK/I/O errors, CSI1 15→30. Does not prove every aspect of native XNU GPIO timing. | [GPIO replay](../../logs/camera/gpio-replay-wait-20261110/reference-main.txt) |
| [x] | CSI core / PHY recipe isolation | XNU PHY recipe: 16→31 frames; core plus PHY: 0→15 after reset. Samsung platform retained; sysfs writes slower than native MMIO. | [Receiver replay](../../logs/camera/receiver-replay-20261110/isolation-result.json) |
| [x] | Reset / MCLK restart isolation | Receives frames after reset and clock restart on Samsung. | [Power replay](../../logs/camera/cold-power-settle-20261111/reference-main.txt) |
| [x] | Dedicated sensor rail cycle | Receives 15 frames, CSI1 0x1e→0x2d, after rail cycle. Samsung receiver/platform stays initialized; not a complete XNU cold boot. | [Cold-rail result](../../logs/camera/cold-power-settle-20261111/receiver-results.json) |
| [x] | PMIC readback transient | BUCK2 d9→d8 on first 1 ms retry. Exact comparison retained, maximum 10 retries, no repeated writes. Full rail restoration passed. | [Detailed retest](../../logs/camera/cold-power-settle-20261111/reference-main.txt) |
| [x] | Active PLL lock | XNU PLL_SHARED4 a0cd0400 and PLL_SHARED0 a1480400 during active probe. Idle STABLE-clear reading was not evidence of an active clock failure. | [Active PLL tests](../../logs/camera/pll-active-20261110/) |
| [x] | Receiver-to-memory DMA | Hardware-generated 320×240 pattern: 153600 changed bytes, frame-end, no DMA errors, guards intact, buffer released. Synthetic, bypasses optical link. | [Final pattern](../../logs/camera/readback-20261110/final-pattern.json) |
| [x] | iOS software capture path | Two AVFoundation devices; synthetic RAW10 and NV12 preview; camera capabilities and icon enabled. No physical sensor delivery proven. | [Emulator checks](README.md#ios-capture-path-emulator) |
| [x] | Reference boot and recovery | Saved BOOT restored and SSH returned after latest test. No restoration pending at this checkpoint. | [Latest runner log](../../logs/camera/cold-power-settle-20261111/run.txt) |

## Tested changes that did not recover XNU CSI reception

These hypotheses were tested in the listed configurations. Do not call them
universally ruled out, and do not repeat them unchanged.

| Checked | Experiment | Outcome / evidence |
| --- | --- | --- |
| [x] | Normal timing versus exact Samsung Vision timing (modes 5/6) | Internal sensor timing fixed, CSI still zero. [Readback tests](../../logs/camera/readback-20261110/) |
| [x] | CSI deskew/interleave, CLK_CTRL, VC1 0x120ac, shadow config | Corrected values; CSI still zero. [Receiver history](README.md#samsung-reference-comparison-2026-11-09) |
| [x] | CSI reset / PHY skew / lane enable ordering and MMIO barriers | Corrected sequence; CSI still zero. [Continuation](continuation-20261110.md#what-changed) |
| [x] | Sensor internal pattern and faster software I2C | No link activity; neither recovered reception. [Direct comparison](README.md#direct-sensor-and-microcode-comparison-2026-11-09-evening) |
| [x] | Match CSI QCH / MCLK controls, bus source/divider (modes 7–10) | Zero frames; branch restoration verified. [Clock comparison](../../logs/camera/sfr-reference-20261110/) |
| [x] | Alternate bus PLL and MCLK pin drive (modes 11/12) | Zero frames despite locked active PLLs. [PLL tests](../../logs/camera/pll-active-20261110/) |
| [x] | Receiver setup before sensor programming (mode 13) | Zero CSI frames, sensor counter 17. [Result](../../logs/camera/start-order-20261110/stream-13.txt) |
| [x] | Omit pre-stream verification reads (mode 14) | Zero CSI frames, sensor counter 17. Reads alone do not explain failure in this configuration. [Result](../../logs/camera/start-order-20261110/stream-14.txt) |

### Native timing and stream-transition batch, 2026-11-11

- [x] PHY writes plus reset assertion/release paced with readback at 100 us
  and 1 ms: no CSI frames. Tested mode 14 baseline/100 us/1 ms and mode 12
  at 1 ms; internal counters 18/17/17/17, PHY_STATUS f1, cleanup passed.
  [Four-test summary](../../logs/camera/phy-pacing-20261111/summary.json).
  This tests PHY pacing, not every timing difference in Linux's CSI core path.
- [x] Defer immediate post-stream sensor reads until after the observation
  window: no CSI frames. Mode 14, pace 0, third argument 1.
- [x] Prime sensor stream for 500 ms, stop and confirm standby, wait 8 ms,
  restart with receiver/platform kept alive: no frames in either window.
  Third argument 2; also tested combined with deferred reads (3).
  [Three-test summary](../../logs/camera/stream-transition-20261111/summary.json).
  This is a warm stream cycle, not a second complete setfile replay.
- [x] Both batches: PMIC before/after byte-identical, reported cleanup true;
  normal iOS/SSH restored after deployment. No restoration pending.

## Open milestones, in order

- [x] **Compare remaining platform controls during active streaming.** Completed
  2026-11-11 on kernel built 00:00:19 IST. Modes 6/12/13/14 all match Samsung
  at all nine sampled controls: oscillator, reset-sync, APB/SYSREG and shared
  PHY bias. Zero CSI frames; internal counts 17/18/18/18. Cleanup reported
  true and PMIC snapshots match. [Address-labelled comparison](../../logs/camera/platform-batch-20261111/comparison.json),
  [kernel and raw probes](../../logs/camera/platform-batch-20261111/).
  These are active readbacks, not measurements of transition history or waveforms.
- [ ] **Explain native XNU platform initialization difference.** Successful
  isolated recipes retain Samsung state. Whole-domain cold initialization,
  unobserved routing/clock/reset state, and native timing interactions remain
  open. Choose the next bounded experiment from measured differences.
- [ ] **Receive physical CSI packets under XNU.** Require counter progress and
  frame start/end evidence without CSI errors, then verify cleanup.
- [ ] **Export one real optical frame under XNU.** Require matching successful
  export result, DMA completion, intact guards, plausible decoded pixels and
  a response to changing the viewed scene. A pattern is insufficient.
- [ ] **Repeat optical capture across clean boots and repeated start/stop.**
  Demonstrate consistent acquisition and restoration without resource leaks.
- [ ] **Connect real frames to the iOS capture bridge / Camera.app.** Verify
  preview and still capture. Existing synthetic bridge success is partial.
- [ ] **Sustained operation and image quality.** Frame rate, exposure/gain,
  orientation/color processing, calibration, thermal behavior, suspend/resume,
  error recovery and any supported focus controls remain unverified.
- [ ] **Rear main S5K2LD.** Resolve ACK/identity on documented bus; repeat the
  hardware milestones for this module. Front success does not validate rear.
- [ ] **Other rear modules and lens switching.** Deferred until front works.

## Invalid results and methods not to repeat unchanged

| Item | Why it is closed / what would justify reopening |
| --- | --- |
| CSI frame counter at +0x100 | Wrong v5.2 map; result invalid. Use v5.3 +0x500. |
| Indirect read via 0x6028/0x602A | Write pointer, misleading values. Use 0x602C/0x602E. |
| SYSREG_CSIS +0x504 | External abort under XNU; wrong chain variant supplied the original idea. Do not retry ordinary MMIO access. |
| Standalone QCH gate toggling | Earlier probe panicked and was removed. Matching final values is not evidence that arbitrary transitions are safe. |
| Kprobes/ftrace/CAL debug or /dev/mem on saved Samsung kernel | Required support unavailable. Existing fixed-whitelist SFR capture works. Reopen only with changed kernel capabilities. |
| Initial GPIO replay standby failure | Setup failed before replay; superseded by successful standby-wait run. |
| Initial cold-rail setup failure | Not evidence of no reception after cycling rails; superseded by successful bounded readback retest. |
| Initial Samsung Vision single frame | Timing shifts caused slow internal progress; subsequent normal-timing replays received 15 frames. |
| Historical “no physical frame” statements | Still true for XNU optical capture; no longer true for Samsung reference reception. |

## Next-session entry point

Read this file, check the last row's evidence for the area being changed, then
use [continuation notes](continuation-20261110.md) for commands and
[reference-boot instructions](../reference-boot/README.md) for boot/log handling.
The latest deployed XNU kernel was built **2026-11-11 00:22:48 IST**. Verify the
live kernel before comparing results. Keep new evidence separate from old runs.

## CSIS/PHY routing re-check, 2026-11-11 (session B, phone-free)

- [x] Front 3J1 route from x1slte DT (boot-dtb.0 + dtbo.1, fragment@21 → is_sensor1): module id 1 → CSIS 0x17040000,
  PHY `dphy_m0s4s4s4s4s4s2_csi1` at 0x170d2b00 (D-PHY, reset-sysreg bit 3, PMU isolation 0x730), csi_ch 1, no C-PHY.
  Matches `redwood_camera_s20.c` (CSIS1_PHYS / CSIS1_DPHY_PHYS). Routing is NOT the blocker.
- Trap: `device/dump/*/live-fdt.dtb` is the S20 Ultra (z3s) tree. There, is_sensor1 is 0x17050000 + C/D-PHY 0x170d1b00.
  Never take camera wiring from it.
- [x] PHY recipe re-audit: `cam_s20_dphy_3j1_config` = Samsung `__set_phy_cfg_0503_0000_dcphy` mode D / type D
  (v8.1 → PHY 0x0503.0000; CSIS1 = CSI_ID_B), including the clock-lane skew quirk, write order and 200 us settles.
- **Key discriminator (from existing logs, no new run):** Samsung active stream SFR read of CSIS1 `PHY_STATUS`
  (0x17040700) = **0x0** ([power-reference](../../logs/camera/power-reference-20261110/reference-main.txt), line ~207);
  XNU = **0xf1 in every sample of every run** (or=and=0xf1, `phy_active_samples` 0, 11 runs on 2026-11-11).
  Under XNU no lane ever leaves LP-11 stop state. Either the sensor never starts HS transmission, or our PHY's
  LP receivers do not observe the wires at all.
- [ ] **Line probe** (patch ready: `proposals/camera/line-probe/line-probe.patch`, opt-in third-arg bit 4,
  `rwsys kern.redwood.poke 'cam.s20.front.stream 14 0 4'`): before the receiver is restored, sample PHY_STATUS with
  the sensor in standby, in reset, and with rails off. If rails-off still reads 0xf1, the PHY is blind → work on
  the PHY/receiver bring-up (power/clock/reset of the PHY block). If it changes, the PHY works → the sensor never
  enters HS under XNU → work on the sensor side (MCLK quality/frequency, power-up timing, stream-on sequence).

### Session B flash batch, 2026-11-11 08:10–08:35 (kernels 08:10:09, 08:18:07, 08:27:21, 08:31:59)

- [x] **Line probe** (`cam.s20.front.stream 14 0 4`, 3 runs): PHY_STATUS 0xf1 in standby, sensor reset AND sensor rails
  off. [logs](../../logs/camera/line-probe-20261111/). Sensor counter 1 on two boots, 17 on later boots (erratic).
- [x] **273-register Samsung-vs-XNU compare during streaming** (shared list `tools/reference-boot/camera-regdump-list.h`,
  XNU bit 8, Samsung `camera-ref.c -DCAMERA_FULL_REGDUMP`, `tools/camera/compare-regdump.py`): all 117 CMU_CSIS, 58 CSIS1
  core, 70 PHY, bias, PMU iso/PD, PHY reset, TOP camera clocks/PLLs, MCLK/reset pins. Only 17 differ: unused VC2/3,
  DMA mux 0x17020418, OIS QCH, GPC0-0/3 parking (other cameras' MCLK pins), GPM11 PUD, plus results (FRM_CNT, PHY_STATUS
  Samsung 0xf0 vs XNU 0xf1). [logs](../../logs/camera/regdump-20261111/comparison.json). Sensor registers match too
  (only 0x0340 frame length: Vision timing). S2MPS22 PHY supplies decode to 0.85 V / 1.8 V (= Samsung).
- [x] **Matched every remaining difference** (bit 16) + PHY-in-reset sample: still 0 frames, PHY_STATUS 0xf1 even with
  the PHY held in reset. [logs](../../logs/camera/match-rest-20261111/)
- [x] **Fresh CSIS power-domain cycle** exactly like Samsung pmucal csis_off/on + DTZPC save/restore (bit 32): off/on OK,
  still 0 frames. A repeat in the same boot timed out on power-off and the next run HUNG the phone (forced restart).
  Rule: at most ONE bit-32 run per boot. [logs](../../logs/camera/pd-cycle-20261111/)
- Samsung reference runs without replays finish in ~25 s (flash→restore 35 s): normal, it did run (CAMERA_REGDUMP in log).
- Conclusion: readable register state is identical; the sensor's clock lane never goes HS under XNU (Samsung: always HS
  while streaming). Remaining: dynamic sequence/timing, or sensor MIPI TX not starting (MCLK quality, start-up timing).
- [x] **Main PMIC (S2MPS19) rails vs Samsung** ([logs](../../logs/camera/pmic-main-20261111/)): LDOs used by Samsung all on at
  the same voltage (LDO6M 750 vs 775 mV, no consumer). Bucks were low under XNU: BUCK3M 662/762, BUCK5M 587/675 mV.
  `acpm.rate 11 690000; acpm.rate 12 690000; acpm.rate 1 533000` (INTCAM, CAM, INT max) raises BUCK3M/5M to exactly
  Samsung's 762/675 mV. BUCK6M follows CPU/GPU load (not camera). Stream with these voltages: still 0 frames
  ([logs](../../logs/camera/dvfs-20261111/)). Note for the final driver: request INTCAM/CAM/INT like Samsung's is-dvfs.
- Samsung Vision reference: sensor counter 1 per run AND CSIS FRM_CNT 1 (one long frame); XNU (same boot type) sensor 1,
  CSIS 0. Same sensor behaviour, receiver sees nothing.
- **Next (recommended):** decisive split on Samsung's kernel WITHOUT its camera driver touching the receiver: replay XNU's
  whole receiver sequence through sec_debug `SFR ADDR VAL` writes (write form), sensor over /dev/i2c-1. Frames → the gap is
  kernel/platform level; no frames → our sequence misses a step Samsung's driver does.
- [x] **DECISIVE SPLIT (2026-11-11 08:49, `camera-ref.c -DCAMERA_XNU_COLD`):** Samsung only does `S_INPUT` (front
  camera open: PD+DTZPC, clocks, DVFS qos, PHY power, sensor rails/reset/MCLK). Its receiver is never configured and its
  stream never started. Then XNU's full receiver sequence (SFR writes) + XNU sensor programming (hardware I2C) + stream:
  **CSIS1 FRM_CNT 0 → 15, PHY_STATUS 0xf0. FRAMES.** [logs](../../logs/camera/cold-split-20261111/)
  → XNU's receiver sequence and sensor programming are CORRECT. The gap is in what S_INPUT provides.
  After-open regdump vs XNU: every register XNU does not itself write is identical (all CMU_CSIS, TOP, PMU, pins).
  Remaining candidates: (1) sensor power-up incl. MCLK *start* from CMU_TOP CIS_CLK1 (earlier Samsung rail-cycle tests
  always kept Samsung's running MCLK clock); (2) non-register platform: DVFS levels (MIF/INT/CAM), CSIS SYSMMU, BTS.
- [ ] **Next split:** same as above, then after S_INPUT do XNU's complete sensor power-off/on INCLUDING the CMU_TOP
  CIS_CLK1 mux/div/qch/gate writes in XNU's order (extend camera-power-replay.h + receiver_allowed whitelist).
  Frames → sensor power path is fine, gap = platform services (try DVFS/SYSMMU next). No frames → bisect the sensor
  power-up steps (MCLK start order vs reset vs rails).

## SOLVED 2026-11-11 13:41 — XNU receives physical CSI frames and exports a real optical frame

- [x] **Root cause:** S2MPB02 BUCK1 (reg 0x0b), buck-boost (0x0f) and LDO18 (0x2f) are always-on/boot-on in Samsung's
  x1slte DT, off after S-Boot, never enabled by XNU (map `0b=18 0f=18 2f=6c`; Samsung `d8 da ec`). Proof on Samsung's
  kernel with the XNU sequence: on → 15 frames, off → 0 frames, PHY 0xf1 ([logs](../../logs/camera/baseoff-split-20261111/)).
  Enabling them 9 ms before use was not enough (sensor counter 255); enabled at boot it works.
- [x] **Fix:** `redwood_s2mpb02_torch.c` enables the three at boot (`cam_pmic_base=1` default).
- [x] **Receive physical CSI packets under XNU:** `cam.s20.front.stream 14 0 4` / `6 0 0`: 0 → 17 frames, FS/FE irq 0x3,
  473/500 PHY-active samples, cleanup true. [logs](../../logs/camera/pmic-base-boot-20261111/)
- [x] **Export one real optical frame:** `cam.s20.dma.test 1 14` exported 1824x1368, frame-end, dma_error 0, guards 0,
  pixels 1..1023; `ssh s20 /var/root/s20-frame > frame.raw; python3 tools/camraw.py frame.raw PREFIX` → real scene
  (ceiling, fan blades). Still to do from the milestone list: scene-change check, repeat across boots, feed real frames
  to the iOS capture bridge / Camera.app, image quality, rear cameras.

## ROADMAP after the first live camera (user's list, 2026-11-11 ~14:00) — continue from here

State: front SAK3J1 live in Camera.app through `cam=5 cam_s20=1` (default in `x1slte.cmdline`), commits 60e706a +
9027baf on `x1slte`. **User verified: preview shows, photo saves to Photos.** Everything below is open.
Code map: kernel `xnu-src/.../bsd/dev/redwood_camera_s20.c` (sensor/receiver/DMA + stream loop),
`redwood_camera.c` (`s20_session`, `/dev/redwood-camera`), `redwood_s2mpb02_torch.c` (camera PMIC + flash LED);
userland `userland/src/camera_device.c` (capture device in mediaserverd), `camera_isp.c` (CPU ISP), `inject_media.c`.

### 1. Front camera quality ("looks like a 2000s photo")
- [ ] Confirm orientation/mirroring after the 180-degree turn (kernel 13:56:21) and natural colours (Bayer GB/RG).
- [ ] Full resolution: now the binned 1824x1368 mode. Add the sensor's full modes from Samsung's setfile
  (`is-cis-3j1-setA.h`: 3648x2736 4:3, 3968x2232 16:9, 2736x2736) for stills; keep a binned mode for preview.
- [ ] Colour tuning file for this sensor (log: "no colour tuning ... no colour matrix, no lens shading"): colour
  matrix, lens shading, black level, noise reduction/sharpening in `camera_isp.c`. Samsung's calibration is in the
  module EEPROM (read at Samsung's S_INPUT) and `setfile_3j1.bin`.
- [ ] Exposure limits: analog gain max is 16x now; check flicker (50 Hz banding), long exposure, HDR off.
- [ ] Front autofocus: the DT lists an actuator for the 3J1 (`af` node, i2c ch 3) — check and drive it.

### 2. Frame rate / smoothness
- [ ] Preview is ~15 fps: `s20_frame()` packs from the write-combined DMA mapping (slow reads) and misses every other
  frame. Fix: cached mapping of the DMA buffers (+ cache invalidate per frame) or let WDMA write packed RAW10 directly
  into the ring (no CPU pack); move packing off the polling loop; use the CSIS/DMA interrupt (GIC SPI 215 is masked).
- [ ] Request camera DVFS while streaming like Samsung (`acpm` INTCAM 11, CAM 12, INT 1 — only ever raise, never lower).
- [ ] CPU ISP cost in userland (`camera_isp.c`) at full resolution; consider GPU (Metal) debayer.

### 3. Video
- [ ] Video recording does not save (user). Find where the recording path stops (mediaserverd / AVAssetWriter / H.264
  encoder: no hardware encoder on this port — check VideoToolbox software path).
- [ ] Link the microphone to video (`/dev/redwood-mic` works for Voice Memos; the capture graph needs an audio device
  for the camera session).
- [ ] Video modes the S20 hardware has: HD/FHD up to 120 fps (slow-mo; super slow-mo higher), UHD 60, 8K 24 (rear main).
  Needs the sensors' high-speed/UHD modes, the receiver at those link rates, and an encoder that keeps up.

### 4. Rear cameras (three) + switching by zoom
- [ ] Rear main S5K2LD (position 0, I2C0, MCLK0, CSIS per DT fragment@20 `is_sensor0`, C-PHY `use_cphy`): retry
  `cam.s20.id` now that the PMIC base supplies are on (its earlier NACK likely had the same cause); then power
  sequence (incl. VDDPHY_1.05V_CAM, OIS/AF rails), C-PHY receiver recipe (`__set_phy_cfg_0503_0000_dcphy` mode C),
  sensor setfile, stream. Careful: the S20 Ultra tree (live-fdt) is NOT this phone; use boot-dtb.0 + dtbo.1.
- [ ] Rear ultra-wide / second sensor S5KGW2 (position 2, I2C1, MCLK0) and telephoto S5K2LA (position 4, I2C4, MCLK3).
- [ ] Expose them to iOS as the back cameras (`REDWOOD_CAMERA_SENSOR` switch already exists in the device contract:
  0 macro, 1 wide, 2 front, 3 ultra-wide) and switch lens by zoom level (0.5x / 1x / 3x) like the Camera app expects.
- [ ] Autofocus and OIS for the rear main (actuator + OIS MCU; CMU OIS_MCU clocks exist in the register list).

### 5. Flash
- [ ] Link the rear flash LED (S2MPB02 FLED, already driven as the torch in `redwood_s2mpb02_torch.c`) to the capture
  path: flash on photo, torch during video, front "screen flash".

### 6. Robustness
- [ ] Repeated open/close, app switching, lock/unlock, sensor switch; no leaks, rails off when idle
  (base supplies BUCK1/BB/LDO18 stay on from boot by design, as on Samsung).
- [ ] Thermal behaviour during long preview/recording; suspend/resume.
- [ ] Remove or gate the diagnostics (`cam.s20.front.stream` bits) once the driver is final; keep `cam_pmic_base`.

### 2026-11-11 evening: second half of the root cause (cold start)
- [x] S2MPB02 BUCK1/BUCK2 **voltage** registers (0x0c/0x0e) reset to 0xc0/0x40 (1.6 V / 0.8 V) on a hard reset; Samsung
  sets 0x90 (1.3 V). The afternoon's success rode on Samsung's values surviving warm restarts. Fixed at boot in
  `redwood_s2mpb02_torch.c`. Verified on the first boot after a forced power-off (kernel 18:54:31).
- Lesson: after any Samsung reference boot, do a HARD power cycle before trusting an XNU camera result.
- Rear cameras will need their own LDO voltage codes (Samsung map: LDO1 0x1e=0x24, LDO3 0x20=0x24, LDO13 0x2a=0x58; XNU
  cold values 0x18/0x20/0x5c) — take them from a Samsung `CAMERA_PMIC_MAP` with each rear sensor open.
- Boot hang 18:26: a kernel with a read-only cached DMA mapping + `dc ivac`, flashed together with a root.apfs rebuilt by
  hand (`mkapfs.py ... redwood=root`); cause not separated. Rules: new kernel features that run at camera open stay OFF at
  boot (mediaserverd opens the camera during startup); never flash a hand-rebuilt root.apfs together with a kernel change.

## Front camera status 2026-11-12 (session B) — what works, what is next

Works (user verified in Camera.app): live preview at 30 fps from the full 3648x2736 sensor mode, photos save, correct
orientation with `cam_turn=2` (default from kernel 04:36:50 on; live: `tools/poke 'cam.turn 2'`, then switch cameras or
reopen the app — it is read at stream start), autofocus (AK7374), no banding (50 Hz anti-flicker), stable exposure.

Open, in priority order:
- [ ] **Video recording = empty file (0x0, 36 bytes).** Cause found: this iOS image has only Apple's HARDWARE encoders
  (`/System/Library/VideoEncoders/AppleVideoEncoder.bundle`: "Apple H.264 (HW)", "Apple HEVC (HW)") which need the
  iPhone's AVE chip; the S20 has none. Needs our own VideoToolbox encoder (software H.264, e.g. OpenH264 BSD-licensed,
  at 1080p30 on the A76/M5 cores; or a Mali GPU encoder later), registered so VTCompressionSession picks it.
- [ ] **Video/panorama preview slow:** libredwood-media's CPU ISP renders every frame at the video format's size
  (3840x2160 for the front camera's "Video-30"). Move debayer/colour/scale to the GPU (Metal works: RedwoodMetal) or
  give the graph smaller buffers; also pick the binned 1824x1368 sensor mode for video.
- [ ] **Microphone in video** (after the encoder).
- [ ] **Photo save is slow** (~0.4 s ISP + HEIC encode on CPU): GPU ISP, threads.
- [ ] **Front camera reports 7 MP** (iPhone X front format 3088x2320 in camera_device.c `cameras[]`); the sensor is 10 MP.
  Changing the format must still match an entry of CMCapture's D22 AVCaptureSession.plist.
- [ ] **Flash** for the front camera = iOS screen flash (FlashMode is "set (not used)" in camera_device.c).
- [ ] Colour matrix / lens shading tuning for SAK3J1 (still none: "no colour tuning for the ov16a1q"), denoise/sharpen.
- [ ] The "Back Camera" button shows the front sensor until the rear cameras are brought up.

### Video encoder plan (2026-11-12, session B) — reverse-engineered VideoToolbox encoder interface
From `/System/Library/VideoEncoders/AppleVideoEncoder.bundle/AppleVideoEncoder` (each slot logs "ENTER AVE_H264<name>"):
- Factory: `OSStatus create(FourCharCode codec, CFAllocatorRef, CMBaseObjectRef *out)` → `CMDerivedObjectCreate(alloc,
  &vtable, VTVideoEncoderGetClassID(), out)`; vtable = { 0, &baseClass, &encoderClass }.
- Base class (version 1): [1] derived storage size, [3] Invalidate, [4] Finalize, [6] CopyProperty(obj,key,alloc,out),
  [7] SetProperty(obj,key,value).
- Encoder class (version 3): [1] StartSession(enc, VTEncoderSessionRef, codecType, CMVideoDimensions packed in x3),
  [2] EncodeFrame(enc, token, CVPixelBuffer, CMTime *pts, CMTime *dur, CFDictionary frameProps, VTEncodeInfoFlags *),
  [3] CompleteFrames(enc, CMTime until), [4] CopySupportedPropertyDictionary(enc, CFDictionaryRef *), [5]
  SetProperties(enc, CFDictionary), [7] PrepareToEncodeFrames(enc), [8] BeginPass, [9] EndPass, [17-20] tiles.
- Registration: `VTRegisterVideoEncoderWithInfo('avc1', infoDict, factory)` (AVE calls it the same way).
- Output: `VTEncoderSessionEmitEncodedFrame(session, token, status, infoFlags, CMSampleBuffer)`; input format via
  `VTEncoderSessionSetPixelBufferAttributes`; format description from SPS/PPS
  (`CMVideoFormatDescriptionCreateFromH264ParameterSets` or `VTEncoderSessionCreateVideoFormatDescription`).
- Engine: OpenH264 (BSD) built for iOS arm64, NV12 → I420, Annex-B → AVCC. Registered from libredwood-media in
  mediaserverd. Camera.app must record H.264 (Settings → Camera → Formats → Most Compatible) until there is an HEVC one.

## OPEN ITEMS — camera wrap-up 2026-11-12 (session A); details in tools/camera/REAR-CAMERA.md
Done and verified today: front camera (B); rear streams in Camera.app; front/rear switching reliable; rear warm start
(0.35 s flips, B); rear flash LED (B); rear video recording saves (H.264, A/V in sync, ~20 fps, 7c3b2ee); in root.apfs.

- [ ] **Rear dark scenes: colours / black level.** S5K2LD black is not 64: sinks with gain (to ~17-39 at 12-16x), differs
      per Bayer site, drifts left-right, depends on exposure. Binned mode uses B's per-gain tables (camera_device.c).
      Leads: Samsung LN modes (0x0B30 0x0101 LN2 at mode change, never tested alone), 0x0A7A/0x0A7C, sensor BLC 0x0B04 +
      a pedestal register, a per-column dark map (phone face down; redwood-fixed-exposure + redwood-raw-dump tools).
- [ ] **Rear lens shading** (vignetting, green corners): files are flat 1.0. White paper over the lens, then
      `touch /var/mobile/Library/Caches/com.apple.mediaserverd/redwood-lsc-capture`.
- [ ] **Occasional rear frame glitches:** likely race — the reader thread writes isp_black4 / isp_black4_low / isp_band*
      for frame n+1 while the draw thread renders frame n (camera_device.c ~1989-2173 vs render_wait ~2342). Copy them
      into the render handoff (h) with isp.
- [ ] **Heat:** 91-96 C with the camera on (CPU ISP + OpenH264) → throttling, rear ~20 fps (28 cool). Options: cheaper
      rear preview path (cells 1008x756 enlarged to 1504x1128 is the slow "magnified" path), smaller encode size.
- [ ] **12 MP rear (cam_s20_rear_full=1, off):** plain Bayer after 0x021E=0 (sensor HDR off), but a black band from row
      1912 and a 32-row structure; two watchdog resets during 12 MP raw dumps. Leads: Samsung's retention sequence
      (cam_s20_rear_ret=1), LN2, 0x0702/0x0704 shifters, stream-off 0x6028/0x602A/0x010E sequence (agent audit, sessions/A.md).
- [ ] **Rear start from cold ~3 s** (Global_A3 over bit-banged I2C); warm flips are fast. Leads: HSI2C controller driver,
      half_us 1 alone (never tested alone), Samsung retention.
- [ ] Metadata/format mismatches found by audit (minor): transfer tag BT.709 vs sRGB curve applied, no ChromaLocation,
      ISO ignores digital gain, advertised format sizes (camera_device.c ~402, 443-449) differ from real frames.
- [ ] Not started: ultra-wide (S5KGW2), tele (S5K2LA), Live Photo movie (both cameras), CSIS0 ERR_WRONG_CFG (VC1 PD rows).

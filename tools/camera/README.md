# Galaxy S20 camera bring-up tools

Implementation status and host-only build/test commands: [primary camera quality fixes](../../docs/devices/x1slte/camera/implementation.md).

Research preservation and complete handoff: [S20 camera research archive](../../docs/devices/x1slte/camera/research-handoff.md).

Current offline stock-versus-port quality investigation: [x1slte-camera-quality](../../docs/devices/x1slte/camera/quality.md).
Typed stock tuning decoder and reproduction commands: [ISV8 format report](../../docs/devices/x1slte/camera/tuning-format.md).
Latest: [configuration audit and verified colour-helper reconstruction](../../docs/devices/x1slte/camera/config-audit.md).
The dated bring-up status below is historical; physical rear capture now works.


The S20 is Exynos 990. Keep its camera diagnostics separate from `cam=` and
`bsd/dev/redwood_camera.c`, which target the Qualcomm POCO X5 Pro. Bring-up priority is **front SAK3J1 first**, because the rear main S5K2LD currently does not ACK on its documented bus; switch back to rear main after the front CSI path is proven. Secondary rear modules follow.

Start here: [camera port evidence checklist](CHECKLIST.md). It records completed
checks, failed experiments, limitations, and the ordered remaining milestones.
Update it after each experiment so future sessions do not repeat unchanged tests.
Detailed history: [continuation notes](continuation-20261110.md).

As of 2026-11-11, Samsung-hosted sensor, receiver, GPIO-I2C and sensor-power
replays have received physical CSI frames. XNU optical capture still receives
zero CSI frames; receiver-generated DMA patterns and the synthetic iOS preview
work. The nine additional active platform-control readbacks now match Samsung in
four XNU probe configurations; initialization transitions and unobserved
platform state remain open.
The dated sections below preserve historical results, not the latest status.

## Read-only snapshot

Run from the repository root:

```sh
python3 tools/camera/s20-diag.py
```

Requires the existing `ssh s20` alias. It records source revision and build
artifact timestamps, kernel boot arguments and device-tree camera nodes,
camera device nodes, Camera.app bundle, MobileGestalt camera capabilities,
SpringBoard icon reference, recent media logs, and the HackiOS kernel log. The
complete kernel log is saved beside the report. The report is timestamped under
`logs/camera/`. It does not reboot, respring, or otherwise change phone state.
Use `--out PATH` to choose a different report path.

## Hardware gates

Each camera change should produce a report and pass these gates in order:

1. **Boot/recovery baseline:** record boot ID, source revision, kernel artifact
   hash and phone logs. Keep a known-good recovery image available.
2. **Inventory (`tools/poke cam.s20`):** read pin mux/pull/drive/input for the
   main/front sensor I²C, MCLK and reset pins. The front SAK3J1 is a child of HSI2C1 on
   `gpp0-2/3`; `gpp1-0/1` is HSI2C4 for the display PMIC, and `gpm14/15`
   is HSI2C45 for the rear tele module. This first command
   is read-only and does not touch clocks, PMIC rails, sensor reset, HSI2C or CSI/PHY. Extend
   inventory to those blocks only after their mappings and safe read registers
   are independently verified.
3. **Sensor identification and mode setup:** front `cam.s20.front.id` now reads SAK3J1 ID `0x30a1` / revision `0xa2`; `cam.s20.front.mode` applies the Samsung 1824×1368/30 non-PDAF setfile, verifies registers, and powers down/restores state. Rear `cam.s20.id` still needs an ACK on its documented bus.
4. **CSI receiver:** `cam.s20.front.stream` runs the physical SAK3J1 in the 1824×1368/30 RAW10 mode for 500 ms and checks the CSIS1 VC0 frame counter. It does not enable DMA. Optical capture requires that count to advance. A separate receiver-generated pattern test can independently validate DMA before the sensor link works; see [the S9 comparison](s9-comparison.md) for its prerequisites.
5. **Single frame (`cam_s20=4 sensor=main|front`):** capture one frame, stop,
   and confirm rails, pins and clocks return to the recorded baseline. Decode
   with `tools/camraw.py` only when framing matches its documented format.
6. **Application preview:** after repeatable single frames, connect the iOS
   capture bridge and make Camera.app discoverable on the Home Screen. Verify
   main and front independently before lens switching or sustained streaming.

Failures must identify a bounded phase: controller timeout, bus NACK, bad ID,
rail/clock mismatch, CSI packet error, DMA timeout, or userspace delivery stall.
A camera failure must never reboot automatically. Save logs and frame artifacts
before any explicit reboot or flash.

## Pin/I/O trace format

Emit stable JSON events with boot ID, module, stage, phase, timestamp,
resource/register, prior value, requested value, readback, result and error.
Use pin names from the x1slte DT and register block/base/offset, not unlabeled
numbers. On failure, report the first failing event and every reverse-order
cleanup result. A bounded flight recorder should retain recent events across a
userspace service crash. Do not expose arbitrary raw-register writes.

Use existing `tools/phone-lock.sh`, `x1slte.sh`, and `tools/x1slte-auto.sh`
for reboot and recovery workflows. Camera diagnostics never acquire the phone
or initiate a flash/reboot.

## iOS capture-path emulator

The XNU camera device and mediaserverd bridge can be exercised without camera
hardware. With `cam=5 cam_fake=1` in `x1slte.cmdline`, the kernel serves a
generated 1920×1080 RAW10 chart through `/dev/redwood-camera`; `cam_fake=1`
returns before mapping or touching camera MMIO. The mode is built in, so this
test does not depend on a sensor firmware table. The same bridge advertises Back
and Front Camera devices to AVFoundation. `cam_fake_fps` defaults to 5 fps;
raise it only after checking CPU/thermal impact.

Smoke checks on 2026-11-08: `camera_avtest` listed two cameras;
`camera_test raw 3` returned three 1920×1080 frames and rendered a 960×540
NV12 preview; `camera_test raw 3 sensor 2` also returned three frames through
the front stream selection. The camera capability cache was backed up to
`/var/tmp/MobileGestalt.plist.before-camera-20261108` and now reports
front/rear/still-camera true. The Camera icon is present in SpringBoard's
IconState. These checks prove the emulator and software path only. The frames
are a generated chart, not the phone's optics.

## PMIC inventory

`tools/poke cam.pmic` reads the S2MPB02 ID, BUCK2, and front-camera VT LDO5/LDO6/LDO11/LDO16 control registers. It performs no writes and serializes with the existing flashlight I²C transactions. The enabled bit is bit 7 of each LDO control register. This snapshot is required before any sensor power test.

## 2026-11-08 S20 CSI status

`cam.s20.front.id` reads the physical SAK3J1 (`0x30a1`, revision `0xa2`), and
`cam.s20.front.mode` applies the Samsung 1824×1368/30 setfile and restores sensor
state. The safe `cam.s20.csis1` and `cam.s20.csis.dphy` snapshots also pass. No
physical sensor frame or CSIS frame counter increment has been demonstrated.

The panic report captured on 2026-11-09 says `Unhandled implementation specific
error` in `rwsys`; symbolizing its backtrace puts the fault in
`cam_s20_csis_gate_probe` while it toggled/restored CSIS QCH gates. This does
not prove DPHY configuration caused that panic. The risky standalone gate probe
is removed. The bounded DPHY configuration test is SMC-backed and verifies
full state restoration.

On 2026-11-09, a later stream test panicked when it tried to read
`SYSREG_CSIS + 0x504` (`MIPI_PHY_SEL`). The saved log reports a synchronous
external abort at that MMIO address, with the backtrace in
`cam_s20_front_stream_probe`; this register is not safe to access through the
current XNU mapping. That access was removed and a corrected kernel was flashed
from Log Access. Do not retry this register via `ml_io_map` without a documented
EL3/TZPC access path.

The exact Exynos990 path enables S2MPS22 `VDD_MIPI_CSI_0P85_AP` (LDO5S) and
`VDD_MIPI_CSI_1P8_AP` (LDO6S) before `phy_set()`. The read-only rail check
reports `0xcc` and `0xec`, both enabled at 0.85 V and 1.8 V. The current
0x0503.0000 D-PHY recipe follows Samsung's `__set_phy_cfg_0503_0000_dcphy()`
for mode D/type D and 1807 Mbps: bias at `0x170d1000`, four data lanes, skew
delay 3, skew calibration enabled, settle 9, settle-clock select 0.
`cam.s20.csis.dphy.config` now reports configured with lane-enable readback
`0x3` (bit 0 is enable; upper bits are hardware status) and zero restore
mismatches. `cam.s20.front.stream` checks real frame-counter progress without
DMA and tears down after 500 ms. Camera.app's color chart remains synthetic;
it does not prove optical capture. Do not add DMA or claim camera hardware
success until the physical CSIS frame count increments.

The x1slte DT maps front SAK3J1 (`sensor id=1`) to CSIS1 (`id=1`, `csi_ch=1`).
Use Samsung's **v5.3** receiver register map (`is-hw-csi-v5_3.h`) for this
Exynos990 block: VC0 frame count is `0x0500`. The v5.2 map assigns that name
to `0x0100`; the temporary 2026-11-09 probe build that read `0x0100` used the
wrong version and its frame-count result is invalid. Earlier v5.3 probes at
`0x0500` correctly reported no CSIS frames while the sensor's internal frame
counter advanced. The captured `PHY_STATUS=0xf1` decodes as all four data
lanes and the clock in stop-state during the sample. Physical image capture
remains unverified; Camera.app still shows the synthetic chart.

On 2026-11-09 the probe sampled CSIS1 `PHY_STATUS` and `CSIS_INT_SRC0`
500 times over 500 ms. With non-PDAF 1824×1368, the sensor's internal frame
counter advanced (`0xff` to `0x11`), but VC0 frame count and packet interrupts
stayed zero; `PHY_STATUS` stayed `0xf1` on every sample. The stock setfile's
regular 1824×1368 mode was added as `cam.s20.front.stream 1`. It produced
`0x0114=0x0301` and an advancing internal counter (`0xff` to `0x01`), but the
receiver still saw no frames or lane activity. Both probes restored hardware
state and iOS returned normally. Samsung's 3 ms pre-setfile and 200 µs PHY
settle waits were also added; they did not change the non-PDAF result.

The stock SM-G980F_INU_G980FXXSNHYB7 AP archive's DTBO was decompressed and
its two entries matched `device/dts/dtbo.0.dtb` and `dtbo.1.dtb` byte for byte.
ExtremeROM 3's EROFS system image lists `SamsungCamera.apk`; its vendor init
starts `vendor.samsung.hardware.camera.provider@4.0-service_64`, and the
extracted `media_profiles_V1_0.xml` lists a front camera profile. These files
confirm Android's userspace camera stack; the hardware probe still has to
find where CSI lane activity stops.

The stock `stock_defconfig` for x1slte selects `CONFIG_PABLO_V8_1_0=y` and
`CONFIG_CSIS_V5_3=y`. Camera chain v8.1.0 leaves `is_hw_camif_cfg()` empty;
the `MIPI_PHY_SEL` write belongs to v8.10.0, so it is not an x1slte setup
step. The 2026-11-09 attempt to access that register came from comparing the
wrong variant and must not be repeated. The stock front-camera firmware is
already available locally at `device/firmware/vendor-firmware/setfile_3j1.bin`
(relative to the research root) for offline comparison. The safe v5.3 probe
read at `0x0500` reported zero VC0 frames while the SAK3J1 internal counter
advanced.

On 2026-11-09, Samsung's `sensor_3j1_cis_set_size()` page-select write
(`0x6028=0x2000`) was added before the iOS probe's crop writes. Both the
stock mode-8 PDAF setfile and the non-PDAF variant were retested after a
verified flash. In each 500 ms probe the sensor's internal frame counter
advanced about 18 frames, but CSIS1 VC0 remained at zero, no frame interrupt
arrived, and `PHY_STATUS` stayed `0xf1` in all 500 samples. Thus page selection
is now correct, but the physical CSI path is still inactive and Camera.app
still displays synthetic pixels. The Samsung Vision reference did receive
one VC0+VC1 frame, so the next useful comparison is a register-level trace
of the sensor and CSI setup in that boot against this bounded iOS probe.

## Samsung reference comparison, 2026-11-09

The temporary Samsung Vision boot can now power SAK3J1 and start CSIS1 after
explicitly enabling S2MPB02 BUCK2. With two ION buffers queued, Samsung's
`debug_irq=2` trace records one clean VC0/VC1 frame start and frame end, with
zero MIPI errors. The SAK3J1 I2C frame counter reaches 1 and remains there
for five seconds: this minimal Vision setup also stops the sensor after one
frame. See `tools/reference-boot/README.md` and raw logs in
`work/stock-camera-test/reference-vision-phy-main.raw`.

The Samsung register dump gave `CSIS_CMN_CTRL=0x4b01` and
`CSIS_CLK_CTRL=0x10`; the iOS bounded probe now matches its deskew,
interleave and clock bits, and Samsung's D-PHY skew-delay writes. It also
configures VC1 as RAW10 at 1824×342, as the stock device tree requires.
Live tests after each change still showed `FRM_CNT_CH0=0`, no packet IRQ and
`PHY_STATUS=0xf1` throughout 500 ms, while the sensor's internal counter
advanced. These changes are diagnostic only. Camera.app still uses its
synthetic preview; no optical image has yet appeared in iOS.

### Direct sensor and microcode comparison (2026-11-09 evening)

The stock Samsung Vision run and `cam.s20.front.stream 2` now have identical
readbacks for the sensor's output format (`0x0112=0x0a0a`), four-lane mode
(`0x0114=0x0301`), MIPI PLL (`0x0306=0x00d9`, `0x0310=0x0116`), crop, binning,
MIPI timing registers `0x080e..0x0816`, group hold, and stock gain
(`0x0204=0x0200`, `0x020e=0x1000`). Samsung's Vision exposure manager changes
frame length to about `0xbf9b`; iOS leaves the 30 fps mode value `0x0597`.
The full stock direct snapshot is
`work/stock-camera-test/reference-direct-final-main.raw`.

Indirect memory reads must use `0x602C=0x2000`, `0x602E=address`, then read
`0x6F12`. The earlier use of `0x6028/0x602A` selected the **write** pointer
and returned a misleading alternating `0x0220/0x84f8` on both kernels. With
the read pointer fixed, the iOS global microcode words at addresses
`0x918c,0x918e,0x9190,0x9192,0x9256` read back as
`0x0094,0x00f0,0x4ef8,0x0122,0x0026`, exactly the Samsung setA source.
The global 200-word burst is now sent as one I2C transaction, matching
Samsung's helper. An optional 250 kHz software-I2C probe (`stream 4`) with
stock gains produced the same zero CSIS frames as the slower path. This
rules out the tested I2C timing and setfile differences, but it does not
validate every word of microcode.

The iOS CSIS1 core (`0x4b01`), interrupt mask (`0x00fff07f`), PHY enable
(`0x1f`), D-PHY skew/settle values, powered sensor rails, MCLK readback,
QCH gates and PMU isolation now match the known Samsung setup values.
Nevertheless `FRM_CNT_CH0=0`, all packet IRQ sources remain zero and
`PHY_STATUS=0xf1` in 500 one-millisecond samples while the sensor's internal
counter advances 18 times. The remaining work is to prove why the physical
MIPI output does not leave stop state under iOS, especially any unobserved
clock or PHY routing state. Do not wire this probe into Camera.app yet.

The focused stock CSI core dump in
`work/stock-camera-test/reference-csis-core-main.raw` exposed a real VC1
field error: Samsung sets `ISP_CONFIG_CH1=0x000120ac`, with VC1 in bits 16+;
our probe had put `1` in bit 0. The iOS register and its shadow now both
read `0x000120ac`. The probe also resets the CSI core before programming the
D-PHY, matching Samsung's `csi_init()` then `phy_set()` ordering. Neither fix
yet produced a CSIS frame: `PHY_STATUS` remained `0xf1`, packet IRQs and
VC0 frame count remained zero. The sensor's internal test pattern (`stream 3`)
also showed no link activity with those fixes, so image-array exposure is
not the remaining blocker.

Samsung's `csi_hw_s_lane()` enables the data lanes before VC programming and
`csi_hw_enable()` enables the clock lane after the VC shadow update. The iOS
probe now follows that write order. Its live `PHY_CMN_CTRL` still reads
`0x1f` and `PHY_STATUS` stays `0xf1`; this sequencing change did not produce
the first frame. Avoid treating matching register readbacks as proof that
the physical MIPI path is live. The next diagnostic should capture the
Samsung and iOS CSI domain clock request/PHY power transitions during stream
without touching the known unsafe `SYSREG_CSIS + 0x504` register or blindly
cycling CSIS QCH gates (an earlier standalone gate probe panicked).

## Receiver DMA pattern verified (2026-11-10)

`tools/poke cam.s20.dma.test` captured a 320x240 RAW10 hardware pattern
under iOS: frame-end IRQ, all 153600 bytes changed, pixel values 0..1020,
no DMA errors and no guard corruption. Abort completion was acknowledged;
registers and the borrowed low-memory pool slot were restored/released.
This proves the downstream memory path only, not optical camera capture.

The test uses v5.3 WDMA1 at 0x170c2000 and TPG at 0x170c0000, refuses
active DMA owners or active/unknown SysMMU translation, and obtains its
32-bit address through MiniIOSurface's existing pool allocator. It never
guesses free physical RAM. If DMA does not acknowledge stopping, the slot
is retained and further tests refuse to run until reboot. Sensor rails,
CSI link, PHY and clock rates are untouched.

The Samsung clock-reference boot returned safely to iOS, but its normal
clock summary and Samsung CAL debug interface were unavailable. The log is
`work/stock-camera-test/reference-csis-clocks-main.raw`; do not describe it
as a successful active-clock comparison.

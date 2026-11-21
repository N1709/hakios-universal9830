# Temporary Samsung reference boot

The microphone was brought up by briefly booting the phone's saved Samsung
kernel with a small custom initramfs, capturing the hardware state while its
driver ran, then restoring the HackiOS BOOT image. The reusable artifacts are
in `work/stock-audio-test/`: `boot-mic-ref.img`, `build-mic-reference.py`,
`microot/init`, `capture.sh`, and the saved reference logs. This does not need
Android's system partition; the current `super` partition contains iOS.

## Repeatable procedure

1. Record hashes of the active HackiOS BOOT head and recovery image. Keep
   `out/x1slte-boot.img` (Samsung DT table magic `d7b7ab1e`) and
   `out/x1slte-recovery.img` available for restoration. The original phone
   partitions are backed up under `device/backup/20261105-203534/`.
2. Repack the saved Samsung kernel and DTB with a task-specific initramfs.
   The image builder must update the ramdisk size and Samsung boot header SHA1.
   The initramfs mounts proc, sysfs, devtmpfs and debugfs, supplies only the
   needed firmware and static utilities, and never exits.
3. The initramfs writes bounded logs to the dedicated super tail window used
   by the mic run, `super+0x1e9b04000`, after checking the partition size.
   Do not touch the iOS APFS window at `super+0` or its log at
   `super+0x1e8b04000`. A USB ACM gadget can stream the same log to the Mac.
4. From Download Mode, flash the temporary image to **BOOT only** with
   Heimdall. Let the reference kernel run its bounded capture, then have its
   initramfs reboot to Download Mode. Retrieve the log and restore the
   previously saved HackiOS BOOT image; verify the write and boot iOS.

For this phone, build the camera image with `python3
tools/reference-boot/build-camera-reference.py` after compiling
`camera-ref.c` as a static AArch64 binary at
`work/stock-camera-test/camera-ref`. The output is
`work/stock-camera-test/boot-camera-ref.img`. The known HackiOS restore image
is `out/x1slte-boot.img`; its recovery payload is already in the RECOVERY
partition. From Download Mode, flash with `heimdall flash --BOOT IMAGE
--stdout-errors`. The reference init writes its log at `super+0x1e9b04000`
(4096-byte block 2005764). Once iOS is back, retrieve it with
`ssh s20 'dd if=/dev/rufs_super bs=4096 skip=2005764 count=256
2>/dev/null' > reference-log.raw`. The log is separate from the loader's
`ulog` window. The current init also writes a rolling camera helper and
kernel breadcrumb at block 2006020 (512 KiB); retrieve it with the same
command using `skip=2006020 count=128`. If the reference kernel stalls or loops at the boot logo,
manually enter Download Mode and restore `out/x1slte-boot.img`; do not wait
for the initramfs watchdog indefinitely.

The mic run initially bootlooped when its test init exited. The initramfs must
remain alive until it explicitly reboots to Download Mode. The reference
kernel's debugfs regmap may show cached values; use `cache_bypass` when the
task needs real hardware registers, and limit direct MMIO reads to registers
verified safe for the active power domain.

## Optional isolation replays

Compile `camera-ref.c` with `CAMERA_XNU_SENSOR_REPLAY` to replay XNU's global,
PDAF mode, normal timing, gain and stream-on writes over Samsung hardware I2C
after the original observation window. Samsung retains V4L2 ownership and
power. The first run in `logs/camera/foundation-audit-20261110/` advanced the
CSI1 counter from 1 to 16 in the replay window. This tests a warm restart;
it does not validate XNU's cold power sequence, software I2C or verification reads.

Additionally defining `CAMERA_XNU_RECEIVER_REPLAY` enables a separate,
write-capable experiment in `camera-receiver-replay.h`. It first requires
sensor standby and SYSREG reset value 8, then applies the XNU PHY recipe,
replays the sensor, applies the CSI core plus PHY recipe, and replays again.
Its fixed write whitelist excludes clock, PMIC and DMA controls and the unsafe
SYSREG +0x504 address. Reads use fresh `/dev/kmsg` records, and setup errors
prevent sensor restart. The ordinary SFR dump remains read-only. These tests
change receiver state while Samsung owns it; V4L2 teardown and the reference
boot's return to Download Mode follow. Use the saved-BOOT restoration runner.

`CAMERA_XNU_GPIO_REPLAY` additionally requests GPP0-2/3 using Linux GPIO
line handles with open-drain emulation, checks ID 0x30a1, and routes the sensor
replay over software I2C. It restores only those two pin mux fields before
hardware I2C resumes. The successful run in
`logs/camera/gpio-replay-wait-20261110/` completed 376 transfers with no NACK or
I/O error and advanced CSI1 from 15 to 30. The first attempt in
`gpio-replay-20261110/` did not run because it checked standby too early.
The helper now waits up to 100 ms for sensor standby before taking the pins.

`CAMERA_XNU_POWER_REPLAY` requires the known front-camera PMIC values and pin
muxes, then tests reset/clock restart and a separate dedicated sensor-rail
cycle. It holds reset and MCLK low before disabling supplies and restores
saved values on every exit. Per-register PMIC results distinguish a failed
setup from a valid reception test. The 50-second boot watchdog remains; the
helper timeout is 25 seconds to accommodate the optional stages.

## Camera adaptation

The saved Artisan reference kernel disables KPROBES, FUNCTION_TRACER,
CMUCAL_DEBUG and DEVMEM (see `work/stock-audio-test/kernel-reference.config`).
The 2026-11-10 clock-trace attempt therefore captured no dynamic probes.
The supported alternative is Samsung's
`/sys/module/sec_debug/parameters/force_error` **`SFR ADDRESS`** command.
Its `sec_debug_test.c::simulate_SFR` reads one register with one argument;
an additional argument would write and must never be supplied by this helper.
`camera-ref.c` checks that SFR is supported and reads a fixed whitelist while
it owns an active camera stream. It excludes `SYSREG_CSIS + 0x504`.
`camera-init` saves the result lines from dmesg, the complete regulator summary
and the sub-PMIC's read-only register selector results.

Validated captures are `logs/camera/sfr-reference-20261110/` (30 SFRs) and
`logs/camera/power-reference-20261110/` (56 SFRs, 21 camera-PMIC controls).
Both observed clean VC0/VC1 frame start/end and restored iOS automatically.
The latter also confirms PLL_SHARED4's STABLE bit during the Samsung stream.
An XNU idle read with STABLE clear is not an active-clock comparison; the next
XNU build samples it while streaming before making further clock changes.

The earlier reference log confirms Samsung's kernel registered
`is_sensor1`, front SAK3J1, and CSIS-related devices during early boot.
It did not start a camera stream. The camera reference init now selects front
position 1 on `/dev/video102`, sets 1824×1368 RAW10, and calls Samsung's
`V4L2_CID_IS_S_SENSOR_SIZE` and `VIDIOC_S_PARM` for 30 fps, sets 1824×1368
RAW10, then calls `V4L2_CID_IS_S_STREAM` for five seconds. `VIDIOC_STREAMON` on the
buffer queue alone is insufficient: the sensor driver starts the CIS and CSIS
from this private control. The run captures CSIS clocks, interrupts and
Samsung camera logs while the sensor is active. To make the comparison more
decisive, capture CSIS1 D-PHY status,
`FRM_CNT_CH0` (`0x0500` on CSIS v5.3), packet interrupts, sensor registers,
clock gates and PMU state while streaming. The stock firmware files
`is_lib.bin` and `setfile_3j1.bin` are already extracted under
`device/firmware/vendor-firmware/`. Confirm the V4L2 request sequence and
firmware requirements before making the temporary image.

The first camera run on 2026-11-09 proved the restore and log path, but the
helper opened `/dev/video2` and failed with `ENOENT`. Samsung adds
`EXYNOS_VIDEONODE_FIMC_IS=100` to the internal video ID; sensor1 is therefore
`/dev/video102`, and the run's device list confirms that node exists. No
camera stream was started in that first run.

The second run on 2026-11-09 opened `/dev/video102`, selected the front
camera and read valid SAK3J1 EEPROM calibration. `VIDIOC_S_FMT` then failed:
Samsung logged `sensor mode(0x0@30fps) is not found`. The driver takes sensor
dimensions from `V4L2_CID_IS_S_SENSOR_SIZE`, not the V4L2 format structure.
The third helper version supplies that private control before `S_FMT`.

The third run accepted sensor selection and size, but `VIDIOC_S_PARM` failed
because Samsung's sensor driver insists on
`V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` for the frame rate ioctl, despite the
node's output capabilities. The fourth helper uses that type.

The fourth BOOT flash uploaded successfully, but the phone did not return to
Download Mode within 95 seconds and then cycled through boot logo and black
screen. The user manually entered Download Mode; HackiOS BOOT was restored and
iOS SSH returned. The protected log has only early camera registration, ending
at `cis_3j1_probe done`; no helper result or stream result was flushed. This
run cannot be counted as evidence of a camera frame. A separate rolling
breadcrumb log was added for the next run to preserve progress across stalls.

The fifth run also bootlooped and required manual Download Mode entry;
HackiOS BOOT was restored and iOS SSH returned. Its main log again ends after
early `cis_3j1_probe done`, before a helper result. The new breadcrumb was
written, but its unfiltered dmesg content filled the 512 KiB window before
the helper started, so it did not identify the failing ioctl. The logger now
filters for camera, sensor, CSIS and panic messages before writing that
window. Do not infer that Samsung streamed a frame from these runs.

The sixth run added a 4 KiB checkpoint per ioctl at block 2006276. The
protected pages prove `S_INPUT`, sensor-size, 30 fps and `S_FMT` all returned;
the last completed checkpoint is **before** `V4L2_CID_IS_S_STREAM`. The
normal scenario enters Samsung's ISP chain during that control, and a full
Android camera HAL and ISP userspace are absent from the minimal ramdisk. The
next probe selects `SENSOR_SCENARIO_VISION` (bit field at 26), whose device
tree power sequence omits `S2MPB02_BUCK2`, and which Samsung's driver marks as
stand-alone so `is_sensor_start()` bypasses `is_itf_stream_on(ischain)`.
This is a diagnostic stream attempt, not an Android Camera.app equivalent.

Further tests on 2026-11-09 narrowed the path:

- Vision `S_INPUT` initially failed with an I2C NACK. Its power sequence omitted
  `S2MPB02_BUCK2`; the normal scenario enabled that rail.
- The normal scenario accepted two ION DMA buffers and queued both, but hung
  at queue `VIDIOC_STREAMON` in this minimal initramfs. The full ISP chain is
  therefore not a useful sensor-only reference without more Android userspace.
- Enabling BUCK2 before the Vision test made `S_INPUT`, mode setup and private
  `V4L2_CID_IS_S_STREAM` return successfully. `G_STREAM` remained 1 for five
  seconds; the helper stopped the stream and restored BUCK2. Raw evidence is
  `work/stock-camera-test/reference-vision-buck2-main.raw` and its checkpoints.
  This proves sensor initialization, not an optical frame.
- A `debug_csi=1` run captured Samsung's CSIS1 v5.3 register baseline just
  before sensor stream-on: `CSIS_CMN_CTRL=0x4b01`, `CSIS_CLK_CTRL=0x10`,
  `CSIS_INT_MSK0=0x00fff07f`, `FRM_CNT_CH0=0`, and `PHY_STATUS=0xf1`.
  The zero count is expected at that instant. Evidence is
  `work/stock-camera-test/reference-vision-debug-main.raw`.

In the Vision run without DMA buffers, the named CSI interrupt count rose only
from 1 to 2 in the first second, then remained at 2 for the next four seconds
despite `G_STREAM=1`. Queuing two ION buffers in Vision mode also succeeded but
had the same count. `debug_irq=2` then identified those two interrupts as one
clean VC0/VC1 frame start and one clean frame end, with zero CSI error bits.
`V4L2_CID_IS_G_MIPI_ERR` remained 0 for five seconds. Evidence is
`reference-vision-queued-main.raw` and `reference-vision-irq-detail-main.raw`
under `work/stock-camera-test/`. The physical link can carry a frame under
Samsung's kernel, but this minimal Vision setup does not sustain 30 fps.

Samsung's CSIS1 baseline also exposed a concrete iOS setup mismatch:
`CSIS_CMN_CTRL=0x4b01` includes deskew level 2 (`0x4000`) and VC interleave
mode 2 (`0x0800`). The iOS bounded probe set four lanes but omitted those two
fields. It now sets them and `CSIS_CLK_CTRL` bit 4 before starting the sensor.
The live test still reported zero iOS CSIS frames, so these fields alone are
insufficient. Camera.app still shows a synthetic preview.

One final reference run read the SAK3J1 frame counter directly via I2C and
captured the PHY SFR dump. Its counter reached 1 and stayed at 1 for the full
five seconds, consistent with the single clean CSI frame. The incomplete
Vision setup therefore also stops the *sensor* after one frame; adding DMA
buffers did not fix it. The PHY dump confirmed clock and lane registers in
Samsung's kernel. The iOS probe was adjusted to mirror Samsung's skew-delay
writes to clock and data lanes, but its live CSIS count remained zero. Raw
evidence is `work/stock-camera-test/reference-vision-phy-main.raw`.

The later reference helper reads extra SAK3J1 output and timing registers
while streaming. `reference-direct-final-main.raw` under
`work/stock-camera-test/` records the stock values. The helper's first
attempt at indirect microcode readback mistakenly used the write-pointer
registers; use `0x602C/0x602E` for future reads. Its `sensor_write16()`
returns zero on a successful one-message `I2C_RDWR` transfer. The reference
helper source now has both corrections. The iOS probe with the corrected
read pointer matches the five sampled Samsung setA microcode words.
The focused CSI core rows are saved in `reference-csis-core-main.raw`; they
include VC0/VC1 active and shadow settings, LRTE and PHY enable immediately
before the reference sensor streams. The VC1 value is `0x000120ac`.

## GPS and cellular adaptation

The same boot and restore mechanism can capture kernel driver state for GPS
and modem work. Each run needs its own firmware, userspace trigger and
read-only telemetry. Early driver registration alone does not prove that GPS
has a fix or that the modem has registered on a network. Record what actually
ran and the hardware state it produced before copying settings into iOS.

For each new subsystem, keep the same BOOT image builder, super log offset,
size guard, independent reboot watchdog and HackiOS restore image. Replace
only the static test helper, its firmware files and init script's bounded
telemetry. For GPS, record GNSS power, clocks, firmware load, serial or IPC
traffic and a real fix attempt. For cellular, record modem power and reset,
firmware load, IPC handshake, SIM state and network registration. Save each
run's image SHA256, raw log, exact trigger sequence and restore result; a
probe message or an open device node alone is not a functional pass.

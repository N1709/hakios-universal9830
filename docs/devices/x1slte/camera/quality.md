# S20 camera quality: stock versus iOS (2026-11-14)

Research branch: `camera-improve`, based on accepted local master `8d584cc0`.
Worktree: `work/hackios-camera-improve`. No phone commands, writes, capture,
service restart, flash or reboot were performed. The workspace phone lock was
held by B for fingerprint work. Existing fingerprint edits were left alone.
This is an offline investigation, **not a stock-quality implementation**.

## Follow-up: typed tuning decoder

The 65-key layout is now recovered and validated across all 122 local ISP entries
(7930 named blocks). Typed CCM, GRAS LUT and BLC fields and source-based scenario
labels are decoded offline. See [the format report](tuning-format.md)
for offsets, evidence, tools and remaining conversion limits. The initial findings
below describe the earlier investigation stage; its serializer-recovery task has
now advanced. No runtime tuning has been installed.

## Hardware and the pixel-technology question

This project's x1slte phone is SM-G980F / Exynos990. Existing successful sensor
identification records the rear main as S5K2LD, ID 0x20cd, revision 0xA301.
Samsung lists S20/S20+ main wide as **12 MP, Dual Pixel AF, f/1.8, OIS**, and a
separate 64 MP camera; do not apply S20 Ultra's 108 MP processing assumptions.
Source: https://www.samsungmobilepress.com/media-assets/galaxy_s20?tab=specs .
Dual Pixel AF, Bayer colour sampling, sensor binning, HDR and ISP noise reduction
are separate parts of the pipeline. A visible grid or dark band alone does not
identify which part is wrong. Earlier comments describing full mode as inherently
non-Bayer are not sufficient evidence: later experiments obtained plain Bayer
with HDR disabled, but still saw bands (tools/camera/REAR-CAMERA.md).

The repository calls the other rear modules S5K2LA and S5KGW2. Their exact optical
roles must be verified against this phone's DT/module mapping before enabling
lens selection; the presence of a firmware filename is not proof of a live module.
Front SAK3J1 quality is outside this change's scope.

## Findings supported by source and binary analysis

### Sensor tables already match

`tools/camera/audit-2ld-tables.py` compares actual integer triplets, including
operation tags and widths, against the locally available Samsung/ExtremeXT
`is-cis-2ld-setA.h`. Results on the branch baseline:

| Port table | Records | Exact match |
| --- | ---: | --- |
| Global_A3 | 7354 | yes |
| 2016x1512 / 30 fps | 417 | yes |
| 4032x3024 / 30 fps | 417 | yes |

Selected page 0x4000 values (16-bit writes):

| Register | Binned | Full | Meaning / boundary |
| --- | --- | --- | --- |
| 0x0340 | 0x5824 (22564) | 0x2f80 (12160) | frame lines |
| 0x0342 | 0x08e0 (2272) | 0x1090 (4240) | line clocks |
| 0x034c / 0x034e | 2016 / 1512 | 4032 / 3024 | output dimensions |
| 0x0900 | 0x0122 | 0x0111 | mode's binning configuration |
| 0x021e | 0 | 0x0100 | port overrides full mode to 0 for single exposure |
| 0x0b30 | 0 | 0x0100 | table value, BEFORE stock post-mode logic |
| 0x0b04 | 0x0001 | 0x0001 | preserve write width; not two independent controls proven here |
| 0x0114 | 0x0201 | 0x0201 | interface configuration |

A matching table does not establish a matching stream: stock performs additional
mode, calibration, timing and ISP operations afterward.

### Stock low-noise behavior differs from our optional plain setting

In Samsung `is-cis-2ld.c`, `sensor_2ld_cis_mode_change()` unconditionally writes
page 0x4000 then **8-bit 0x0e0b=0**, disabling auto exposure bracketing.
`cis_get_lownoise_supported()` returns true only for 4032x3024 and 4032x2268
at 24/30 fps, with AEB off. Those modes then select **LN2**, not LN off.

`cis_set_lownoise_mode_change()` uses group hold and writes:

| Mode | 0x0b30 | coarse exposure margin |
| --- | --- | ---: |
| LN off | 0x0100 | 36 lines |
| LN2 | 0x0101 | 72 lines |
| LN4 | 0x0102 | inspect whole path before use |

For 4032x3024 / 30, conditional `CAMERA_REAR2` code also writes
0x0a7a=0x0065 and 0x0a7c=0x0010 for LN2, versus 0x2a30 / 0x0010 for LN off.
Whether that macro was enabled in the exact shipping kernel is NOT established.

The iOS `cam_s20_rear_plain` experiment instead combines AEB off with LN off,
is disabled by default, and previously produced noise alongside a bus-speed
change. That experiment does not isolate AEB, LN2 or bus reliability.
**Do not enable LN2 in binned mode:** stock's support predicate excludes it.
Do not infer that the binned stock path writes LN off: its unsupported branch
updates software state without calling the low-noise setter.

### Firmware tuning container decoded

These local binaries come from `device/firmware/vendor-firmware/`; their exact
provenance must be tied to the running stock build before claiming parity.
The new inspector follows `hardware/is-hw-control.h` and
`parse_setfile_header()` / `parse_setfile_info()` in `is-hw-control.c`.
No firmware or payload bytes are committed or exported by the inspector.

All four local containers are v3: magic 0x12345679, version ISV8, revision HE01,
design bits 0x608 = 3AA/ISP + MCSC + VRA. The scenario table is IP-major;
entry offsets are relative to the payload base, not the file start.

| File | Bytes | Scenarios | 3AA/ISP entries | MCSC entries | VRA entries | Payload base |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| setfile_2ld.bin | 12565720 | 70 | 39 | 2 | 2 | 1224 |
| setfile_3j1.bin | 9987792 | 71 | 31 | 1 | 2 | 1164 |
| setfile_2la.bin | 8055060 | 70 | 25 | 1 | 2 | 1104 |
| setfile_gw2.bin | 8699300 | 70 | 27 | 1 | 2 | 1120 |

In these files each 3AA/ISP entry is 322112 bytes; MCSC is 972 and VRA 60.
Every declared entry fits in its container. The inspector reports each entry's
SHA-256, absolute offset, size and referencing scenarios. Scenario numbers are
not yet mapped to camera use cases; do not pick entry zero as universal tuning.

Rear-main file SHA-256:
`c6b9792d6150f64f48fb179e313a4883c1f4596fcec6265b02f102d4a140e2e7`.
Full four-file metadata: workspace `build/camera-improve-setfiles.json` (local).

### ISP library disassembly establishes the next decoding targets

`is_lib.bin` is a raw binary, not ELF; size 4717472 bytes, SHA-256
`e047faff00230a58dd547c6fe0503d91223f3a888730dd550b2dedbedf7d99b5`.
Capstone AArch64 disassembly plus decoded ADRP/ADD references located the tuning
reader. Addresses below are **file offsets**, not live kernel addresses.
`include/is-binary.h` describes CDH 128 KiB plus VRA 768 KiB before DDK;
DDK registration stubs appear at 0xe0040 and 0xe0080, consistent with that layout.
No binary was executed.

At 0x152c60 the reader checks magic **0x3e671996** at object +0x190, reads its
size at +0x194 and starts serialized data at +0x198. At 0x152d40 it iterates
65 known keys, calls per-key readers and checks consumed size against expected
size. Failed validation prints the strings at 0x35f7c0 / 0x35f800; successful
validation references 0x35f760. The first rear-main payload begins with the same
magic. Do not confuse the in-memory object's +0x190 with a file entry offset:
the creation code at 0x15237c copies the file data into object +0x190.

The name-to-size dispatch around 0x1528ec identifies:

| Key | Name reference | Size assigned by dispatch |
| --- | ---: | ---: |
| ccmTuning | 0x35f4f0 | 0x20f4 |
| grasTuning | 0x35f500 | 0xa3d4 |
| grasLutTuning | 0x35f510 | 0x67a4 |

These are block sizes, **not offsets within setfile payloads**, and not evidence
that any float-looking sequence is a usable matrix. Other dispatch names include
`dmsc`, `TnrTuning`, `reconstructionTuning`, `hqdns`, `sharpnoise`,
`oetfRgbGammaTuning`, `wbg`, `dns_wbg`, and `exposureMngrTuning`.
Strings also show separate AWB/LSC calibration and module calibration inputs.
These references identify decoding targets; they do not prove how a particular
stock camera scenario uses each block. The 65-key reader and its serializer must
be decoded before converting CCM or GRAS data to RWCISP02.

## Why current images cannot yet match stock

The actual port path is sensor -> C-PHY/CSIS0 -> WDMA2 packed RAW10 ->
/dev/redwood-camera -> CPU ISP -> NV12 -> iOS / software H.264.
Samsung's ISP library and hardware processing are not in this path.

* Default rear mode remains 2016x1512. Preview uses one RGB value per 2x2 Bayer
  cell: 1008x756 independent colour cells before scaling. A larger advertised
  buffer does not recover lost detail. Full mode remains opt-in because recorded
  tests showed row ~1912 and 32-row structures plus watchdog resets.
* Software ISP has basic demosaic, grey-world AWB, CCM, shading and gamma;
  source explicitly records no noise reduction. Stock tables alone cannot add
  temporal processing, calibrated noise models or autofocus/PDAF/OIS integration.
* `tools/camtuning.py` accepts QTI Chromatix, not Samsung ISV8. Current rear colour
  correction was measured from a screen chart, and rear shading was recorded as
  flat, not decoded stock tuning. A filename such as `camera-s5khm2sd.isp` is a
  legacy logical name, not proof that this phone has that sensor.
* Dark offsets currently use empirical gain/exposure tables and full-mode band
  compensation. These need controlled covered-lens measurements, not scene
  minima (which remove real shadow detail). 0x0b04 semantics remain insufficiently
  established to justify the existing comment's claimed SMIA interpretation.
* The reader mutates global black/band state while the draw thread can still
  render the preceding frame. This is a source-supported race candidate, not a
  demonstrated cause of every visible glitch. Fix via per-frame immutable ISP
  state and buffer ownership, then measure CPU/render performance.

## Next implementation gates

1. Decode the library's serializer / 65-key ordering, validate exact block sizes
   and consumed lengths on all 39 rear-main entries. Resolve CCM coefficient
   types, scale, interpolation axes, matrix direction and illumination triggers;
   resolve GRAS grid geometry, channel order and module-EEPROM calibration.
   Only then write a Samsung-specific converter. Do not guess float offsets or
   reuse Qualcomm lux-index assumptions.
2. Fix frame-owned black/band state with host RAW fixtures and a thread-race
   check. Keep the front's accepted tuning behavior intact.
3. Isolate the sensor sequence comparison at the known-good I2C speed. AEB off
   alone first; LN2 only in supported full modes with matching timing/margin,
   retention/revision behavior and conditional register semantics understood.
   Source changes must be mirrored into the actual XNU build tree before build.
4. With phone ownership and user cooperation, capture bounded RAW10 fixtures
   at fixed exposure/gain: covered lens, neutral flat field and colour/detail
   chart. Record mode/readback, frame dimensions/stride, CFA, capture timing and
   sensor revision. Avoid long full-resolution dumps that previously triggered
   watchdog resets. Stock-versus-port comparisons require matched scene,
   illumination, exposure, focus and actual module, not screenshots alone.
5. Validate black stability, clipping, row/column periodicity, colour error,
   corner shading, resolved detail, noise, FPS and temperature. Full-quality
   parity remains unproven until paired optical tests pass.

Phone ownership must be rechecked before capture/service changes. User approval
is required before **any reboot**, including a temporary stock/reference boot.
No approval is needed to continue offline decoding. Do not deploy firmware or
new register settings merely because a parser passes.

## Reproduce offline

```sh
python3 tools/camera/test_samsung_setfile.py
python3 tools/camera/inspect-samsung-setfile.py /local/vendor-firmware/setfile_2ld.bin
python3 tools/camera/audit-2ld-tables.py /local/samsung/is-cis-2ld-setA.h
```

Validation performed: five synthetic parser tests (valid v2/v3, truncation,
out-of-range payloads/tables/scenario references, excessive v2 IP count); all four real containers;
exact comparison of all three imported rear-main tables. No runtime image,
boot/profile or kernel source changes were made, so no build or on-phone quality
claim follows from these checks.

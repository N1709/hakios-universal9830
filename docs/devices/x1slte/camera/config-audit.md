# S20 camera: deeper reverse engineering and configuration audit

2026-11-14, `camera-improve`. Continues [the tuning layout report](tuning-format.md).
No phone access, flash, service restart or reboot was performed. Runtime camera
sources/defaults are unchanged. The new code is offline decoding and a mathematical
reconstruction of one isolated stock colour helper.

## Confirmed colour-helper mathematics

The function at **DDK file offset 0x2b4938** is named by an error string
`ConvertVectorsCcm9ToMatrices`. Its inputs include 27 signed vector coordinates
and a signed row-major 3x3 transform. It derives **nine colour sectors**, each
with **six packed coefficients**, not one universal 3x3 colour matrix.

`tools/camera/samsung_ccm_math.py` reconstructs the helper. It performs:

1. Per-sector reconstruction of a 3x3 transform from adjacent vector vertices,
   with 256 as the unity coordinate and signed 16-bit intermediate storage.
2. Composition with the input 3x3 transform, keeping two columns in column-major
   order (three rows per column).
3. Signed nearest rounding, ties away from zero, division by **64**, and signed
   16-bit output storage. Q8 x Q8 / 64 yields **Q10** output.

The third-column representation and subsequent processing must be understood at
the consuming stage before using these packed coefficients in the port. The
helper's output alone is not a complete stock colour transform.

A synthetic identity set of colour vertices and a Q8 identity matrix produces
`[1024, 0, 0, 0, 1024, 0]` for every sector. A non-identity input matrix preserves
its first two columns, multiplied by four, for those synthetic identity vertices.
This establishes the Q8-to-Q10 relationship beyond merely observing row sums.

`verify-samsung-ccm9.py` checks this reconstruction against the original helper in
Unicorn AArch64 emulation. The DDK hash is pinned. The library is read-only, only
the helper's page is executable, and a control-flow guard restricts execution to
0x2b4938..0x2b4bdc. A scratch/stack mapping and instruction limit bound each call.
No HAL/firmware initialization, host calls, MMIO or device interfaces run.
This is a new isolated emulation step; earlier research used static tracing only.

Validation results:

| Input set | Exact matching cases |
| --- | ---: |
| Deterministic random full signed-16-bit inputs | 200 |
| Rear-main 2ld: all 39 x 34 records x 3 vector sets | 3978 |
| 3j1: all 31 x 34 x 3 | 3162 |
| 2la: all 25 x 34 x 3 | 2550 |
| gw2: all 27 x 34 x 3 | 2754 |
| Total | **12644** |

These stock-data cases supply each record's PreCCM and its High/Low/Ccm9 vector
sets to the helper. They validate arithmetic with real input values; they do not
prove the running stock pipeline passes precisely those pairs at every stage.

`inspect-samsung-tuning.py --values` now includes
`ccm9_helper_with_PreCCM_q10` alongside the raw signed fields. This name deliberately
identifies the calculation, rather than claiming it is the final stock CCM.

## Main shading grids located and decoded

The earlier 26532-byte GRAS LUT block contains residual/reference/difference data.
The separate **41940-byte grasTuning block contains the main positive grids**.
SDK serializer **0x1ff570** names the fields and uses unsigned 16-bit reads:

| grasTuning offset | Field | Shape |
| --- | --- | --- |
| 0x0000 | uBypass | u32 |
| 0x0004 | CGrasDisable | u16 |
| 0x0008 | aperturesInUse | u32 |
| 0x0014 | GainIncrement | u16 |
| 0x0016 | bBiQuadraticEstimation | u16 |
| 0x0018 | uSaturationSatx | u16 |
| 0x08f0 | lutGridGainAwbAshCord | 8 x u16 |
| 0x0900 | lutGridGainIRAwbAshCord | 8 x u16 |
| 0x0910 | lutGridGainIRLowAwbAshCord | 8 x u16 |
| 0x0920 | lutGridGainGolden | 4 x 25 x 33 x u16 |
| 0x22e8 | lutGridGain[0] | same |
| 0x3cb0 | lutGridGain[1] | same |
| 0x5678 | lutGridGain[2] | same |
| 0x7040 | lutGridGain[3] | same |
| 0x8a08 | lutGridGain[4] | same |
| 0xa3d0 | optionBrightnessMeasure | u32 |

The serializer loops over five main grids, each containing 3300 samples.
Separate reader/resampler code at 0x22c3c0 establishes the 33x25 target dimensions
and 825-sample plane strides; hardware packing at 0x22b590 reads planes at byte
strides 0x672 (=825 x 2). Plane-to-CFA colour mapping remains unresolved.

Rear-main entry zero's main grids have centres around 128; the first plane's
first corner ranges from 696 to 889 in the five selected grids. This suggests a
128-centred scale but does NOT establish an independently applicable gain map.
The golden grid's corresponding corner is 752, also with centre 128.

The processor at 0x2b79a8 interpolates grids using scene/white-balance-related
coordinates with weights summing to 256. At 0x2b7e30 onward, it computes ratios
with **selected grid shifted left by 10, divided by golden/reference samples**,
then combines them with generated/calibrated grid values and other weights.
`GetGridGain` at 0x2b7870 calls the LSC grid calculator and supplies 1024 defaults.
Later code adds focus-position and colour-temperature-dependent differences.
Therefore:

* Do not apply the main grid and the calibration grid independently as two full
  shading corrections; stock explicitly normalizes against a golden/reference.
* Do not interpret the eight `AwbAshCord` entries as Kelvin temperatures. The
  local values are not even a monotonic eight-element temperature axis; source
  control paths select subsets/branches. That mapping still needs tracing.
* A 33x25 grid could be reduced to the software ISP's 17x13 grid, but that is not
  sufficient for correctness without colour order, crop coordinates, gain scale,
  golden/reference normalization and module calibration.

The decoder now reports six main grids with per-plane min/max, centre and corner
values. `--values` includes the full unsigned arrays. Firmware-derived reports
remain in workspace `build/camera-re/`, outside git.

## Concrete port-versus-stock configuration differences

Reproduce the source audit with `audit-s20-controls.py SAMSUNG_CIS_DIRECTORY`.
It records hashes and refuses to repeat manually reviewed findings against
changed port source. Findings describe the local Samsung source, not an observed
live stock kernel on this phone.

| Item | Samsung source | Current port | Interpretation |
| --- | --- | --- | --- |
| Group hold | `USE_GROUP_PARAM_HOLD (0)` compiles helper to a no-op | Rear exposure writes 8-bit 0x0104=1 then 0 | Actual sequencing mismatch; investigate before changing defaults |
| Normal exposure quantum | Coarse integration rounded down to multiples of 2 | No rear even-line rounding | Odd requests violate the source's normal-mode quantization |
| Minimum coarse exposure | 16 lines | 16 lines | Already matches; do not report it as missing |
| Normal margin | 36 lines | 40 lines | Port is conservative here, not a demonstrated image-quality bug |
| LN2 constraints | Minimum 32, step 4, margin 72 | Full-mode LN2 not enabled; shared step/margin path | Cannot enable LN2 correctly with a one-register change |
| LN4 constraints | Minimum 64, step 8, margin 108 | Not implemented | Not a drop-in replacement for binned mode |
| Analog gain range | 0x20..0x200 (1x..16x) | Same | Matches |
| Rear runtime I2C errors | Register writes check return values | Exposure loop discards every return value | Software can tag a requested exposure as applied after a failed write |
| Long exposure shifting | Binned mode starts FLL/CIT shifting above 80000 us | No equivalent paired shifting control path | Missing stock behavior; distinguish it from simple register overflow |
| Rear mode descriptor | Physical path is C-PHY / 3 trios | Generic built-in descriptors say 4 lanes, D-PHY, settle 9 | Metadata/config mismatch; actual rear path configures C-PHY separately |

The existing `cam_s20_rear_plain` option combines AEB-off with LN-off. It is not
an isolated AEB experiment. Stock's AEB-off write does not prove AEB is currently
active in the port: the actual register readback is needed. Similarly, the
source's disabled group-hold macro does not prove our hold writes cause the bands.

### Timing metadata

Both imported Samsung rear PLL descriptions specify **1547000000 Hz**. The port's
built-in rear descriptors both contain **1537927680 Hz**, and `ios_line_ns` is
computed from the nominal 30-fps label instead of the PLL and line length.

| Mode | Line clocks | Frame lines | PLL-derived line ns, floor | Port line ns | PLL-derived fps |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2016x1512 | 2272 | 22564 | 1468 | 1477 | 30.176294 |
| 4032x3024 | 4240 | 12160 | 2740 | 2741 | 30.004810 |

This affects exposure-time metadata and calculations such as anti-flicker timing.
It does not prove that physical sensor timing is wrong: the sensor receives the
matching Samsung register tables. Correct metadata should follow the actual
PLL/line timing; do not tune image gamma to hide a timing error.

### Error handling and frame attribution

The rear loop clears `cam_s20_exp_pending` before issuing writes, then ignores
page-select, hold, frame-length, exposure, gain and release failures. Separately,
`s20_frame` updates its two-frame request history before that physical transaction
is acknowledged. The history is a fixed-delay model, not proof of the sensor's
applied values. This is particularly relevant when black correction depends on
reported gain and exposure. A proper fix needs acknowledged applied state, a
bounded retry/error policy and frame-delay verification; simply retrying I2C in
an unbounded loop would create a worse camera stall.

## Firmware provenance warning grounded in local evidence

The adjacent `device/firmware/vendor-build.prop` identifies:

* product fingerprint `samsung/x1sxxx/x1s:11/RP1A.200720.012/G981BXXSNHYB1:user/release-keys`
* incremental build `G981BXXSNHYB1`, Android 11.

This identifies a **G981B donor property artifact**, not the target G980F build.
The directory association alone does not establish that every binary was acquired
from that exact build, but it prevents treating these files as verified original
G980F stock values. Hash each binary, retain acquisition provenance, and compare
with the target's own vendor/DDK/setfile/module calibration before declaring
parity. Shared Exynos990/sensor identity makes these useful research references;
it does not erase firmware-version or module-calibration differences.

## What is ready, and what is next

Ready: offline typed main-grid decoding; source-level control/timing audit;
CCM9 arithmetic reconstruction verified against the isolated stock helper;
**24 host unit tests** plus the 12644 emulation comparisons and all 122 real
entries decoded with main-grid summaries.

Still required before applying stock colour/shading values: complete Build-stage
composition of High/Low/Ccm7/Ccm9 transforms, scene/brightness interpolation and
colour-sector selection; calibration-to-CFA grid mapping and exact crop/gain
representation; target firmware provenance. The frame-owned black-state race is
also still open. No claim of stock image quality or deployment follows from these
host checks.

The next narrowly scoped runtime fixes are even-line exposure normalization,
correct timing metadata and acknowledged exposure state. Keep group-hold/LN/AEB
experiments separate, preserve the accepted front path, mirror tracked kernel
changes into the correct build tree before compilation, and obtain user approval
before any reboot.

```sh
python3 tools/camera/test_samsung_ccm_math.py
python3 tools/camera/test_samsung_tuning.py
python3 tools/camera/test_samsung_setfile.py
python3 tools/camera/audit-s20-controls.py /local/samsung/cis
# Optional isolated emulation: Python unicorn required.
python3 tools/camera/verify-samsung-ccm9.py /local/is_lib.bin /local/setfile_2ld.bin
```

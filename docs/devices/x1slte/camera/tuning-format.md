# Samsung ISV8 tuning: recovered layout and typed fields

Follow-up on `camera-improve`, 2026-11-14. This records offline results after
[the initial camera comparison](quality.md). Firmware stays outside
git. No phone access, service changes, flashing or reboot occurred.

## Latest follow-up

Main positive shading grids are now decoded, CCM9 helper mathematics is verified
against isolated AArch64 emulation, and control/timing mismatches are audited.
See [the deeper configuration report](config-audit.md). The sections
below retain the earlier layout-discovery stage; the candidate CCM scale and
unresolved main-grid statements have since advanced as described there.

## What is now reproducible

The DDK's 65-key order and size dispatch have been recovered, not guessed from
float patterns. The resulting metadata is in
`tools/camera/samsung-isv8-schema.json`. Its 65 sizes total **322097 bytes**,
exactly the serialized-size field in each local ISP tuning entry.

`recover-samsung-ddk-schema.py` independently regenerates the schema from the
known `is_lib.bin` hash. It uses Capstone and a bounded symbolic trace of the
initializer at 0x1523cc. Only register moves, address arithmetic, comparisons,
branches and symbolic stack/table accesses are allowed. The one string-compare
call is modeled; the zero-initializer call uses fresh symbolic state. It cannot
call firmware functions, execute native firmware or access hardware. A different
binary hash, instruction, branch target or external call is rejected.

The on-disk entry layout is:

| Region | Offset | Size |
| --- | ---: | ---: |
| magic 0x3e671996, little-endian u32 | 0 | 4 |
| serialized byte count, u32 | 4 | 4 |
| 65 blocks in SDK key order | 8 | 322097 |
| zero alignment padding | 322105 | 7 |
| total entry | 0 | 322112 |

There is **no per-block padding**. Some blocks have odd sizes, so later blocks
can begin at unaligned offsets. The decoder must use packed little-endian reads.
The schema recovers block boundaries, not all fields inside all 65 blocks.

`inspect-samsung-tuning.py` validates container tables, entry lengths, magic,
serialized size and zero trailing padding, and reports absolute file offsets
and hashes for every block. It also decodes the typed CCM, GRAS LUT and BLC
fields described below. `--values` includes signed arrays in the local report;
there is no runtime converter or installer.

Validation against all four local files succeeded:

| File | ISP entries | Named blocks checked |
| --- | ---: | ---: |
| rear main 2ld | 39 | 2535 |
| 3j1 | 31 | 2015 |
| 2la | 25 | 1625 |
| gw2 | 27 | 1755 |
| total | 122 | 7930 |

This establishes a consistent layout across these files, not exact compatibility
with the phone's shipping firmware. Provenance and runtime use remain open.

## Rear-main block offsets

Offsets here are **relative to an ISP entry**, not the file or library base.
The full metadata schema records every block; these are immediate quality targets.

| Block | Entry offset | Bytes |
| --- | ---: | ---: |
| blcStrp | 0x28cdc | 384 |
| blcDns | 0x28e5c | 384 |
| ccmTuning | 0x29648 | 8436 |
| grasTuning | 0x2b73c | 41940 |
| grasLutTuning | 0x35b10 | 26532 |

The initial file's payload base is 1224. For example, entry zero's CCM starts at
`1224 + 0x29648`. Use the container table for every subsequent entry, never a
fixed whole-file stride assumption.

## CCM fields: more than one 3x3 matrix

DDK offsets are file offsets in the hash-pinned library. The CCM callback at
0x2ad1c8 transfers 0x20f4 bytes directly. The SDK field serializer at 0x1fd508
names the top-level fields; its child at 0x1ea9d4 establishes signed 16-bit
coefficient/vector loads and record dimensions.

| CCM offset | Type / interpretation |
| --- | --- |
| 0..3 | u8: isEnabled, useGMapCCM, enableSaturatedInputInterpolation, useSignedOutput |
| 0x08 | 6 x u16: awbLowlightCcmCord |
| 0x14 | 6 x u16: awbCcmCord |
| 0x78 | first scene record, outdoorCcm |
| 0x16c | shadyCcm |
| 0x260..0x724 | six indoor records |
| 0x818 | lowLightCcm |
| 0x90c..0xdd0 | six low-light records |
| 0xec4 | darknessCcm |
| 0xfb8 onward | face records, then three flash records |

There are **34 records of 0xf4 bytes** ending at 0x20e0. Each contains:

| Record offset | SDK name | Typed elements |
| --- | --- | --- |
| 0x00 | PreCCM | 9 x i16 |
| 0x12 | OuterCCM | 9 x i16 |
| 0x24 | VectorsHigh | 27 x i16 |
| 0x5a | VectorsLow | 27 x i16 |
| 0x90 | VectorsCcm7 | 21 x i16 |
| 0xba | VectorsCcm9 | 27 x i16 |

Four bytes at the record end are not decoded yet. Scalar/threshold fields outside
these rows are only partially decoded. The presence of identity matrices with
256 on the diagonal and rows summing to 256 supports a **candidate unity scale
of 256**. This is an inference, not a validated final RGB transform. The vector
stages, interpolation, AWB inputs, matrix composition and ISP range must be
traced before exporting to the port's one-matrix CCM representation.

In rear-main entry zero, many PreCCM/OuterCCM fields are identity, while the shady
and darkness PreCCM differ. Copying an identity PreCCM as the sensor's entire
colour correction would discard other stock transformations.

Across all 39 rear-main entries there are **two distinct CCM block hashes**.
This is evidence of scenario-dependent tuning, not 39 independent colour models.

## GRAS LUT fields: these are not four illumination gain maps

The class name is `ISP_GrasLutHandler_33_25`. Callback 0x22c8f4 passes object+8
to the field reader at 0x19ec10. Its serializer at 0x200a48 names and types:

| Block offset | SDK name | Type |
| --- | --- | --- |
| 0 | tableSize | u16 (local value 6600) |
| 2 | lut | 3300 x i16 |
| 0x19ca | lutRef | 3300 x i16 |
| 0x3392 | lutActPosDiff | 3300 x i16 |
| 0x4d5a | lutColorTempDiff | 3300 x i16 |
| 0x6722 | struct alignment | 2 bytes |
| 0x6724 | lutWeight | 16 x u32 |
| 0x6764 | lutTh | 16 x u32 |

3300 equals 33 x 25 x 4, consistent with four colour planes on the named grid.
The plane order, coordinate order and residual/gain encoding are unresolved.
Negative signed samples and separate reference/actuator/temperature differences
rule out treating the whole block as four ordinary positive gain maps.
The main `grasTuning` block and module calibration still need decoding.

All rear-main entries share the same GRAS configuration hash and the same GRAS
LUT hash. The decoder records this; it does not replace the port's flat shading
with unverified numbers.

## BLC fields: a direct lead for the green/purple shadows

The SDK serializer at 0x1ee55c establishes the following 384-byte layout, shared
by blcStrp and blcDns. Signed loads distinguish offsets from unsigned gains.

| Offset | SDK field | Type/capacity |
| --- | --- | --- |
| 0 / 1 | isEnabled / applyDgain | u8 / u8 |
| 2 / 4 | pixelMaximalLimit / pixelMinimalLimit | i16 / i16 |
| 8 | selectOffsetType | u32 |
| 0x0c | offsetsTableFixed | 4 x i16 |
| 0x14 | offsetsAfterGainTableFixed | 4 x i16 |
| 0x1c | gainsTableFixed | 4 x u16 |
| 0x24 / 0x28 | noiseIndex_length / values | u32 / up to 9 x float |
| 0x4c / 0x50 | totalGain_length / values | u32 / up to 9 x u32 |
| 0x74 / 0x78 | bv_length / values | u32 / up to 9 x i32 |
| 0x9c / 0xa0 | offsetTableVariable_length / values | u32 / up to 9 x 4 x i16 |
| 0xe8 / 0xec | offsetTableVariableSecond_length / values | same |
| 0x134 / 0x138 | gainTableVariable_length / values | u32 / up to 9 x 4 x u16 |

The decoder validates each count against its capacity. It preserves the four-site
order without assigning CFA colours. Stock's total-gain axis and output offsets
are **not yet converted into the port's RAW10/analog-gain units**.
The local entry's isEnabled fields are zero. Do not infer that these particular
tables run during a real stock capture: runtime overrides and other black-level
stages remain to be established. Both BLC block hashes are scenario-invariant
across the rear-main file. Their existence alone does not justify replacing the
port's empirical dark correction.

## Scenario names and actual entry selection

`--scenario-header` optionally parses `enum is_subscenario_id` from the local
Samsung `vendor/mcd/v8_1_0/is-metadata-vendor.h`. It accepts explicit integer
assignments, previous-symbol aliases and implicit increments; expressions are
rejected. Labels are source-version-dependent, and the report stores the header
hash. These are source-based candidate names, not an observation of stock HAL
requests on this phone.

The local rear-main container maps:

| Source scenario | Candidate use | ISP entry |
| ---: | --- | ---: |
| 0 | still preview | 0 |
| 1 | still preview HDR on | 1 |
| 2 | full-resolution preview | 2 |
| 3 | super-night preview | 37 |
| 10 | still capture HDR auto | 3 |
| 11 | still capture / Pro HDR off | 4 |
| 15 / 16 / 17 | MFNR / MFHDR / super night capture | 8 / 9 / 10 |
| 30 / 31 | FHD30 HDR off / auto | 13 |
| 40 / 41 | UHD30 HDR off / auto | 20 |

Several undeclared/reserved scenario numbers also map to entry zero. This does
not establish that those cases are implemented. The enum includes scenarios
beyond this container's 70 entries; do not index them blindly.

Rear-main block diversity: CCM 2 variants, temporal-noise tuning 6 variants,
demosaic tuning 2 variants, GRAS and both BLC blocks 1 variant each. A useful
next comparison is preview entry 0 versus capture entry 3 and night entry 10,
with separate treatment of static optical calibration and temporal algorithms.

## Reproduce and validate

```sh
python3 tools/camera/recover-samsung-ddk-schema.py /local/is_lib.bin > /tmp/schema.json
cmp tools/camera/samsung-isv8-schema.json /tmp/schema.json
python3 tools/camera/inspect-samsung-tuning.py /local/setfile_2ld.bin \
  --scenario-header /local/is-metadata-vendor.h --values > /local/research/2ld-values.json
python3 tools/camera/test_samsung_tuning.py
python3 tools/camera/test_samsung_setfile.py
```

The recovery command needs Python Capstone; container/tuning inspection and tests
use the standard library. Current validation: **17 synthetic tests**, exact
recovered-schema comparison, and all 122 real ISP entries. Local detailed values
and reports are under workspace `build/camera-re/`, outside this repository.
These checks prove decoding boundaries and typed fields, not improved images.

Next: trace CCM vector composition and GRAS representation/calibration; establish
BLC input units and runtime enable behavior. Keep the front untouched, fix the
separate per-frame black-state race before performance testing, and preserve the
known-good binned defaults until matched optical fixtures validate a replacement.

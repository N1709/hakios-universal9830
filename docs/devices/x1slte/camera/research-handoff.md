# S20 camera research handoff and preservation

Saved 2026-11-14 on **`feat/s20-camera-research`** in
`<workspace>/work/hackios-camera-improve`.
The existing **`camera-improve`** branch remains at `20d25baf` with all three
research implementation commits. This feature branch inherits those commits and
adds the consolidated handoff and artifact manifest. Accepted main/master and
fingerprint work are unchanged. No push was attempted.

## Read in this order

1. [Latest configuration audit and verified colour math](config-audit.md):
   confirmed differences, positive shading grids, emulation validation, provenance,
   and next implementation gates.
2. [Typed tuning layout](tuning-format.md): 65 blocks, CCM, residual
   LUT, BLC and scenario mappings. Its opening follow-up points out superseded
   uncertainty statements from the earlier research stage.
3. [Initial stock-versus-port comparison](quality.md): hardware,
   table equivalence and missing processing stages.
4. Preserved artifact/input manifest (private build manifest):
   absolute local paths, byte counts and SHA-256 checksums.
5. Existing [rear-camera operating history](../../../../tools/camera/REAR-CAMERA.md) and
   [camera checklist](../../../../tools/camera/CHECKLIST.md): previous hardware experiments,
   failure modes and accepted defaults.

## Conclusions to preserve

* The physical rear-main identification recorded by this project is S5K2LD,
  model 0x20cd, revision 0xA301. The normal S20's main sensor is 12 MP Dual Pixel;
  do not apply the S20 Ultra's 108 MP assumptions to it.
* All 7354 global-init triplets and both 417-triplet rear mode tables match the
  local Samsung source exactly. Re-importing the same tables is not the missing
  stock-quality step.
* The port defaults to 2016x1512, uses a basic CPU ISP, and has empirical black
  correction. Its software path lacks stock temporal/noise processing and complete
  module calibration. Existing full-mode artifacts and watchdog history remain
  unresolved; full mode has not been enabled by this research.
* The stock ISV8 container and 65-key serialized layout are decoded consistently
  across 122 ISP entries / 7930 named blocks. Internal types are decoded for CCM,
  both BLC blocks, the residual GRAS LUT, and selected main-GRAS fields/grids.
* Main shading data includes one golden/reference and five positive 4x25x33 grids.
  The separate residual LUT includes reference, focus-position and colour-
  temperature differences. Runtime combines calibration and golden-normalized
  grids; these must not be blindly applied as independent positive gain maps.
* The stock CCM9 helper is mathematically reconstructed: Q8 inputs, nine colour
  sectors, two packed columns per sector, signed rounding and Q10 output. Exact
  comparison against the isolated AArch64 helper passed 12644 cases. This is not
  yet a reconstruction of the entire Build/selection/composition pipeline.
* Source mismatches include forced group-hold writes versus Samsung's disabled
  helper, missing even-line exposure quantization, inaccurate nominal timing
  metadata and ignored rear-exposure I2C failures. The minimum coarse exposure
  and analog gain limits already match. A conservative 40-line normal margin
  is not itself a demonstrated quality defect.
* Stock full-resolution LN2 requires matching exposure steps/minimum/margin and
  possibly build-conditional settings. It is not supported by the normal binned
  mode predicate. The old combined plain/bus-speed experiment did not isolate
  these controls. AEB activity needs actual readback, not assumption.
* The adjacent vendor property artifact identifies G981BXXSNHYB1 (Android 11),
  not verified G980F shipping firmware. Local binary hashes/provenance remain
  essential. Shared silicon does not establish exact tuning compatibility.
* The reader/draw-thread global black-state race remains open. A request history
  based on a fixed frame delay also does not prove that failed I2C writes applied.

## Where everything is stored

Durable local research archive:

`<workspace>/discovery/s20-camera-research/2026-11-14/`

This archive copies the decoded-value reports, container/table/control audit
reports, partial disassemblies, schema-recovery output, exploratory analysis
scripts and verification logs out of disposable `build/`. It includes:

* `2ld-values-gras.json`: latest detailed rear-main values, including main grids,
  residual arrays, typed CCM/BLC fields, source-based scenario labels and the
  reconstructed CCM9 helper outputs. This is the preferred detailed-value report.
* `setfile_{2ld,3j1,2la,gw2}-tuning.json`: latest per-entry/block metadata and grid
  summaries for all four local tuning files.
* `control-audit.json`, `camera-improve-table-audit.json`,
  `camera-improve-setfiles.json`, `recovered-schema.json`.
* `ccm-code.txt`, `ccm-process-code.txt`, `gras-code.txt`,
  `gras-process-code.txt`, `blc-code.txt`: partial, file-offset disassemblies.
* `host-tests.log` and `ccm-verification.log`: saved verification output.
* `requirements-research.txt`: Capstone 5.0.9 and Unicorn 2.1.4, the versions used.

Earlier value reports and exploratory scripts are retained to avoid losing work.
`check_debug.py` contains an intermediate failed attempt, superseded by the
maintained verifier. Partial disassembly files are not complete decompilations.
The local archive is on this machine; it is **not an off-machine backup**.

Original firmware stays under `device/firmware/vendor-firmware/`, public Samsung
source under `git/ExtremeXT__android_kernel_samsung_exynos990/`. The manifest
fingerprints these inputs without committing firmware, bulk decoded values or
calibration backups. All maintained tools and layout metadata are tracked in git.

## Maintained tools

| Tool in tools/camera | Purpose |
| --- | --- |
| audit-2ld-tables.py | Integer-for-integer sensor table comparison |
| inspect-samsung-setfile.py | Container/scenario/entry boundaries and hashes |
| recover-samsung-ddk-schema.py | Hash-pinned symbolic recovery of the 65-key schema |
| samsung-isv8-schema.json | Recovered names, sizes and offsets; no firmware payload |
| inspect-samsung-tuning.py | Typed field/grid decoding; `--values` for local detailed reports |
| samsung_ccm_math.py | Reconstructed CCM9 helper arithmetic |
| verify-samsung-ccm9.py | Isolated, bounded Unicorn comparison with the stock helper |
| audit-s20-controls.py | Source-level control and timing audit with hashes |
| test_samsung*.py | 24 synthetic standard-library unit tests |

Run host tests from the worktree:

```sh
python3 -m unittest discover -s tools/camera -p 'test_samsung*.py'
```

The individual reports contain full decoding and emulation reproduction commands.
Inspection/tests need only standard Python; recovery needs Capstone, and isolated
emulation needs Unicorn. The existing optional environment is in
`build/camera-re/venv`; it can be recreated from the archived requirements.

## Commit history

| Commit | Contents |
| --- | --- |
| 8d584cc0 | Accepted local master base |
| 45372c29 | Initial comparison, exact table audit, container inspector |
| c471bce9 | DDK block-schema recovery, typed CCM/residual shading/BLC decoding |
| 20d25baf | Verified CCM9 math, main shading grids, control/timing audit |

## Next work and validation boundary

Continue offline with CCM Build-stage composition and sector selection; GRAS
calibration/CFA/crop/units and scene coordinates; BLC runtime units/enables; exact
target firmware provenance. Then prepare narrow, independently testable changes
for even-line normalization, timing metadata and acknowledged exposure state.
The stock math helper is ready for further software experiments, not for direct
replacement of the port's complete colour pipeline.

No runtime source/default changes, image builds, flashes or claimed optical
improvements were made. Keep accepted front-camera behavior and passcode intact.
Before device-changing operations, read workspace NEXT.md/TASKS.md and phone-lock
status. Before compiling kernel changes, mirror tracked xnu/files edits into the
correct isolated xnu-src build tree. **Ask the user before every reboot.**

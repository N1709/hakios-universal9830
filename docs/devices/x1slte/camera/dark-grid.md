# Rear additive dark-grid experiment

2026-11-14, `feat/s20-camera-quality`. **Experimental; live trial rejected and rolled back.**

The latest JPEGs IMG_0205/0206 show coloured noise and spatial colour gradients.
Covered-lens RAWs confirm the binned S5K2LD has spatial, CFA-site, exposure and
gain-dependent offsets. A uniform four-site pedestal cannot represent them.
This is separate from multiplicative lens shading: a dark pedestal must be
subtracted before white balance, CCM, gamma or GRAS gains.

## Capture and reproducibility

`tools/camera/s20_capture_probe.c` selects explicit binned rear mode and reads 36
headers, requests exposure after the first frame, and optionally saves a complete
RAW. Use it only with Camera closed and no other reader. Exposure range in this
probe is deliberately bounded to 16..22000 lines, gain 32..512.

`inspect-frame-raw.py` rejects partial transfers, checks the RWFRAME1 ABI, and
reports sparse per-tile/per-site statistics without third-party image libraries.
`make-rear-dark-grid.py` creates a private RWBLK001 file from 2..8 covered frames
of different gains but the exact same sensor/readout/exposure/frame/CFA state.
The operator must establish opaque coverage; a scene cannot be safely detected
as unsuitable from the RAW statistics alone. RAWs and generated profiles stay
outside git.

The candidate loader accepts only 2016x1512, 1468 ns/line, frame length 22564,
32 gain unity, finite offsets 0..256 and strictly increasing gains. The frame
must also match the calibrated sensor, CFA, exposure and gain range. It never
extrapolates exposure/gain or applies a rear map to the front. Profiles interpolate
in analog gain; the 17x13 tile-centred spatial samples interpolate bilinearly.
All RAW consumers use the frame-owned additive correction, including stats,
preview, sharpness and still demosaic. It is loaded per stream from the optional
`redwood-rear-dark-grid` file in the active tuning directory. No such file has
been installed. The new library is built locally, not deployed.

## Validation status

- Synthetic ASan/UBSan/TSan checks pass for frame ownership, concurrent rendering,
  spatial correction through preview/stats/still/full paths, gain interpolation,
  exposure/gain gates and front rejection. Existing 24 stock-container tests and
  66000 exposure-limit/fault-injection checks pass.
- Signed iOS candidate builds (existing CoreVideo deprecation warning only).
- The independent covered-capture evaluation **failed**: maximum absolute tile
  residual was 38.108 RAW counts. Signed site bias after correction was
  [7.982, 4.508, 2.969, 7.466] counts, versus the old [8.338,4.632,3.071,7.293].
  This is insufficient improvement. Do not install this calibration or advertise
  it as a fix.
- The mismatch primarily affects one region while other rows closely repeat.
  Sensor history/warm start, incomplete opaque coverage and capture attribution
  must be distinguished with another covered repeat before asserting a cause.
- `evaluate-rear-dark.c` exercises the production interpolation and reports signed
  residuals before clipping. Its old-offset comparison is specifically the 8x
  inside-table case; do not use those baseline numbers for other gain values.

## Next

Repeat the same covered exposure/gain twice, with the Camera app closed. Compare
spatial maps and actual sensor register readback. Investigate stock sequencing
if reproducibility remains poor; full-resolution Bayer/PDAF interpretation and
stock low-noise setup remain separate unresolved work. Do not enable LN2 for
the binned mode: the local Samsung driver only lists four full-resolution modes
as low-noise supported. Do not silently deploy unvalidated gradient compensation.

## CSI virtual-channel check

Local Samsung `module_dt/module_2ld.dtsi`, mode4, explicitly describes 2016x1512
30 fps: VC0 RAW10 image 2016x1512; VC1 RAW10 tail-PDAF 2016x378;
VC0 embedded-8 data 2016x2. Full mode0 similarly separates the 4032x3024
VC0 image from 4032x756 VC1 tail-PDAF. Current rear CSIS0 input register 0x0040
is programmed for VC0/data-type 0x2b, and WDMA format 7 is packed Android/MIPI
RAW10. This routing agrees at source level; it is not evidence that the port
must demosaic the PDAF tail as image pixels. Further per-frame routing/layout
validation remains useful, especially for full-resolution mode. Do not label
the visible noise as a four-in-one or PDAF-layout fault without that evidence.

## Live trial outcome

Fresh covered repeats passed a relative spatial-bias reduction check, including
high-gain interpolation, but IMG_0207 and user feedback did not establish a visual
fix. Logs confirm the correction was active at rear gain512/exposure20436.
Observed app rate was ~13–14fps (thermal baseline unmatched). Candidate was
removed. The original system media library is active again; front exposure
regression feedback requires baseline verification. No dark-grid calibration
is active. See tracked handoff.md for the complete chronology and hashes.

# Other S20-family Linux camera ports

Research checkpoint2026-11-14. No phone changes or downloaded-code execution.
Later paused for the user's brightness diagnostic. Original front remains active.

## Ubuntu Touch / Halium

[Sota4Ever's family status matrix](https://github.com/Sota4Ever/UTFamily-Samsung-S-or-N20-Series/blob/e0e94cea25a1a679e822d776f3dc28a9b006a3c4/Family-Soc-Exynos9830.md)
marks camera working for x1s, r8s and c1s. S20+/S20+5G entries are unknown and
untested; existence of y2s/y2slte configurations is not camera validation. Its
[y2s deviceinfo](https://github.com/Sota4Ever/UTFamily-Samsung-S-or-N20-Series/blob/e0e94cea25a1a679e822d776f3dc28a9b006a3c4/deviceinfo-y2s)
selects a downstream Android kernel, not a new mainline ISP driver.

Fetched kernel revision `08d22bb2265640392ac945fc83d17388544a4840`.
S5K2LD CIS driver, set-A tables and CSI-v5.3 implementation are byte-identical to
our existing local ExtremeXT source. See the hash manifest alongside this report.
There is no newly discovered sensor register table in these three files.
The y2s defconfig retains Samsung PABLO ISP, EEPROM calibration, retention, OIS,
AF host control and the2LD driver.

The maintainer's [public runtime logs](https://gist.github.com/Sota4Ever/6a46c34800e5e0c9e35ecc38c91ab090)
show Samsung ExynosCamera, DDK/setfile loading, PDP/3AA/ISP/MCSC and a2LD setfile.
The trace has six fast-AE frames at1008x756/120 before a4032x3024 pipeline.
This is r8s evidence, not a verified y2s trace. Older reports describe JPEG
completion followed by capture stalls; successful processing is not proof of
stable photo saving. The exact final fix for those older stalls was not established.

Halium's [architecture documentation](https://docs.halium.org/en/latest/project/Planning.html)
describes camera adaptation through libhybris. Inference from the configuration
and runtime trace: this port retains Samsung's HAL/ISP machinery rather than
reconstructing picture quality from a bare RAW stream as our software ISP does.
Do not infer an exact frontend library version from the kernel log alone.

## Mainline / other devices

- [ben443's pmos repository](https://github.com/ben443/samsung-s20-pmos/tree/c75cdba1d1470e6dddad9221abafbc509731c6f3)
  lists camera unimplemented; it also mixes z3s and G980F labels, so model claims
  require care. No working camera processing implementation found there.
- [r8s-mainline](https://github.com/victorpivotto-cloud/r8s-mainline)
  explicitly reports no Exynos990 ISP camera support in its README when checked.
- [S20 FE Droidian port](https://github.com/pedrogomes30/galaxy-s20fe-linux-port)
  reports photos using an Android vendor stack, but targets Snapdragon865/r8q.
  Its Qualcomm camera settings cannot be used as Exynos990 sensor values.
- A public S20+ Debian13/all-cameras claim was found in a Reddit comment, but no
  linked implementation or reproducible camera test was established. Treat it as
  a lead, not validated code. Do not contact its author without user instruction.

## Consequences for the iOS work

Continue checking rear-specific transport/packing/readout. The comparison does
not rule out a bug in our receiver or RAW interpretation. It also gives no basis
for copying front settings or calling all full-resolution data inherently
non-Bayer. Useful reference targets are the complete mode transition, calibration
and hardware-ISP path, beyond the already-matching mode tables.

Linux/Halium binaries depend on their OS and driver interfaces; they are not a
drop-in Darwin library. Hardware-ISP adaptation or faithful processing/calibration
reconstruction remains substantial work. No new quality fix was deployed from
this research.

Local evidence: build/camera-otherports (pinned family/pmos checkouts, fetched
kernel sources, source hashes, camera history and the public gist logs).

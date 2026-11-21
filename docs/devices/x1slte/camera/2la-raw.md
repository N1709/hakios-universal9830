# Bounded 2LA native RAW receiver — prepared, not yet device-verified

Previous live kernel positively identifies both auxiliaries and streams2LA's
native4032×3024 sensor counter. This next candidate adds `cam.s20.2la.raw`,
root-only, no arguments. It does not add Camera lens controls or a persistent
stream. Default front/main userland paths are unchanged.

## Pipeline and gates

Exact20ca/a202 identity required. Stock initial/Global_2/full12MP/single-sync
settings retain the successful counter-test sequence. The new receiver uses
board overlay C-PHY3trios, PHY0x170d2300/reset bit2, CSIS4 at0x17070000, WDMA0
VC0/common0x170c1000/1400, mux0x17020414=4. DT LRTE_DISABLE respected. C-PHY
0503.0000 analog settings use the stock1501-speed settle7, not front D-PHY.

Before access, require CSIS domain on, enabled S2MPS22 PHY supplies and secure
CSIS restore. Enable the shared/CSIS4 clocks and dedicated stock CSIS bus branch
with bounded BUSY waits. Read SysMMU only after its clock gates are observed on.
Require v7 pass-through; never alter page tables. Refuse any active WDMA/VC,
pattern generator or pending interrupts. Existing camera ownership reservation
prevents an iOS stream from starting concurrently.

Allocate one24,385,536-byte unpacked16-bit frame plus16KiB guards from the existing
low32-bit contiguous camera pool. It requires two adjacent14MiB slots. Format6
checks packing independently of Android RAW10 mode7. All32 frame pointers target
the guarded buffer. Start receiver before sensor, poll the sensor for600ms,
stop the sensor and confirm stop counter, then disable CSI/WDMA and request abort.
Only a confirmed abort-ack AND inactive DMA permits export/free/restore. Otherwise
retain the DMA allocation/configuration, report buffer_retained and fail camera
ownership closed until an authorized recovery. Never free potentially live DMA.

The configuration journal snapshots registers BEFORE both digital and analog
resets; resetting itself can clear untouched fields. Later writes preserve the
first snapshot and union of masks. Status/self-clearing bits are not mistaken
for persistent fields. Restore PHY settings while reset held, then its prior reset
state. Link counters and test W1C status are diagnostics, not restored history.
Unchanged journal fields need no write. Continue remaining restores after a
readback failure and surface failure to the camera reservation.

## Acceptance and export

After drain, require DMA frame-end, no DMA error, no link error, unchanged guards,
changed payload and unpacked values<=1023. A CPU-owned copy is repacked as
RWCAM001 with labelS5K2LA,4032×3024 and stride5040 for root-only
`kern.redwood.camera_frame`. Packed export is15,240,960bytes plus4096-byte header.
Prior export is cleared at test start to prevent downloading a stale frame.
An exported RAW frame still needs host CFA/layout/scene inspection before calling
this a functioning preview. No claimed optical calibration or stock ISP quality.

## Validation and artifact

ASan/UBSan compiles the ACTUAL receiver header with simulated MMIO/allocator:
valid frame export, digital/analog pre-reset state restoration, guard overwrite,
out-of-range pixel/link error rejection, active-MMU refusal, failed PHY readback
rollback and no-free-on-abort-timeout all pass. The standalone journal suite
faults every configuration operation, including a latched failed write, mask
union, unrelated bits and cleanup failures. Existing inventory/routing/catalogue
and passcode checks pass; native GICv2 kernel compile succeeds without new warnings.
Tracked and build sources match. Simulation replaces CPU barriers; actual cache,
PHY and DMA behavior still requires hardware testing.

Private candidate in build/camera-aux-latest-20261115/:
`kernel-slot-2la-raw.bin` SHA256
`a1f28ed505feb3c27f201d1061f887bd4cecdee3ba84b1f07ab0a00839a59d2e`.
Stripped kernel15212888bytes, SHA256
`aba710dbd46954d4511d9ed5871d26dfd9104cf0b1c17e70dae7ffe4e2095858`.
NOT installed. Current working sensor-counter kernel slot15d996b7... is the
immediate rollback; original16:31 kernel remains backed up separately.

Live read-only prerequisites: CSIS domain1, QCHs0x3f0002, PHY rails0xcc/0xec.
They do not prove the new PHY4/WDMA0 receiver has received a frame. Obtain the
next reboot authorization, retain current loader/root/system/data, install only
this slot with loader readback, then run cold/idle raw command once. Stop on
failed cleanup; keep private image/capture/logs outside git. Validate original
front/main and repeat locking after the new boot.

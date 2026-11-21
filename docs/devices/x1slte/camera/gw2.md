# GW2 third-rear bring-up

Isolated branch feat/s20-gw2-telephoto from accepted f03fd343. Preserve front,
primary and ultra-wide behavior and permanent root. Existing session camera-test
reboot authorization continues. Stable full recovery/boot rollback remains in
workspace build/camera-final/deploy. No new Camera lens until RAW is verified.

Hardware GW2 ID0972/A300. First diagnostic loads stock setA TNP firmware/global,
single-sync and2432x1824@30 mode; verifies output dimensions before stream-on;
checks12 frame-counter samples; verifies stream-off and restores both PMIC maps,
GPIO and MCLK. No receiver or autofocus/OIS writes. No EEPROM calibration is
invented: diagnostic uses stock uncalibrated-module PDXTC-disable fallback.
Image tuning/calibration remain unqualified. Firmware comes from Samsung's GPL2
Exynos990 kernel table, with source hash recorded in generated header. Checked
executor validates before writing, chunks indirect firmware transfers128words,
and fails immediately on any I2C error. Host test checks exact firmware byte order
and every transfer failure, plus invalid table refusal.

The board uses CSIS2 C-PHY3, PHY170d1b00, WDMA3/VC0170c4000 and mux17020420=2.
A 5x+ application route will require sensor capture, calibration/exposure, focus,
measured field-of-view alignment and cropped high-resolution data. S20 hardware
is specified as3x hybrid; do not advertise5x optical or30x quality without tests.

Live counter test passed:2432x1824,11changes, verified stop/cleanup. GW2 must
read width/height using separate16-bit reads; combined4-byte read was invalid.
Controls read back correctly: frame3608/line17680, exposure3000/gain64. Receiver
initialization refused before DMA arm, cleanup passed. Auditing stock mux code
found mux_reg=[address,byte_length], not[address,value]: write CSI channel2,
not length4. Added exact failed-MMIO report and corrected route in next build.
Front/main/UW capture regression36frames each passed after the refused probe.

RAW2432x1824 now PASSED: no CSI/DMA errors, guards intact, exact controls,
shutdown/restoration verified. Frame was exported and a real scene was visible.
WDMA3 address slots17..32 read0xdeadc0de; configure only16 implemented slots.
VC1 carries608x904 RAW10 PDAF data and needs valid CSI dimensions even when not
DMA-captured; configuring it removed WRONG_CFG. Full crop4864x3648 size readback
passed but control gate caught a timing conversion error:0x142e is5166 lines,
not5294. Corrected to5166/12304 (1918MHz pixel clock) before stream test.

Next candidate adds mode6 continuous4864x3648, distinctive sensor4 metadata,
finite exposure limits, frame-geometry guard, and GW2 AK737x0x0f on HSI2C0, per
stock DT (primary remains0x0c). Native selector test passed on actual CameraUI
for the historical multi-button prototype and10x drag with no graph zoom forwarding; front delegate
restoration still passed. GW2 backend/UI are opt-in by redwood-gw2-enable.
3x is a provisional test entry, not a claimed stock switching threshold.
Samsung public documentation confirms3x hybrid/up to30x digital only:
https://www.samsung.com/uk/support/mobile-devices/check-out-the-new-camera-functions-of-the-galaxy-s20-plus-s20-ultra/
This prototype caps the control at16x and uses stock central crop, nominal2x
base pending field-of-view calibration. It does not implement5x optical zoom.

## User-accepted integration and final packaging

User confirmed3x live image, then confirmed the SINGLE native lens icon cycles
0.5->1->3->0.5 and the scale closes. No three-button platter or overlay is
installed. Native tap handlers run their normal cleanup before the selected
route is applied; front delegates/layout retain their original behavior.
The original dual-style native dial is retained, with16x maximum when GW2 is
enabled. A native-handler device test covers the cycle and closed dial.

Continuous4864x3648 passed36frames with actual focus metadata, then UW, main
and front each passed36frames. App logs verify4864x3648 delivery and rendered
frames after fixing BOTH old width heuristics: rear/front classification and
the legacy deliberate-black rear gate now use sensor identity. Stale GW2
headers are discarded when returning to the pinned original primary worker.
User confirmed image and cycle. 5x/10x control trials kept the GW2 route active.
Factory PDXTC calibration and OIS are not newly implemented/qualified.

Startup request: baseline first-frame probe GW2=2651.4ms, primary=2945.3ms.
Cold firmware uploads dominate; main/GW2 share MCLK0 and are powered down on
switch, so true seamless switching is NOT implemented. Primary already uses
1us half-period. Final candidate speeds ONLY GW2 firmware upload from4us to
the bounded rear setting1us; identity/exposure reads retain4us. On any upload
error, replay the full sensor reset/TNP at4us and retain slow fallback until
reboot. Host tests cover fallback and restoring bus timing. Final live speed
measurements are recorded after deployment. Do not promise instant switching.

Clean images now seed redwood-gw2-enable and include native cycle/media hooks
and the kernel's GW2 firmware/receiver/motor/four-slot DMA support in source
and tracked prebuilts. Existing files are preserved by init seeding. No wiped
data boot was performed; clean seeding is fixture-tested.
Private deployment/rollback artifacts: workspace build/camera-gw2/permanent.
Original stable2-lens rollback remains build/camera-final/deploy.
Final image is prepared from the live root, retaining its keybag/AOD/telephony
chain, logos, loader configuration, system and user data. Temporary native UI
probe files were removed. After reboot, injection must point to the root media
library, not the temporary GW2 test file.


## Permanent deployment verified (2026-11-16)

Final source/kernel integration: e859bd7a. Recovery and standalone boot were
written and read back successfully; the phone rebooted into the 07:42:36 IST
kernel. The root media library hash is
`8a5e6e11da631fe50f349f1378b6ef59c9c6599851b844e2d59ad78c2d2dcbf0`.
Runtime injection uses `/redwood/lib/libredwood-media.dylib`, not a development
override. Protected keybag restored locked (state0x5). Recovery SHA256:
`bcd83e4ec5230288bcaf07e2bc03123ca8eb595b43e09a6f587e9114306b0348`.
Full artifact hashes and rollback are in build/camera-gw2/permanent.

The final cold GW2 first-frame probe measured1671.1ms versus2651.4ms before
(37% reduction), and delivered36 native4864x3648 frames. This is a single
cold-start comparison, not a guaranteed Camera-app switch latency. Primary's
2945.3ms baseline is unchanged; shared-clock firmware reinitialization still
prevents seamless primary/third-sensor transitions. Front processing is pinned
to its original worker. No wiped-data test was performed.

Final post-deployment regressions: ultra-wide, explicit binned primary mode4,
and front each delivered36frames; primary exposure/gain readback passed in
mode4. The legacy boot-default mode2 probe delivered36frames but did not
apply its manual exposure assertion; this is not counted as an exposure pass.
App primary continues to use its explicit accepted mode. Front remained on its
original worker. Final regression logs are in the permanent artifact directory.

# Rear main camera (S5K2LD) on the Galaxy S20 under iOS — state, known flaws, leads (2026-11-12)

For whoever continues (person or AI agent). Front camera (SAK3J1) is finished and user-approved: do not change it.
The user's decision: the rear stays in its BINNED 2016x1512 mode; improve everything else.
History: sessions/B.md, sessions/A.md, SHIPPING.md; branches `x1slte` = `camera-b` (this state), `a/camera-rear`.

## What works (verified on the phone)
- Sensor: S5K2LD id 0x20cd rev 0xA301 at 0x10 on HSI2C3 (gpp0-6 SCL / gpp0-7 SDA, bit-banged), MCLK0 gpc0-0, reset gpm22.
  Samsung tables from is-cis-2ld-setA.h (`tools/camera/import-2ld-setfile.py` -> `redwood_camera_s20_2ld.h`): Global_A3,
  then mode 2016x1512@30 (frame length 22564 lines, line 2272); stream on = 0x0BCC=0x0100, 0x0100=0x0100 on page 0x4000.
- Receiver: C-PHY, 3 trios, CSIS0 0x17030000, combo PHY csi0 0x170d1300 (Samsung __set_phy_cfg_0503_0000_dcphy C branch,
  settle 7), CMN_CTRL PHY_SEL=1 lane number 2 interleave 3, **LRTE_CONFIG EPD_EN (0x600 bit 31) must be 1** (DT lrte:
  without it only frame starts arrive), **CSIS bus clock TOP 0x104c=1 / 0x1848=0** (else ERR_OVER), Q-channel 0x3060.
- DMA: WDMA2 (VC0 0x170c3000, common 0x170c3400), input mux 0x1702041c = 0, RAW10 packed (format 7).
- To iOS: kernel REDWOOD_CAMERA_SENSOR a=1 b=2 selects the rear (b=0/1 front full/binned); libredwood-media's Back
  Camera asks for it. Exposure via SMIA group hold (0x0104; 0x0340/0x0202/0x0204, select page 0xFCFC=0x4000 first).
- Autofocus: AK7377 (id 0x13) at 0x0c on HSI2C0 (gpp0-0/1), same driver as the front's AK7374.
- Orientation: rear uses `redwood-rear-turn` (default 4); front `redwood-camera-turn`/kernel 5.
- Colour matrix measured from a screen chart (white-framed patches): redwood-ccm-s5khm2sd =
  1.588 -0.342 -0.246 / -0.407 1.764 -0.357 / -0.437 -0.239 1.676. Bayer order GRBG (kernel tag 1) is right.
- Flips front<->rear: clean since commit 86265e0 (one rear start per flip; verified 6 flips).

## Known flaws (open), most visible first
1. **Dark scenes go green/purple with a gradient across the picture.** The sensor's black level is NOT a constant 64:
   it sinks with analog gain (Gr 63.5 at 1x -> ~30 at 16x), differs per Bayer site, depends on where the exposure sits
   in the frame (8000-20000 lines: ~18-40 at 16x; 2 lines or frame-stretching exposures: ~60-70), AND drifts left to
   right across the frame (up to 35-50 DN at 8-16x, direction reverses between regimes). Per-gain/per-site tables are in
   camera_device.c (rear only); session A added per-frame measurement. Bright scenes are fine. NOT sensor damage: dark
   frames are repeatable. Untried leads: Samsung's low-noise register 0x0B30 (0x0100 = LN off, written at mode change in
   is-cis-2ld.c; A tried it together with a faster I2C and got noise — never tested alone); sensor BLC 0x0B04 (A: on/off,
   no cure); a per-column dark map per gain (phone face down); Samsung's retention sequence.
2. **Start/flip to the rear takes ~3.1 s** (front ~0.6 s): the 7354-record Global_A3 table over bit-banged I2C
   (half_us 2; half_us 1 untested alone). Leads: keep the sensor powered in soft standby between sessions and send only
   the mode table (417 records); Samsung's "retention" tables exist for exactly this; a real HSI2C controller driver.
3. **Low light: 15-20 fps and noisy** (CPU ISP ~50 ms/frame at 2016x1512 + long exposures; no noise reduction).
4. **Vignetting / green corners**: no lens-shading table for the rear (files are flat 1.0). Calibrate with a diffuser:
   `touch <tuning dir>/redwood-lsc-capture` while white paper covers the lens.
5. **12 MP (4032x3024) mode**: works as plain Bayer only with sensor HDR off (0x021E=0), but has a black-level step at
   row ~1912 and a 32-row structure; two hardware-watchdog resets happened during 12 MP raw dumps. Left OFF
   (`cam_s20_rear_full=1` to test). Details: sessions/A.md.
6. **Occasional frame glitches** reported by the user on the rear; nothing in the logs (no DMA errors, no refused
   buffers). Suspects: black tables changing between frames while the draw thread renders (race found by A's review:
   isp_black4 globals are written by the reader thread), slot choice at frame end.
7. **Rear video recording: FIXED 2026-11-12 (session A, 7c3b2ee)**: rear movies were HEVC through Apple's slow software
   encoder; now H.264 (inject_media.c movie_video_settings): complete files, A/V in sync, ~20 fps. Older note: Recording should work like the front's
   (same encoder); the LED is the S2MPB02 FLED already driven as the torch (`redwood_s2mpb02_torch.c`, /dev/redwood-torch):
   fire it around StillImageCaptureNow when FlashMode is ForceFlashOn and the camera is the rear (front uses the screen).
8. CSIS0 reports ERR_WRONG_CFG (VC1 PDAF rows / embedded data not configured); harmless so far.
9. Ultra-wide (S5KGW2) and tele (S5K2LA) not started. Live Photo movie never written (both cameras; sessions/B.md).

## Tools
- No-reboot camera code: `tools/camera/media-dev.sh` (tuning dir: /var/mobile/Library/Caches/com.apple.mediaserverd).
- Raw rear frame: Camera closed, `tools/poke 'cam.s20.dma.test 2 [g*100000+lines]'`, `ssh s20 /var/root/s20-frame`,
  `tools/camraw.py`. Dark frames: phone face down. Sensor readback each second in the kernel log ("rear frame N").
- The kernel the phone runs is built in work/hackios-cellular (cellular + GPU changes); copy
  bsd/dev/redwood_camera{,_s20}.c there from the main xnu-src before building.

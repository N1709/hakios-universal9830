# Apple ecosystem over Bluetooth (Continuity, Instant Hotspot) — findings and status (2026-11-19, B)

Goal: the S20 in a Mac's Wi-Fi menu under "Personal Hotspot" with battery, signal bars and LTE/5G, switched on from
the Mac (Instant Hotspot); later Nearby, Handoff, AirDrop, iMessage/FaceTime. **Parked by the user's choice**: the
pieces below are shipped; the rest is written down for another time.

## What already works (no Continuity needed)

- Personal Hotspot itself (docs/devices/x1slte/hotspot.md): "Jesvi IOS", WPA2, internet, island icon.
- Macs see "Jesvi IOS" as a phone hotspot (IsExpensive, hotspot symbol): Apple's vendor element 00:17:f2 type 06
  in our beacons (`kern.redwood.wifi_ap_vendor_ie`, default built into hotspot-up.sh).
- Classic Bluetooth both ways: the Mac discovers the S20, pairs with a code, lists it under My Devices. A2DP audio.

## The chain Instant Hotspot needs, and where each link stands

| # | Link | Status |
|---|------|--------|
| 1 | sharingd decides the phone can share its hotspot | **DONE.** Without CommCenter it said `tethering support = NO (simStateReady NO, cell data enabled NO)`. libredwood-mis (userland/src/mis_ct.m) is now also loaded into sharingd: CoreTelephonyClient getSIMStatus (CoreTelephony's own kCTSIMSupportSIMStatusReady), -[SDStatusMonitor simStateReady] = YES, -[SDStatusMonitor cellularDataEnabled:airplaneMode:] (types v32@0:8^B16^B24) -> enabled. Log now: `tethering support result = YES`. |
| 2 | sharingd knows the account's other devices (IDS peers) | **BLOCKED.** identityservicesd is back in the system image (sysimage/select-system.py allow-list; 12 MB). With the Store relay on, its Anisette (device proof) now works. It cannot register: **no APNs push token**. |
| 3 | apsd (Apple Push) has a token | **WALL.** `Unable to copy activation identity: _APSCopyActivationIdentity() failed. Device is probably unactivated.` The push certificate comes from Apple's device activation (albert.apple.com), which rejects this phone (500, seen in the Store work). |
| 4 | sharingd asks bluetoothd to advertise Tethering Source (BLE, Apple type 0x0E: battery, bars, network type; furiousMAC/continuity) | Not reached (needs 2). |
| 5 | bluetoothd's advertising reaches the air | **Will need work.** bluetoothd advertises through Apple-firmware vendor commands (BD_VSC_LE_CREATE_EXT_ADV_INSTANCE, ..._SET_EXT_ADV_INST_PARAMS/DATA, _SET_EXT_SCAN_RSP_DATA, _SET_EXT_ADV_INST_ENABLE). The S20 runs Samsung's Broadcom patchram: it answers status 0x1 (unknown) to Apple's 0xfce9/0xfce2 setup commands (IRK list "status 712", etc.). The driver (redwood_exynos_bt.c) must translate those into standard LE (extended 0x2036-0x2039 if the chip has them, else legacy 0x2006/0x2008/0x2009/0x200a, rotating instances) and answer bluetoothd as Apple's firmware would. |
| 6 | The Mac accepts the advertisement | Needs 2 (same Apple ID; the Mac matches through iCloud). |

## Ways past the wall (3), for another time

1. **A real Apple device as push relay** (like the Store's Hackintosh relay): the most realistic; days of work, no
   guarantee.
2. **Borrow the Hackintosh's APNs identity**: Apple would see two devices on one identity; the Mac's push would
   likely break.
3. **Fake an activation** (pypush-style): Apple closed most of it by checking device signatures.

## Reverse-engineering notes (to resume quickly)

- bluetoothd's vendor-command names: a pointer table in /usr/sbin/bluetoothd at file 0x804a28 (__DATA_CONST, vm
  0x100804a28), 219 names, indexed by bluetoothd's own number (not the opcode); name lookup function at
  0x100282c00 (`cmp w1, #0xdc`). Opcodes are built in code (0xfc00 | OCF), not stored next to the names.
- At start bluetoothd logs which vendor commands it believes the chip has ("VSC X, not supported"); the
  EXT_ADV ones are in the supported list, so it will use them once something asks it to advertise.
- Kernel (feat/s20-continuity): `bt: adv 0xfce9/0xfce2 N: <hex>` logs those commands whole (first 400).
  Seen: 0xfce9 sub 01 (27 10 xx 00), 02 (24 B), 05, 07 (16-byte key: IRK), 1e, 32 (54 B), 35; 0xfce2 sub 0b, 0c, 0d,
  15, 1c, 1e, 26, 36. Our driver also keeps 0xfdb9/0xfd81/0xfc57 from the chip (they hung it at audio start).
- wifid's STA_ARRIVE layout (used by the hotspot): {mac[6], pad[2], count@8, pad[3], capabilities@12}.
- The Store relay (apple-account work) must be on for Anisette: tools/apple-account, sessions/B-apple-account-handoff.md.
  Its boot job is now in the image (userland/com.redwood.account-store.plist) and rwsys is built.

## Open-source references

- furiousMAC/continuity — Apple BLE message formats (Tethering Source 0x0E, Nearby Info 0x10, ...).
- OpenDrop / OWL (seemoo-lab) — AirDrop / AWDL.
- pypush / openbubbles — IDS/APNs registration outside Apple hardware (and why activation is the hard part).

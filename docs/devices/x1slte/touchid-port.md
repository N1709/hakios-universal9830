# Touch ID on the S20: everything in one place (A, 2026-11-20)

Status, how it is built, what was learned, and where to pick up. Older detail: [touchid.md](touchid.md) (handoff log),
[fingerprint.md](fingerprint.md) (the TA protocol), [fingerprint-handover.md](fingerprint-handover.md),
[android-reference.md](android-reference.md) (Samsung's own command sequence and the reversed `identify`).

## Status

| Part | State |
|---|---|
| Apple's side (biometrickitd, Settings, lock screen) on the **ported** `libBKDM1`, no iPhone 8 cache | works (2026-11-20) |
| Enrolment (Samsung TA, 17 touches, ~35 s, template saved) | works |
| Enrolled finger kept across reboots (catacomb) | works |
| Ring on the lock screen, shake on a failed read | works |
| Clean install: TA set + sensor calibration put back by themselves | works (calibration verified byte-identical) |
| **Real unlock by the TA's matcher** | **not yet: every identify is NO MATCH or 0x1d** |

Unlocks reported earlier (2026-11-19) came through Apple's phase-2 window (RedwoodMesa passes "a finger touched the
sensor" while Settings finishes an enrolment), not through the matcher. `kern.sweet_mesa_match=2` delivers whatever
redwood-fpd says; the matcher itself has never said MATCH.

## Pieces and where they live

| Piece | Source | On the phone |
|---|---|---|
| Kernel persona (AppleBiometricServices, bridge, catacomb) | `xnu/files/iokit/Kernel/RedwoodMesa.cpp` | kernel; `touchid=1` in `x1slte.cmdline` |
| TEEGRIS client, `fp ...` commands | `xnu/files/bsd/dev/redwood_teegris.c` | kernel (`kern.redwood.tee`) |
| redwood-fpd: bridge <-> TA, enrol/match loops, catacomb, clean-install provisioning | `userland/src/redwood-fpd.c` | `/redwood/bin/redwood-fpd` (`com.redwood.fpd`) |
| Ring on the lock screen | `userland/src/fp_ring.m` | SpringBoard library |
| Touch ID module `libBKDM1` (Apple, iPhone 8) | `tools/touchid/fetch-bkdm1.sh` + `relink_bkdm1.py` + `bkdm1-map.json` | `/usr/lib/libBKDM1.dylib` |
| biometrickitd job (`-module 1`) | `userland/com.apple.biometrickitd.mesa.plist` | runs via dsc-exec, which falls back to the system cache |
| Samsung TA set (FINGE `...46494e474502` + driver TAs) | `device/firmware/vendor-tee` (not in git) | `/var/root/tee` (copied from `/System/Library/Redwood/Fingerprint/tee`) |
| Factory calibration (CBGE slots 0, 8; sensor_id) | the phone's `sec_efs` `biometrics/meta` | `/var/root/fp-calib` (`cell-provision --fp`) |
| Enrolled template | made by enrolling | `/var/root/fp-template.bin` |
| Unlock/enrol CONTROL op lists (testing) | `redwood-fpd` defaults | `/private/var/redwood/match-ops` (see below) |

## Porting libBKDM1 off the iPhone 8 cache

The iPhone X cache has only libBKDM2 (Face ID). libBKDM1 exists only in Touch ID devices' caches. Running
biometrickitd on the whole iPhone 8 cache (3.1 GB, `DYLD_SHARED_CACHE_DIR`) worked, but it put iPhone 8 UIKit etc.
under SpringBoard's daemon and needs 3 GB on the data volume, and an iOS without a Touch ID device (iOS 26) has no such
cache. So only the module is cut out and relinked onto the image's own cache. `tools/touchid/relink_bkdm1.py`, in the
order the problems were found (each one crashed or silently broke biometrickitd):

1. **Layout**: segments on 16 KB pages, file offset = VM offset (dyld reads sections from the unmapped file at their VM
   offset: `MachOAnalyzer::hasSwiftOrObjC` crashed). The gaps are zeros: ~130 MB sparse, ~0.4 MB compressed. Copy it
   with `ssh -C` or `rsync -S`, never plain scp.
2. **Imports**: `__got` from the indirect symbol table, bound by name per library ordinal.
3. **Pooled GOT**: the cache builder loads many imports straight from shared `_ptr.X` slots, some outside the image and
   some *inside* `__DATA_CONST`'s range past its last section. All repointed to an own `__RELINK,__got`.
   `bkdm1-map.json` names 215 such slots (`ipsw dyld a2s` on the iPhone 8 cache).
4. **Decoder**: capstone's `disasm()` stops at the first undecodable word; only 583 of 1,135 pooled loads were repointed.
   ADRP/LDR is now decoded by bits over the whole `__text`.
5. **Stubs** (189) and **`objc_msgSend$sel` stubs** (858) pointed into the cache's own pointer area: repointed.
6. **ObjC**: class caches bound to `_objc_empty_cache` (were 0: crash in `objc_msgSend`); `OptimizedByDyld` cleared in
   `__objc_imageinfo`; class references in `__EXTRA_OBJC` bound by class name.
7. **Method lists**: the cache's small (relative) method lists name selector refs outside `__objc_selrefs`, which
   libobjc never uniques, so `-[BiometricKitXPCServerMesa init]` fell to the superclass and the sensor was never
   opened. All 27 lists are rewritten as classic lists in `__RELINK`.

Checks: `dyld_info -validate_only`; on the phone a dlopen test (22 classes, `-init` resolves to the module), then
biometrickitd opening AppleBiometricServices (`rwsys kern.sweet_mesa_state`: `abs_clients=1`, `cmds>0`).
`fetch-bkdm1.sh` reproduces the proven file byte for byte (apart from the signature's identifier).

**Pitfall**: replacing a library or binary **in place** on the phone (`cat > file`) keeps the vnode, and the kernel
keeps the old code signature cached on it: "code signature does not cover entire file", or SpringBoard aborting at
launch. Always write a new file and `mv` it over.

## Clean install

`redwood-fpd` (`fp_provision`, before TEEGRIS starts): copies the TA set from the system image when
`/var/root/tee` has no FINGE TA, and runs `cell-provision --fp` when `/var/root/fp-calib` is empty. The TA set is in the
image only for a build for this phone (`SHARE` unset); a public image must capture the phone's own `/vendor/tee` during
install (the installer replaces `vendor` with a 1 MiB placeholder) - not done yet. Without the TA: `fp: open err 2`
and enrolment fails at once ("Unable to complete Touch ID setup").

## Why the matcher says NO MATCH (where to continue)

Read against Samsung's own service (`libbauthserver` `FPBAuthService::identify` 0x50684, `libbauthtzcommon`
`BAuth_Control_OP` 0x6038, `BAuth_Identify_Init` 0x4f9c; full notes in [android-reference.md](android-reference.md)):

- **CONTROL 22, 49, 108 never reach the TA** on Samsung (answered client-side); do not send them.
- 74 carries value 0 (`iae 74 0 0` in logcat) = what we send. 75 returns <= 1000 bytes that 46 sends back after the
  touch, then IDENTIFY_FINAL (cmd 7) x3: not done by us yet.
- IDENTIFY_INIT params (0x45 bytes at +0x64518): +0x19 = 2 on Samsung; ours 0. Not changed yet.
- **Most likely cause**: identify needs **metadata type 3** (+0x64114). Samsung makes it at the end of enrolment as the
  reply to **CONTROL 45 (SAVE_SECAUTH_ID)**, which needs the secure user ID from the enrolment's **HAT (command 13)**.
  We have no HAT, 45 refuses with "sid is 0", there is no type-3 metadata, identify cannot match.
- A HAT has to be genuine: Samsung's `send_hat_hmackey` is client-side here, so the key lives in the secure world.
  **Next work item: drive Samsung's Gatekeeper TA** (`...474154454b45`, GATEKE) - enrol and verify the passcode with it,
  pass its token to command 13 at enrolment, save the 45 reply as metadata type 3 (`fp_meta_save`), feed it to identify.
  Then 75 -> 46 -> cmd 7 and params +0x19 = 2.
- Also: unlock captures are often rejected 0x1d while enrolment accepts every touch. Samsung also loads
  `bg_estimation*/bg_basis.dat` and `bg_metadata.dat` (in `sec_efs` `biometrics/meta`), which we never load.

### Testing CONTROL ops without a rebuild

`/private/var/redwood/match-ops` (read at each enrolment and match start), for example
`pre=101,74 arm=off post=75,46 epre=43,16 earm=75 epost=45,46,75` (`off` or empty skips a list). Logged as
"ops from ...". Every op is sent as `fp 12:N` without data; the TA answers status 0xc (= the command number) and
error 0 when it accepts one.

## Commands

    tools/touchid/fetch-bkdm1.sh                     # once per machine (needs ipsw; ~3 GB download, deleted after)
    ssh s20 /redwood/bin/rwsys kern.sweet_mesa_state # abs_clients, cmds, catacomb
    ssh s20 /redwood/bin/rwsys kern.redwood.tee      # TEEGRIS state, last fp command
    curl -s http://10.77.0.2:8080/tail?log=0\&dmesg=1 | grep redwood-tee: fp
    ssh s20 /var/tmp/bkdm/oslog -t 60 -l biometrickitd    # Apple's side (build: git/DenialOfGravity.../oslog.m)

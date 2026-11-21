# ExtremeROM cross-check — 2026-11-14

Reviewed the original ExtremeXT/ExtremeROM `fifteen` source at
**2056320784b7d1d88f9e7025f86f7bbd39945d6b**, rather than treating S10 instructions
as a generic Exynos install recipe. This is a source comparison, not evidence of a
successful installation or stock restoration of this iOS candidate. No ExtremeROM
installer code/binaries were copied into our native MIT-licensed recovery backend.
The source project is archived; it remains useful design evidence.

## Relevant findings

- Its [official installation guide](https://github.com/ExtremeXT/ExtremeROM/wiki/Installation-Guide#galaxy-s20note20-series-s20-s20-s20-ultra-s20fe-note20-note20-ultra)
  separates S10 repartitioner/cleaner steps from S20/Note20 preparation. The latter
  uses stock firmware/Odinpack recovery preparation, data reset and ROM sideload.
  That Android reset recipe is not an HFSX initializer or an iOS rollback method.
- The pinned [S20 configuration](https://github.com/ExtremeXT/ExtremeROM/blob/2056320784b7d1d88f9e7025f86f7bbd39945d6b/target/x1s/config.sh)
  specifies a **9,437,184,000-byte super** and a **9,432,989,696-byte logical group**.
  The physical size matches our measured profile. This supports reusing the existing
  partition rather than inventing a PIT resize for our 4 GiB APFS window. It does
  not certify every phone's current table; our backend still measures and rejects
  mismatched devices.
- Its pinned [ZIP builder](https://github.com/ExtremeXT/ExtremeROM/blob/2056320784b7d1d88f9e7025f86f7bbd39945d6b/scripts/internal/build_flashable_zip.sh)
  generates Android dynamic-partition metadata/operations with `lpmake` and OTA
  updater operations. Logical group/partition reconstruction inside super is
  distinct from changing a physical PIT/GPT. Our raw APFS layout instead destroys
  Android metadata in the written range, so return to Android needs a full stock
  super restoration, not an Android cleaner or BOOT-only restoration.
- Its device assertions use **`ro.boot.em.model`** and firmware assertions use
  **`ro.bootloader`**. Our backend was corrected to prefer those Samsung properties,
  with strict fallbacks and conflicting-bootloader rejection. A generic recovery
  product model cannot override a mismatched bootloader-supplied hardware model.
- Its pinned [S20 postinstall](https://github.com/ExtremeXT/ExtremeROM/blob/2056320784b7d1d88f9e7025f86f7bbd39945d6b/target/x1s/postinstall.edify)
  includes device-specific Android work. Those writes are not prerequisites for
  this port and were not imported into our protected-partition policy.
- Its pinned [build entry point](https://github.com/ExtremeXT/ExtremeROM/blob/2056320784b7d1d88f9e7025f86f7bbd39945d6b/scripts/make_rom.sh)
  produces a ROM ZIP by default. Our local full-build entry point now does the
  same; the private image workflow also uploads the final ZIP/checksum/provenance.

The saved wiki snapshot SHA-256 is `cff1fd1fa57234011bcc2678a95b57661564d493606453ddff269ac0d0d38802`.
Source-code references above are pinned; the wiki link is live and should be
checked if its instructions change.

## Reverification of our package

The current recovery image passed the supplied AOSP `avbtool verify_image` with
its recorded public test key: SHA256_RSA4096 vbmeta signature and the boot-head hash
verified. The covered head length is **2,445,312 bytes**, matching the legacy
working standalone head length (`0x255000`). This does not authenticate bytes outside
that head or prove sboot accepts the candidate. Whole-image checksums are separate.

Apple’s read-only APFS checker also passed both generated containers, and its HFS
checker passed a read-only, unmounted virtual disk reconstructed from the actual
format kit. The virtual device was detached and the sparse test file removed.
These checks are now part of ZIP assembly.

Host tests cover identity/property fallbacks, data preservation, explicit fresh
initialization, bounds, intact tails, installed-byte reuse and failures before BOOT.
Actual TWRP property values, mapper cleanup, physical keys, block cache ioctl/readback,
HFS mount, cold boot, interruptions and independent rescue still need a phone trial.
The user requested package/DevOps work only; no phone operations were performed.

See [rollback and stock restoration](rollback.md) for the two distinct recovery
paths and [TWRP](twrp.md) for explicit current limitations.

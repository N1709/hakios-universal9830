# z3s hardware test plan (kernel only)

This is the first hardware test of the z3s boot chain. It exercises:

```
sboot -> recovery -> BootShim -> z3s UEFI -> loader.efi -> XNU
```

It does **not** test iOS user space: `identity.h` is the placeholder, there is
no `system.apfs`, and `installer_validated` stays false. Keep
`TARGET_STATUS=unmaintained` until a hardware boot log and a GPT dump exist.

## Files

Release folder:
`/home/n1709/z3s-release/Jesvi-iOS_0.1.2_20261122_z3s-dt-avb/`

| file | size | use |
|---|---:|---|
| `recovery.img` | 69,009,408 | full `recovery` partition, AVB, **primary** |
| `boot.img` | 61,865,984 | full `boot` partition, AVB, alternative |
| `z3s-recovery.tar.md5` | 69,017,633 | Odin AP tar with `recovery.img` |
| `z3s-boot.tar.md5` | 61,870,113 | Odin AP tar with `boot.img` |
| `z3s-recovery.img` | 6,789,120 | boot-image head only, verifier input — **never flash** |
| `z3s-dtb-table.img` | 342,016 | Samsung DT table, magic `d7b7ab1e` |
| `build.json`, `SHA256SUMS`, `README.txt` | | hashes and record |

Rollback files (public pre-release
[`z3s-rollback-20261122`](https://github.com/N1709/hakios-universal9830/releases/tag/z3s-rollback-20261122)
and the laptop):

| file | size | use |
|---|---:|---|
| `lineage-recovery-vbmeta-z3s.tar.md5` | 69,027,873 | Odin AP, restore LineageOS recovery + `vbmeta` |
| `lineage-boot-z3s.tar.md5` | 61,870,113 | Odin AP, restore the LineageOS `boot` |
| `~/Downloads/twrp-boot-z3s.tar.md5` | 57,456,673 | TWRP on `boot`; built from the **old** TWRP 3.7.0 and expected to fail on `G988BXXSNHYB1` |

The official TWRP 3.7.0 z3s image (`~/Downloads/twrp-3.7.0_11-0-z3s.img.tar.md5`)
is **known bad** on the current bootloader: it carries the old 340,908-byte DT
table at `dtb_addr=0x11f00000`, so S-Boot shows the Samsung logo and reboots.
Do not reuse it. The replacement is the ExtremeXT `twrp_z3s` build
(`.github/workflows/build-z3s-twrp.yml`, branch `feat/twrp-z3s`), whose prebuilt
DT table is byte-identical to the current boot image.

The old `Jesvi-iOS_0.1.2_20261122_z3s/` folder is the raw-FDT build and is
marked **DO-NOT-FLASH-RAW-FDT.txt**. Do not flash it.

## Safety rules

1. Do not flash `recovery.img` and `boot.img` at the same time.
2. Do not flash the stock recovery. TWRP 3.7.1_12 now boots on hardware and is
   the backup path; the LineageOS recovery tar is the rollback.
3. **Extract the log before writing anything to `recovery`.** The loader log
   lives at `recovery+16 MiB` (8 MiB). A TWRP or stock recovery image is
   larger than 16 MiB and overwrites it.
4. Keep the phone above 50% charge, use a good USB cable, and do not
   interrupt Odin.
5. The partition map is measured (`/sys/block/sda`), but the on-disk GPT header
   reads as zero at LBA 1. `SUPER_TAIL_START=0` still makes the loader refuse
   every write inside `super`; do not enable writes there.
6. The modem stays off: the payload at `0xF0000000` overlaps `cp_rmem`.

## Phase 0 — baseline and hashes

```sh
adb devices
adb shell getprop ro.product.model      # SM-G988B
adb shell getprop ro.bootloader         # G988BXXUBDUI5
adb shell getprop ro.build.display.id

cd /home/n1709/z3s-release/Jesvi-iOS_0.1.2_20261122_z3s-dt-avb
sha256sum -c SHA256SUMS
stat -c '%n %s' recovery.img boot.img
xxd -s 0x626000 -l 4 recovery.img        # d7b7ab1e
```

Expected: model `SM-G988B`, bootloader `G988BXXSNHYB1` (the earlier
`G988BXXUBDUI5` is stale), sizes 69,009,408 and 61,865,984, DT magic
`d7b7ab1e`. If any hash fails, stop.

## Phase 1 — TWRP backup and layout record

1. Boot TWRP (do not flash the stock recovery). If TWRP is already installed,
   use `adb reboot recovery` or the key combo. The official TWRP 3.7.0 z3s
   image reboots at the Samsung logo on `G988BXXSNHYB1`; use the ExtremeXT
   `twrp_z3s` 3.7.1_12 build instead.
2. Back up `boot` and `recovery` to an external SD card or USB OTG. Use the
   TWRP GUI: **Backup -> Boot, Recovery**.
3. Pull the backup to the laptop and record its hashes:

   ```sh
   adb pull /external_sd/TWRP/BACKUPS/<serial>/<backup> ./twrp-backup-z3s
   sha256sum twrp-backup-z3s/*.win
   ```

4. Record the live layout from sysfs. The on-disk GPT header reads as zero at
   LBA 1, so `sgdisk --backup` and `dd` of sectors 0..33 do not produce a usable
   table:

   ```sh
   adb shell 'for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32; do
     echo "$n $(cat /sys/block/sda/sda$n/start) $(cat /sys/block/sda/sda$n/size)"; done'
   adb shell 'ls -l /dev/block/by-name'
   ```

   The measured map (2026-11-22, 128 GB unit) is in
   [`README.md`](README.md#partition-map-pit). `super` ends where `prism`
   starts, and the dynamic group has 4,826,603,520 bytes free in its tail.

5. Keep the backup. It is the rollback if the HakiOS image does not boot.

## Phase 2 — primary test: `recovery`

1. Put the phone in Download Mode: power off, hold **Volume Down + Side key**,
   then **Volume Up** to confirm.
2. Odin: put `z3s-recovery.tar.md5` in **AP**. Do **not** tick Repartition and
   do not add BL, CP or CSC. Start and wait for **PASS**.
3. Let the phone reboot. To select the recovery partition, either:

   ```sh
   adb reboot recovery
   ```

   or power off and hold **Volume Up + Side key** until the Samsung logo.

4. Watch the screen and record it with a photo or video. Expected sequence:

   ```
   Samsung logo
   -> Silicium/UEFI logo
   -> loader messages
   -> XNU kernel (placeholder identity: it will not reach iOS user space)
   ```

   Wait at least **90 seconds** (50 s silence budget + 30 s watchdog + boot).
   Do not press buttons; let the watchdog reset the phone.

5. Outcomes to record:

   | screen | meaning |
   |---|---|
   | `DT LOAD FAIL` / `FDT_ERR_BADMAGIC` | wrong image or DT table not at the DTB offset |
   | Download Mode right after the Samsung logo | sboot rejected AVB/secure boot |
   | UEFI logo, then black/reboot | loader or kernel issue; the log has the details |
   | kernel panic text | capture it; the log is in `recovery+16 MiB` |
   | Android boots normally | recovery was not selected; use `adb reboot recovery` |
   | HakiOS/UEFI menu | the chain reached the loader; record it |

## Phase 2b — fastbootd variant

TWRP's fastbootd writes raw partitions, so it can replace Odin for the flash
itself. It does **not** replace the TWRP backup: do Phase 1 first.

1. Boot TWRP, then **Reboot -> Fastboot** (or `adb reboot fastboot`).
2. Check the device and the partition sizes:

   ```sh
   fastboot devices
   fastboot getvar partition-size:recovery   # 0x41d0000 = 69,009,408
   fastboot getvar partition-size:boot       # 0x3b00000 = 61,865,984
   ```

3. Flash the **full** image only:

   ```sh
   cd ~/Downloads/Jesvi-iOS_0.1.2_20261122_z3s-dt-avb
   sha256sum -c SHA256SUMS
   fastboot flash recovery recovery.img
   ```

   `recovery.img` is 69,009,408 bytes and ends with the AVB footer. Never
   flash `z3s-recovery.img` (6,789,120 bytes): it is the boot-image head only,
   with no footer, and S-Boot will reject it.

4. Reboot to recovery and wait 90 seconds:

   ```sh
   fastboot reboot recovery
   ```

   If this fastbootd does not support `reboot recovery`, use
   `fastboot reboot` and hold **Volume Up + Side key**.

5. Once adb is available (TWRP or rooted Android), check the AVB footer:

   ```sh
   adb exec-out "tail -c 64 /dev/block/by-name/recovery" | od -An -tx1
   ```

   The last 64 bytes must contain `41 56 42 66` (`AVBf`).

**Fastbootd only exists while TWRP is running.** After the HakiOS image is on
`recovery`, there is no fastbootd to return to: rollback goes through Download
Mode + Odin (`lineage-recovery-vbmeta-z3s.tar.md5` restores LineageOS recovery).
Extract the log before writing a recovery image; any recovery larger than
16 MiB overwrites `recovery+16 MiB`.

## Phase 3 — extract the log

The loader writes the log to `recovery+16 MiB` before the watchdog reset.
Android never writes `recovery`, so a normal Android boot keeps it.

1. Let the phone reset and boot Android. Connect USB.
2. If Android is rooted:

   ```sh
   tools/z3s-log.py --adb --su --output z3s-log-$(date +%Y%m%d-%H%M).bin
   ```

3. If Android is not rooted, do **not** flash TWRP to `recovery`: a 57 MB
   TWRP image overwrites the log window. First try `adb root` (userdebug
   builds). Booting TWRP from `boot` also works once the ExtremeXT `twrp_z3s`
   build exists: its `recovery.img` can be packed as `boot.img` and flashed to
   `boot`, which never writes `recovery`. The old
   `~/Downloads/twrp-boot-z3s.tar.md5` (MD5 `7f1d2bb9…`) is built from TWRP
   3.7.0 and is expected to reboot at the Samsung logo on `G988BXXSNHYB1`; do
   not rely on it.

   After a TWRP-on-`boot` test, restore Android with Odin AP =
   `lineage-boot-z3s.tar.md5` (public pre-release `z3s-rollback-20261122`).
4. If the phone booted TWRP (only possible if a working TWRP is on `recovery`),
   `adbd` is already root:

   ```sh
   tools/z3s-log.py --adb --output z3s-log-$(date +%Y%m%d-%H%M).bin
   ```

5. Copy the log to the laptop and the brain, and hash it:

   ```sh
   sha256sum z3s-log-*.bin
   ```

## Phase 4 — alternative test: `boot`

Use this only if the `recovery` path is rejected before the loader, or after
the recovery test has left the small HakiOS image on `recovery` (so the log
window at `recovery+16 MiB` is free). Do **not** run this with TWRP on
`recovery`: the log write would corrupt TWRP.

1. Flash `z3s-boot.tar.md5` in Odin **AP**, or `boot.img` from fastbootd:

   ```sh
   fastboot flash boot boot.img
   ```

   Do not touch `recovery`.
2. Reboot normally. The phone boots the `boot` partition.
3. Wait 90 seconds and record the screen.
4. Restore Android with the LineageOS boot tar in Odin **AP**:
   `lineage-boot-z3s.tar.md5` (public pre-release `z3s-rollback-20261122`;
   MD5 `a9cee29f4f162b4b669b35d2c8779e7d`; built from the phone's own `boot`
   dump, SHA-256 `49b2a637…69a2`). If fastbootd is still up, the raw image
   works too:

   ```sh
   fastboot flash boot boot.img
   ```

   Do **not** flash a stock recovery. This leaves the log in
   `recovery+16 MiB` intact.
5. Boot Android and extract the log as in Phase 3.

The boot path has no Android to fall back to until the LineageOS `boot` is
restored, so treat the screen as the primary evidence.

## Phase 5 — rollback

HakiOS on `recovery` replaces whatever recovery was there. Keep the rollback
tars on the laptop before starting. They are published in the public
pre-release
[`z3s-rollback-20261122`](https://github.com/N1709/hakios-universal9830/releases/tag/z3s-rollback-20261122):

```text
lineage-recovery-vbmeta-z3s.tar.md5  69,027,873 B  MD5 dac275a2fb190259626d97bda82836bf
lineage-boot-z3s.tar.md5             61,870,113 B  MD5 a9cee29f4f162b4b669b35d2c8779e7d
```

1. **Extract the log first** (Phase 3). Any recovery image overwrites
   `recovery+16 MiB`.
2. Power off. Hold **Volume Down + Side key**, then **Volume Up** for Download
   Mode.
3. Odin: AP = `lineage-recovery-vbmeta-z3s.tar.md5` (restores LineageOS
   `recovery` + `vbmeta`). Do not tick Repartition and do not add BL, CP or CSC.
   Start and wait for **PASS**.
4. If `boot` was also changed, flash AP = `lineage-boot-z3s.tar.md5`.
5. Reboot and verify:

   ```sh
   adb shell getprop ro.product.model
   adb shell getprop ro.bootloader
   ```

Do **not** use the official TWRP 3.7.0 z3s tar for rollback. It carries the old
340,908-byte DT table at `dtb_addr=0x11f00000` and reboots at the Samsung logo
on `G988BXXSNHYB1`. The replacement is the ExtremeXT `twrp_z3s` build from
`.github/workflows/build-z3s-twrp.yml`; use it only after its `recovery.img`
has booted on the phone.

If TWRP is running and fastbootd is available (only before HakiOS is written to
`recovery`), raw partitions can be flashed with `fastboot flash recovery ...`
instead of Odin. It overwrites `recovery+16 MiB`, so extract the log first.
After HakiOS is on `recovery`, fastbootd is gone; use Download Mode and Odin.

## Why not a menu inside recovery?

S-Boot loads one Android boot image from `recovery` and jumps to its kernel.
A menu inside that image can only run code already in the image. TWRP is a
second kernel + ramdisk (57 MB), so it cannot be a menu entry in the same
image unless the HakiOS loader chainloads it from another partition.

The workable dual-boot layout is:

| partition | contents |
|---|---|
| `boot` | HakiOS (6.8 MB head, log at `boot+16 MiB` in the free space) |
| `recovery` | TWRP (57 MB) |
| loader menu | "Reboot to TWRP" -> Samsung reset reason `0x12345604` |

This needs two changes before it is safe:

1. Move the log from `recovery+16 MiB` to `boot+16 MiB`; otherwise the loader
   would write into TWRP's image and corrupt it.
2. Test the `boot` path first, then add the loader menu entry and the
   `reboot recovery` action (the loader already has
   `reboot_samsung(0x12345604)`).

Until then, the recovery-first test and the TWRP tar above are the safe path.
Do not flash both `boot.img` and `recovery.img` at once.

## What counts as progress

- **sboot passes the DT table**: no `FDT_ERR_BADMAGIC`.
- **sboot passes AVB**: no immediate Download Mode.
- **UEFI logo appears**: BootShim and the FD are placed correctly.
- **loader prints**: the kernel payload and cmdline are readable.
- **kernel starts**: the log contains loader/kernel output; a panic is still
  useful evidence.
- **Android/TWRP still boots after the test**: the rollback path works.

A successful kernel start is **not** an iOS boot. Do not mark the device
maintained or the installer validated until there is a hardware log, a GPT
dump and a working rollback.

## Evidence to save

- photos/video of every screen, especially error text;
- `z3s-log-*.bin` and its SHA-256;
- Odin log and the exact tar/image hashes;
- TWRP backup hashes and the GPT dump;
- phone model, bootloader and firmware;
- date/time and what was flashed.

Update `docs/devices/z3s/README.md`, `build.json` (`hardware_tested` stays
false until then) and the brain note with the result.

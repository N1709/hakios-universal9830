# Latest accepted baseline before auxiliary tests — 2026-11-15

User requested incorporating all newer logo, AOD, dev/release-mode, battery/sleep
and system changes before another camera test. Branch `feat/s20-rear-aux` merged
actual accepted master history with commit `bbc8e510`. Earlier equivalent boot
changes had been cherry-picked separately; this merge establishes ancestry too.

## Source and timestamps (IST, +05:30)

| Item | Verified version/time |
|---|---|
| local main |45820593,2026-11-15 16:57:59 |
| local master |45820593,2026-11-15 16:57:59 |
| private Jesvi-iOS main/master, freshly fetched |45820593, same commit |
| branch merge |bbc8e510,2026-11-15 21:15:45 |
| newly rebuilt candidate kernel |2026-11-15 21:17:15 |
| live kernel before testing |2026-11-15 16:31:08 |
| accepted staged root.apfs file mtime |2026-11-15 12:36:39 |
| accepted staged system.apfs file mtime |2026-11-14 23:20:36 |

Git ancestry checks pass for master and the freshly fetched private remote master.
This includes the JesviOS/-JESVI- boot changes, logo scaling, DEVMODE development
menu and JESVI_TARGET release override, AOD30%-of-slider dimming, screen-off small
cores, governor tick, network/Wi-Fi/USB polling and sleeping flashlight thread.
It includes the root packing guard against losing com.redwood.telephonyd.
Experimental installer work on release/exynos990-devops remains independent;
its unfinished splash packaging is not a prerequisite for this kernel-only test.

The idle wake-tracing hook is tracked in BOTH main and master in xnu/redwood.patch,
introduced by a24b7a63 at11:44:14. Correction to initial progress wording: the
nested build-tree thread_call.c already had that hook (file timestamp11:46:09);
it was confirmed, not newly patched during this integration. Five build overlay
files actually needed refreshing: redwood_net, bcm4375, exynos_acpm, dwc3, audio.
All301 tracked overlay files now match the build tree. A reverse-check of the
ENTIRE tracked redwood.patch passes, proving its hunks are present without
mutating the tree. No unrelated worktree changes were exported or committed.

## Installed image baseline

Exclusive phone lock acquired after A's handoff. Backed up the entire69009408-byte
recovery image. Root extracted using the ACTUAL loader rsize payload entry,
not an assumed fixed root size. Root13467648bytes is byte-identical to the latest
accepted staged root. SHA256:
`7a438d1f7c7d32c7451fdb79c3849fb22218073ec8338240b651ad7bcdeea252`.
It contains the current telephonyd job, native keybagd and motion/AOD services.
Original front-camera system library is unchanged: SHA256
`fcc5cef59533ad963aa05dfee622e554afb876bc20ed638c3e47d981a836a7ce`.
Native keybag read shows protected/locked flags0x5 after prior first unlock.
Live scroff_little=1; awake display_aod=0. No authentication state changed.

Phone and latest staged loader payload settings match (including devmode1 and
boot line). Loader.efi differs in exactly TWO bytes, both within the PE COFF
build timestamp; its code/content otherwise matches. The signed image headers
therefore differ too. Preserve the PHONE'S current loader/UEFI/payload rather
than replacing them merely to equalize timestamps. Current phone root is exact.

The base system is intentionally older than today's kernel/root changes. Its
APFS superblock matches byte-for-byte, SystemVersion.plist matches, and live
keybagd, SpringBoard and Camera executables hash-identically to the staged
system. RedwoodMetal is a runtime symlink into the small root, whose full image
is verified; it is not a missing base-system driver to replace. A full2.66GB
raw-device hash scan was stopped because it kept a CPU busy and was too slow.
Its partial digest is labeled INCOMPLETE and proves neither equality nor a real
mismatch. Full-device system equality is NOT claimed. No system/data writes.

## Candidate and validation

Built on the merged source: all three auxiliary commands present, GICv2 build
succeeds. Passcode integration; AOD root assembly, framebuffer fade/wake races,
swap-power dispatch and initializer ownership;11 inventory/catalogue/routing
Python tests; three ASan/UBSan rail/table fault-injection suites pass. Shell syntax
checks pass. The existing front path and display policy are preserved.

Candidate `kernel-slot-aux-latest.bin` is16MiB, SHA256:
`15d996b7e22abd719c79e3daa5b7a65ed64ac06a7601e56a4c45a5c1e4f747dd`.
Stripped kernel15212880bytes, SHA256:
`dccc24263c0f2c26eb116e3026eb48e5fd05bc2ff81263fe1473dd7886159e84`.
Current kernel rollback slot SHA256:
`e5ec842f99a017e1891986de5021b96ab12c9bd42ad73ee56768927a29220c6f`.
Full pre-test recovery SHA256:
`5f00a3dd1c2d809160eb4161cf96feaf79422a451daab8b5ad9cd6bd27f16165`.

Private binaries, full image, manifests and logs are outside git in
`<workspace>/build/camera-aux-latest-20261115/` (directory0700).
Never use earlier pre-idle-power candidates from camera-aux-20261115 for this test.

Pending device test: after explicit NEW reboot approval, write only the kernel
slot via loader, verify readback, boot once. Preserve current loader/root/system/
data. Confirm protected bag before first unlock, original front/main operation
and normal repeat locking. Close Camera and wait20seconds, run2LA ID, then its
bounded A20212MP sensor-counter test, then GW2 ID only after successful cleanup.
No auxiliary image/zoom controls are implemented yet. No further reboot is
implied by old approvals. Update this document and handoff with actual outcomes.

## Live test outcome after user approval

Installed this exact candidate via loader kernel-only write/readback and one
approved cold reboot. Running kernel21:17:15IST confirmed.2LA native4032×3024
sensor-counter test passes (11 advances, stop verified, cleanuptrue). GW2 ID
returns0972/a300 with cleanuptrue and unchanged S2MPB02 map. User confirms front,
main rear and repeat passcode behavior. First host bag status4 was already
unlocked/protected, so this boot's before-first-unlock state was not observed.
No CSI/DMA RAW image from either auxiliary camera yet. Further implementation
continues with bounded receiver capture before any lens controls/zoom UI.

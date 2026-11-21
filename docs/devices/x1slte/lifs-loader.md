# Apple's lifs.kext in our kernel (A, 2026-11-20)

Why: Files shows external drives through userfsd + livefileproviderd + Apple's user-space file systems
(livefiles_exfat/msdos). livefileproviderd needs Apple's **lifs.kext** (IOKit `com_apple_filesystems_lifs` on
IOResources, user client `AppleLIFSUserClient`); without it it exits ("Didn't find matching client") and userfsd waits
forever (docs/devices/x1slte/sdcard-files-parked.md). Our kernel cannot load kexts. Instead of rewriting lifs, load
Apple's own, from the phone's system volume (`/System/Library/Extensions/lifs.kext/lifs`, 280 KB, iOS 16.5 20F66).

## Why it is small
- MH_KEXT_BUNDLE, arm64 (not arm64e: no pointer authentication), segments from vmaddr 0.
- **452 imports, all present in our kernel** (xnu-8796.121.2 = iOS 16.5's XNU; `nm -u lifs` vs `nm -g mach.x1slte`).
- Relocations are all ARM64_RELOC_UNSIGNED, 64-bit, not pc-relative: 664 external (GOT + pointers: symbol + addend)
  and 286 local (pointer += load address). Code reaches imports through __stubs/__got: no instruction patching.
- Entry: `_kmod_info` (D) -> `__start` -> `__realmain` (lifs_kext_start); IOKit classes from __mod_init_func.

## Loader (bsd/dev/redwood_kextload.c)
1. A boot job hands the file to the kernel (sysctl write); the kernel parses it (MH_KEXT_BUNDLE, arm64 only, bounds).
2. Allocate wired kernel memory for all segments; copy them; apply local and external relocations, resolving names
   through a table generated at build time from the import list (tools/kext/lifs-imports.txt -> asm-label table;
   names only, no Apple code in the repo).
3. Text pages: read-only + executable (a pmap helper clears PNX on exactly those pages; pmap_protect never does);
   flush data cache, invalidate instruction cache and TLB.
4. OSMetaClass::preModLoad("__kernel__") -> __mod_init_func constructors -> postModLoad (classes owned by the
   kernel's own OSKext); kmod_info.start(&kmod_info, NULL).
5. The personality from lifs.kext's Info.plist to gIOCatalogue->addDrivers(): IOKit matches it on IOResources (IOBSD).
Behind boot-arg `lifs=1` (default off until it works).

## Then
livefileproviderd stays up -> userfsd answers -> addDisk with a disk node of the card's exFAT partition -> Files.

## Result (2026-11-21)
Apple's lifs loads, links and starts in our kernel: `com.apple.filesystems.lifs 1.0.0d1`, 288 KB, 286+664
relocations, 35/35 typed views; IOKit service `com_apple_filesystems_lifs` registered/matched/active.
livefileproviderd then starts properly ("set notification port returned 0", "will mount over LIFSv2", FileProvider
domains) and userfsd completes: "LiveFSService XPC service for externally attached volumes is up", "registering for
IOKit matching notification". Bugs found on the way (each froze the phone):
1. write_pte_strong fills the 4 hardware entries of a 16 KB page itself: one call per VM page.
2. __kalloc_type holds the views (64 bytes each), not pointers.
3. XNU builds every dynamic-memory table with PXNTable: cleared on the tables above the text pages, full TLB flush.
4. The personality must name `__kernel__` as owner (else IOKit asks user space to load the kext).
Test: boot-arg lifs=1, `kern.redwood.lifs_load=1` (once a boot); lifs_stop/lifs_text for staged runs.

## Next
The SD card as an external disk in IOKit, the way userfsd's matching expects (IOMedia-like service, BSD name,
Physical Interconnect External), a /dev node for it; then lifs at boot (lifs=2) by default.

## State 2026-11-21 01:00 (A, end of session)
Default boot now: lifs loads at boot (kernel default lifs=2; `lifs=0` off) -- boots fine (44 s); SD probed at boot,
`/dev/disk9` `rdisk9` (card, 121941 MB) and `disk9s1` `rdisk9s1` (exFAT 0x07 at LBA 2048) published, and IOKit
`IOBlockStorageDevice` (Protocol Characteristics: SATA / External) -> `IOMedia disk9` -> `IOMedia disk9s1` (Leaf).
userfsd (started by hand: `launchctl kickstart user/foreground/com.apple.filesystems.userfsd`) comes fully up, but:
- nothing appears in /private/var/mobile/Library/LiveFiles, no UVFSService runs; its IOMediaMatchingCallback lines
  (debug level?) are not in the stream -- did getBSDNameIFExternalMedia skip disk9s1? (strings: skipping non-leaf,
  EFI, BOOT_UUID, "not apple card reader")
- launchd did not start userfsd on its own (its LaunchEvents com.apple.iokit.matching; check launchd's IOKit events)
- livefileproviderd logs "will mount over LIFSv2" but no lifs mount shows in `mount`, then exits on its idle timer.
Next, in order: (1) oslogq with debug level for userfsd's scan (or disassemble getBSDNameIFExternalMedia: which
properties, which parent walk); (2) try the USB shape (IOBlockStorageDevice under a removable "IOUSBHostDevice") if
External SATA is refused; (3) the LiveFiles lifs mount (mount_lifs / livefileproviderd's mount call result);
(4) start userfsd at boot (its own job, RunAtLoad) once it registers the card. Tools: tools/oslogq, tools/sdcard.

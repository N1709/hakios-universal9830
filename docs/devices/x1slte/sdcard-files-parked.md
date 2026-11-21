# microSD in Files (parked 2026-11-20, A)

State: the kernel reads the card (`/dev/rsdcard`, bsd/dev/redwood_sdcard.c, read-only, on by default; see
docs/devices/x1slte/sdcard.md). What is missing is the Files side.

## Native route (being tried)
iOS 16 has userfsd (UserFS.framework, user domain `user/foreground`, on demand) with Apple's user-space file
systems as plugins in the dyld cache: `livefiles_exfat.dylib`, `livefiles_msdos.dylib` (knownPlugins.plist). Its XPC
interface is the `LiveFSVolumeManager` protocol (LiveFS.framework), types read on the phone:

    listVolumes:                   v@?<v@?@"NSDictionary">
    addDisk:reply:                 @"NSString", @?<v@?@"NSError">
    addDisk:fileSystemType:reply:  @"NSString", @"NSString", @?<v@?@"NSError"@"NSArray">
    ejectDisk:usingFlags:reply:    @"NSString", I, @?<v@?@"NSError">

userfsd's entitlements include com.apple.private.LiveFS.connection; sdmount.ent gives our tool that.
`LiveFSMountClient` (mountVolume:...) is the kernel-mount side (lifs.kext: not loadable here) -- not needed for Files.

**Where it stopped:** `sdmount list` (NSXPCConnection to mach service com.apple.filesystems.userfsd, run as a
user/foreground launchd job as mobile -- root/sudo/launchctl asuser do not reach the user domain) gets no reply in
20 s, and userfsd logs nothing in rwlogd. Next: read userfsd's os_log (oslog tooling), check whether the listener
needs LiveFSClient's own handshake (`+[LiveFSClient newConnectionForService:]`), then `addDisk` with a devfs name
userfsd expects (likely `diskN` / `rdiskNs1`: add those nodes in redwood_sdcard.c, partition 1 at the MBR offset).

Tools: tools/sdcard/. Build: `xcrun clang -target arm64-apple-ios15.0 -isysroot ~/theos/sdks/iPhoneOS16.5.sdk -fobjc-arc -framework
Foundation -o sdmount sdmount.m && ldid -Ssdmount.ent sdmount`.

## Found 2026-11-20 (evening): why userfsd never answers
With a live log (tools/oslogq: LoggingSupport's live stream) userfsd stops at `cleanUpStateIfNeeded:start`: it asks
livefileproviderd (`machp://com.apple.filesystems.livefileproviderd`, the mounter) about kernel mounts, and
livefileproviderd exits at start: `main:lifs:v2:1`, "Didn't find matching client", "get client returned e0000001",
"sending mach port to kernel: -536870911" -- it needs **Apple's lifs.kext** (LiveFS user client). So the native route
is: a lifs stand-in in our kernel (the user client livefileproviderd hands its port to, and a VFS that forwards vnode
operations to it), then userfsd/addDisk work. That is a kernel project of several days. livefileproviderd is denied
in userland/mkjobs.py meanwhile (it restarted every 10 s).

## Fallback
Our own File Provider extension ("SD Card" location) on the hidden Files host, using livefiles_exfat.dylib's plugin
API directly on /dev/rsdcard. 1-2 days.

## After either
IDMAC (DMA), write support (CMD24/25) behind its own switch, card detect (GPA1-5), clean-install integration.

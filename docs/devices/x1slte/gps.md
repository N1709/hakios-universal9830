# S20 GPS implementation status

2026-11-16, feat/s20-gps, based on accepted master735b41e0 (camera completion).
**Real satellite fixes verified in the host-assisted lab. No standalone GPS service
or verified Core Location provider is shipped.**
The accepted camera and authentication source/root image are preserved. A GPS-only
hub polling change is under device test; this is not a shipped GPS implementation.
The previous feat/s20-fingerprint branch is preserved separately.

## Current phone state / next work

### Continuation: real acquisition verified, native delivery still pending

The existing09:15:25 kernel remains installed; this continuation did not reboot
the phone or write boot/root/kernel images. Indoor receiver acquisition succeeded
in private lab session `out/gps/private/lab/share/session-1794806033/`:
252GGA and252RMC reports,190valid of each, maximum8satellites used, peak reported
C/N0 40dB-Hz, and settled HDOP about1.2. Independent parsing checked checksums and
fresh dates/times; the last live fix checked was under one second old. A rough
user-supplied reference point was consistent with the captured fixes. Reference
coordinates and receiver positions remain private and are never substituted for
receiver measurements.

Two setup gaps were addressed: stock x1slte `LhePinMux=P17=M1` is available as
the relay's explicit `--s20-lna` option (RPC34, bytes17,1); and stock `ref_time`
accepts `week` and millisecond `tow`, not the previously attempted `utc` field.
The lab launcher optionally supplies the VM's real current time, with5second
uncertainty. Its GPS-UTC assumption is restricted to2026 and must be rechecked
before later use. See [USNO's offset](https://www.cnmoc.usff.navy.mil/Our-Commands/United-States-Naval-Observatory/Precise-Time-Department/Global-Positioning-System/USNO-GPS-Time-Transfer/Leap-Seconds/)
and [IERS Bulletin C72](https://datacenter.iers.org/data/html/bulletinc-072.html).
This is time assistance, not a position or successful-fix injection.

`tools/gps/lab-session.sh` mounts the lab runtime's device/proc paths, starts its
FIFO reader before gpsd can emit startup commands, refuses non-FIFO endpoints,
and cleans up only its own children. An earlier race lost startup commands; the
phone relay now rejects navigation data arriving without an open channel. It
also validates the incoming MEIF stream before forwarding it. The following
130second session had6882valid packets, zero malformed packets, acknowledged
navigation stop and no reported hub exception, but did not regain a valid fix.
Repeat acquisition is therefore not yet qualified.

USB ECM went inactive during two longer tests. A serial `usb restart` restored
the USB link without rebooting iOS; a user cable reconnect was also tested.
The successful long session's final navigation-stop acknowledgement was not
captured after link loss. Subsequent bounded sessions and locked native MKB
state1/AKS0x5 were checked. Do not claim camera or repeat-passcode regression
coverage from these checks.

Native investigation verified `CLAccessoryLocationProviderAdapter`, its adaptee
and `CLDispatchSilo` on the device. `location-abi-probe.m` is metadata-only.
`location-nmea-lab.m` and `nmea-lab-send.c` are an **unqualified prototype** for a
root-only local, fresh-RMC/matching-GGA bridge into Apple's accessory parser.
Host ASan/UBSan gate tests reject stale/future, mismatched, invalid, estimated
and bad-checksum input. No app delivery has been demonstrated. Temporary
locationd launches exposed signing/fixup and proxy-dispatch startup failures;
the runtime injection configuration was restored after each attempt and normal
locationd recovered. Do not deploy this prototype as a boot service. Next resolve
the actual native service lifecycle/protocol before another delivery trial.

Apple Maps' blank world is a separate open issue. All89stock app files and the
GeoServices/VectorKit/MapKit resource sets were compared with the source root DMG
and installed paths. The phone fetched Apple's Maps configuration over TLS with
HTTP200; GeoServices has downloaded resource manifests. This does not prove tile
downloads or rendering. No Maps data, permissions or caches were erased.

Next: finish native delivery and demand/lifecycle handling, reproduce acquisition,
fix the blank map, and remove the Linux/Mac engine dependency. A real lab fix does
not make the port's GPS standalone or make Maps functional.

The older chronological handoff below records the earlier no-fix bring-up.

Phone lock: codex-gps. The user approved recovery and further GPS test reboots in
this session. The unchanged installed07:42:36 IST camera kernel rebooted, the user
confirmed the iOS lock screen, and increasing motion counters verified hub recovery.
A kernel-slot-only test update was read-back verified by the loader at09:20 IST
(approximate); root, loader/boot profile and data were not written. The new kernel
build is09:15:25 IST. Recovery copy: workspace build/camera-gw2/permanent/kernel-slot.bin.

The first768-byte ROM read triggered host_com_rx.cpp assertions. After recovery,
2048 reads of16 bytes captured ROM0x20000..0x27fff (32768 bytes) with sensors
continuing to update. SHA256:
8b11e8772133d3624da5381a930d3de1ce9403b94dbbe4d5539c1c0df80e5df7.
The reader refuses larger requests and aborts on firmware exceptions.

**Live navigation handshake passed twice:** RPC1C start, RPC1E carrying stock
gpsd's14-byte pass-through command, then a valid MEIF state-report10 and power
statistics, followed by acknowledged RPC1C stop. The repeated session decoded
six navigation packets, including an8-byte ACK, with no malformed frames or
firmware fault. That proves the navigation channel and this one state transition,
not satellite acquisition or RF configuration.

The actual8-byte ACK corrected an assumption from static research: MEIF packets
can have a6-byte header plus checksum. They do not all have an8-byte header.
The portable decoder and its independent live-ACK test now cover this.

A read of the actual GPS29 descriptor's ops pointer returned0x82dba0, proving that
its callback table is installed despite its absent advertised scan bit. A paired
SSP sensor29 start/stop completed without exceptions; it produced no GNSS control
request or fix, and its GPS reference counter was zero afterward. Do not equate
SSP transport completion with the application callback executing.

An isolated Linux ARM64 VM on the Mac runs stock Samsung gpsd using its own
Android11 linker/libraries. A temporary USB-only relay connects its actual LHD
commands to the existing XNU hub owner. Two bounded relays reached measurement
engine patch upload but hit gpsd's10.5-second initialization watchdog. Sensor
updates continued, with no hub exception. No NMEA or satellite fix was produced.
A third display-awake test roughly doubled traffic (201 transmitted hub chunks,
299 received packets), but still timed out during patch upload. This supports a
latency problem without proving polling is its only cause.

The kernel previously slept100ms between polls with the display off, including
when a GNSS client had a pending command. The test change keeps the existing4ms
running interval while any GNSS descriptor is open, and restores the original
idle policy after the last close. The actual pump function passes a host fake-clock
sanitizer test for inactive/active/multiple clients and early completion. Both
tracked and generated build sources match. The accepted redwood.patch was checked
against the generated tree with a temporary Git index; stale overlay files were
backed up privately and synchronized from tracked source before compiling.

Device test of09:15:25 kernel: native MKB lock state1, AKS flags0x5 (locked),
sensors0/1/4/5 started normally. Display-off relay reached198 transmitted chunks
and294 received packets with acknowledged navigation stop and no hub exception,
but still hit the stock watchdog. This matches the display-awake throughput, so
the remaining latency is outside the fixed display-off hub sleep.

A subsequent host-VM-only experiment extends the decoded wait4init timer from
10000ms to60000ms (ELF offset0x372f24, original word0x5284e204). It preserves
RF/fix logic and does not modify/deploy a stock binary on the phone. This tests
whether the patch sequence can finish across the temporary laboratory transport.
No position is fabricated. **RelayE completed successfully:**911 transmitted hub
chunks,3835 received packets, acknowledged navigation stop, no hub exception.
It produced95 GGA reports and95 RMC reports over the bounded session, all invalid;
maximum satellites used0. This proves measurement-engine startup and NMEA output
across the lab bridge, not satellite reception or a real fix. Native MKB remained
locked1/flags0x5 afterward. Lab gpsd/bridge processes were stopped.

Next: verify acquisition with clear sky, inspect RF/antenna/clock setup and eliminate
the host dependency. Core Location registration remains unimplemented.

The temporary relay and Linux lab are bring-up tools, not a native iOS GPS service.
Stock daemon/runtime, captures and decompilation remain in ignored out/.

## Confirmed boundaries

Stock S20 vendor binaries `lhd` and `gpsd` have been partially decompiled; detailed
local research and private Ghidra artifacts are in ignored
`proposals/gps-research-20261116/`. Do not add firmware/decompiled binaries to git.

- Hub scan returned000577771f07f6bf; firmware version23030400.
- Advertised scan bit29 (GPS) is absent, but its installed callback table was
  verified in RAM. Firmware contains a descriptor29 GPS
  initializer at0x82d9ae, start/stop callback table0x82dba0, location callback
  0x82d694 and shared buffer0x90f7a8. Do not assume the advertised scan mask
  represents all registered virtual sensor descriptors.
- Stock lhd encodes RPC1C as navigation-channel Start(bool), RPC1E as host-to-hub
  bytes and RPC1F as hub-to-host bytes.1E/1F bodies contain a LE16 byte count.
  A transport acknowledgement is not evidence of acquisition or a fix.
- RPC7 MemoryRead body is bool,u32 address,u16 length; RPC8 response is
  u32 address,u16 length,data. The small successful result contained a ROM vector
  table starting with SP0x00913a30. Large-read compatibility is explicitly failed.
- Inner MEIF startsA5 93, endsA5 68, doubles literalA5, and uses an additive LE16
  checksum. This framing is distinct from outer BBD packets/CRC and from the
  gpsd/lhd daemon connection. Stock message0x8183 configures MicroNav, but its
  flag meanings and complete required startup sequence remain unresolved.
- Actual iOS16.5 locationd includes an accessory/NMEA provider. Its private ABI
  and genuine fix delivery have not been implemented or verified. Compass fusion
  support does not establish GPS support.

## Source/tools added

`tools/gps/gnss_protocol.h` implements bounded pure MEIF decoding/encoding and the
RPC1E body boundary. It performs no device I/O or positioning. Its tests use the
literal passthrough-control packet embedded in stock gpsd as an independent input.

`tools/gps/gnss-read.c` captures fixed firmware windows using the existing
`/dev/redwood-gnss` SPI-owner interface. It never writes firmware memory or resets
hardware. A reply may precede the kernel's acknowledgement record, so it waits
for both. It refuses oversize reads before I/O and aborts on a firmware exception.
Output creation is exclusive, no-follow and0600. Firmware files remain private.
The program is an experimental bring-up tool, not a boot service.

Host checks:

```sh
clang -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/gps/test_gnss_read.c -o out/gps/test-gnss-read
out/gps/test-gnss-read
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/gps/test_protocol.c -o out/gps/test-protocol
out/gps/test-protocol
```

Both passed; protocol cases cover the stock frame, split escaping, bad checksum,
size/overflow recovery and nested navigation length. Reader checks cover pre-I/O
size refusal and the actual captured reply fields. The reader cross-compiles for
arm64 iOS16.5. Existing passcode build-integration test also passed; no build/profile
changes were required.

## Next implementation steps

1. Preserve the recovered stable camera image. Continue from the verified
   pass-through handshake and the isolated stock-engine lab.
2. Recover the actual B1 ROM codec/controller state and the allowed response
   size using small, bounded requests. Large bulk-read attempts are disallowed.
3. Resolve stock GNSS-channel/pass-through initialization and RF/clock config.
   A native session must stop/clean up through the same SPI-owner thread and
   abort on exceptions, dropped records or protocol-state mismatch.
4. Prove real outdoor acquisition, fresh timestamps, validity and accuracy.
5. Add a demand-driven native service and real Core Location provider path;
   retain authorization, stale-fix rejection, fallback and paired start/stop.
6. Validate cold/warm starts, repeat sessions, screen-off/wake, airplane mode,
   power consumption, camera/motion regressions and passcode repeat locking.

Do not replace steps4–6 with simulation or fabricated coordinates. Ubuntu Touch/
Halium references keep Android vendor interfaces; they do not supply a demonstrated
native XNU implementation for this chip. The cached stock vendor image is593920
bytes short of its declared ext4 size and must be re-extracted from an intact
archive before being used for an Android runtime lab.

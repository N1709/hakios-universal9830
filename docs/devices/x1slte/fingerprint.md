# S20 ultrasonic fingerprint research

## Status (2026-11-14, handover after fresh registration)

**Enrollment and saved-template restoration work; matching and iOS fingerprint
unlock do not.** Read [FINGERPRINT-HANDOVER.md](../FINGERPRINT-HANDOVER.md) first.

The last kernel verified by this session was `7e8181fe`, installed with explicit
approval and loader read-back verification. After each of the two approved boots,
the native keybag started locked (`AKS 0x1`), and the user confirmed passcode
unlock, lock, then a second passcode unlock work.

The restore bug is confirmed and repaired in source: ENROLL_INIT `+0x118` is a
**storage path**, not a name. The old code supplied `"fingerprint"` there, binding
the encrypted template's outer HMAC to that 100-byte zero-padded value. Restoring
that exact value with CONTROL `0x30` made the original disk file load successfully
after reboot (finger ID `1:`), without changing the template or bypassing its MAC.
The new kernel source supplies the same path automatically in Identify Init;
that kernel build is prepared locally, not installed. Keep the legacy value even
if the project is renamed.

The user then requested fresh registration followed by matching. Registration
finished with 17 accepted presses, 18 quality retries, 100%. The new 376,182-byte
file differs from the preserved original. It was explicitly read from disk and
sent through the structured client (bypassing the kernel template cache); the TA
accepted it and returned finger ID `1:`. The immediate match trial still returned
error `0x20`, with no image-quality errors and no verified match. This isolates
template restoration from the outstanding comparison failure.

There is no proven causal link between missing secure-user-ID/HAT metadata and
these no-matches. Legitimate passcode-bound authentication still needs that work,
but do not promise that adding an arbitrary ID will repair the matcher. PREPARE
in the tested empty first-call form returned zero sealed metadata bytes.

TA QDBL diagnostic export `0x4c` produced 4,984 private bytes containing eight
verify records and eight identify outcomes. They are captured for offline analysis;
no more pressing is needed now. The user is handing the phone to another kernel
agent. Main/master and the release DevOps worktree are untouched. Source is on
`feat/s20-fingerprint`; private logs/templates/calibration remain uncommitted.

The user wants the actual under-display fingerprint sensor to authenticate native
iOS unlock, with passcode fallback. Merely detecting a finger, fabricating a
BiometricKit match, dismissing SpringBoard, or accepting arbitrary IPC is not that.
No successful-match injection or biometric unlock selector is added here.

Prepared artifacts in ignored `out/fingerprint/`:

- `fingerprint-status`: read-only iOS LocalAuthentication/IOKit diagnostic.
- `wiring.json`: selected properties from the phone's locally owned DTB/DTBO.
- `kernel-build.log`, `mach.x1slte-fingerprint-probe`, `kernel-slot.bin`: the
  experimental one-shot compatibility-probe kernel and its padded flash slot.
- Synthetic host tests for the policy, DTB inventory and manual-probe guard.

## Confirmed device wiring

The actual saved `dtbo.1.dtb` has a `qcom,qbt2000` node, matching the reference
Exynos990 x1slte revision-21 device tree. The base DTB resolves its external
symbols as follows (overlay phandles such as `0xffffffff` are not physical addresses):

| Item | Configuration evidence |
| --- | --- |
| Sensor/model | Qualcomm QBT2000 / G98X |
| Firmware IPC interrupt | GPA1 pin 3 |
| Finger detect / WUHB | GPA1 pin 1 |
| GPIO controller | `/pinctrl@15850000/gpa1` |
| Power supply | `VDD_BTP_1P8` |
| SPI bus | SPI4 / PERIC0 USI04; GPP2 pins 0–2, function 2 |
| SPI chip select | GPP2 pin 3, function 2 in the active pinctrl state |
| SPI inactive | GPP2 pins 0–3, input function, pull-up |

This identifies board wiring, not working communication or a successful live
sensor probe. No GPIO, regulator, sensor reset or SPI configuration was changed.

Samsung's source at local reference commit `69515fbb7` has
`CONFIG_SENSORS_QBT2000=y`, `CONFIG_TEEGRIS_VERSION=4`, and secure fingerprint mode.
Its Kconfig explicitly assigns SPI communication to the trusted application in
secure mode. The normal-world driver exposes power/clock control, finger-down/up
and firmware notification plumbing. The nonsecure qbtspi path is not a demonstrated
production matching API. Do not substitute the touchscreen for fingerprint input.

Local Android init backups reference `tzdaemon`, `tzts_daemon`, the fingerprint
HAL service, and qbt2000 device nodes. A vendor TEE object with a fingerprint-like
UUID namespace is present, but its filename does not establish a callable protocol
or successful loading. The proprietary fingerprint HAL/client libraries have not
been located in the captured firmware subset. No firmware or biometric material
is copied into git.

## Live native-iOS baseline

Read-only checks found no AppleMesa or ApplePearlSEPDriver service, no exposed
qbt2000/tzdev device nodes, and no running biometrickitd in the sampled process
list. AppleKeyStore advertises the existing software passcode backend. After the
user re-enabled their passcode, native state was protected/locked (MKB 1, AKS 0x5).

`tools/fingerprint/status.m` ran on the phone without evaluating an authentication
policy or starting enrollment/matching. Results:

| Query | Result |
| --- | --- |
| LocalAuthentication biometrics available | false |
| Reported biometry type | 0 |
| Biometrics capability error | -1000 |
| Device-owner capability error | -1000 |
| AppleKeyStore / software passcode backend | present / enabled |
| AppleMesa / ApplePearlSEPDriver | absent / absent |

Do not reinterpret the device-owner capability error as “no passcode”: raw native
MKB independently confirms the protected bag. The probe is a command-line
caller, not proof of every app's entitlement/policy behavior. It logs only
capability booleans, numeric errors and state, never credentials or templates.

The extracted iOS 16.5 BiometricKit cache contains BKDeviceTouchID, BKDevicePearl,
BKEnrollTouchIDOperation, BKMatchOperation and XPC match/enrollment callbacks. The
public app-facing goal is LocalAuthentication with fingerprint semantics, not
claiming fingerprint hardware is Face ID. Existing native keybag/credential binding
must work too; drawing a Touch ID prompt or enabling biometrickitd alone is insufficient.

## Prototype policy, deliberately not connected to unlock

`tools/fingerprint/policy.h` is a host-testable decision core only. It is not
linked into the kernel, SpringBoard or any daemon. It has no credential storage,
matcher, IPC service or unlock API. `FP_VERIFIED_MATCH` is a caller contract: a
future trusted backend must verify secure-world proof before making that call.
Passing that enum does not cryptographically verify anything by itself.

The prototype requires a genuine passcode verification after initialization,
backend failure/restart, enrollment change or invalidated keybag generation. It
binds each request to a fresh coordinator-generated 128-bit challenge, keybag and
enrollment generations, and a five-second lifetime. Presence events only wait;
cancelled, timed-out, cross-generation or replayed completions are denied.
A failed match requires passcode immediately (the user's requested conservative
policy); successful biometrics do not reset the passcode-verification timestamp.
An eight-hour passcode validity ceiling is a prototype restriction, not a claim
to reproduce every Apple security timer.

Apple's documented policy requires passcode after restart and allows at most five
unsuccessful biometric matches; a lower fallback threshold can be used. Native
policy integration and its first-unlock, timeout and lockout behavior still need
to be validated when a real matching backend exists.

## Prepared first hardware test: TEE compatibility only

The kernel debug port gains `fp.tee-version 0|1` on the Exynos/GICv2 build:

- `0` reports the planned call, or its cached result; it never initiates a call.
- `1` makes exactly one explicit compatibility call per boot. Further requests
  return its cached result, including errors. No retries or polling thread.

The allowlisted ABI is Samsung's ARM fast SMC32 trusted-OS CHECK_VERSION:
function `0xb200000d`, reference-client version field `0x41357` (4.19.87), driver
ABI `0x30000` (3.0.0), with unused argument registers zero. The client version is
an observed ABI field, not an assertion that XNU implements Linux. Calls use the
existing root-only serialized debug port. No generic arbitrary-SMC interface is
added. The diagnostic does not run at boot, register shared pages, load a trustlet,
control the sensor, verify a fingerprint or change a keybag. Compatibility
negotiation can affect volatile secure-world state; unknown firmware can still
reject or fail the call.

Cross-compile `tools/fingerprint/probe.c` with the iOS SDK, sign ad hoc and copy
it to `/var/tmp/redwood-fingerprint-probe`. After an approved test boot, run on
the phone as root; inspect first, then explicitly call once:

```sh
/var/tmp/redwood-fingerprint-probe --inspect
/var/tmp/redwood-fingerprint-probe --call-once
```

Before deployment, acquire the shared phone lock, obtain user reboot approval,
and back up/read back the currently installed 16-MiB recovery kernel slot. Flash
only the new slot at recovery offset `0x2000000`; preserve current boot arguments,
loader, root volume and userdata. Retain the exact previous slot for rollback.
A successful version response proves only this SMC exchange; it does not prove
trustlet loading, enrollment, matching, signed proof verification or iOS unlock.

## Hardware result: version probe passed

The current 16-MiB recovery kernel slot was read from offset 0x2000000 into
`out/fingerprint/kernel-before-84931a3b.bin` before any write. The new slot was
written and verified by the loader's readback. Boot arguments, loader, root and
userdata were left in place. Deployment hashes are in ignored `deployment.json`.

After the approved reboot, native MKB reported protected/locked before first
unlock (1 / AKS 0x1). The inspect command said NOT RUN. The explicit version call
returned `x0=0, x2=0`, and a subsequent inspect returned the same cached result.
There was no second SMC call. The user confirmed passcode unlock both immediately
after boot and again after re-locking. This establishes the compatibility
exchange, not any enrollment, match, signed-token or native-biometric capability.
`/var/tmp` is cleared on this port's boot; copy the diagnostic client after boot.

## AUX and service channels: hardware test passed

`redwood_fp_channels.h` and fixed helpers in the debug port implement the first
shared-memory steps observed in Samsung's initialization order: per-CPU AUX
registration (SMC 0xb2000000), followed by one service channel (0xb2000001,
mode 1, one page). The manually armed `fp.tee-channels 1` command first requires
a successful explicit version probe, a supported CPU count and all configured
CPUs online. Inspect mode 0 allocates nothing and invokes nothing.

The reference Linux page configuration defaults to 4 KiB with a 32-bit PFN ABI.
For this prototype, each shared page has a dedicated, zeroed, wired 16-KiB XNU
backing page, and only its first 4 KiB is shared. PFNs are physical >> 12, checked
for alignment/range; no kernel/user data from adjacent allocations is included.
All nine backing pages are allocated before the first registration on an 8-core
phone. Each AUX call is bound to its CPU, and the original binding is restored.
Cache clean/invalidate and barriers surround sharing and the bounded service
response read. Registered/uncertain pages stay wired until reboot, including on
failure; total backing on this phone is at most 144 KiB. No periodic work.

At the first firmware error, the sequence stops without retrying or registering
later channels. Repeated commands only report cached status. The prototype does
not install IRQ handlers, send SYSCONF, start firmware scheduling/TA loading,
power the fingerprint sensor, enroll, match or change keybag authentication.
Zero/absent service masks must not be treated as working authentication.

The channel sequence passes host guard/partial-failure tests with ASan/UBSan, and
the Exynos kernel cross-build passes. The user approved a second reboot and test. A fresh backup matched the previously
tested version-only slot, and the loader verified the channel-kernel write. It
is now the running kernel. The explicit test returned:

`retained AUX=8 service=1 result=0 cpu_mask=0x1 user_mask=0x1`.

A subsequent inspect returned the cached result, and native MKB remained protected
and locked before first unlock (1 / AKS 0x1). This confirms the first shared-memory
transport handshake, not trustlet loading or fingerprint authentication. The
on-phone commands were:

```sh
/var/tmp/redwood-fingerprint-probe --inspect
/var/tmp/redwood-fingerprint-probe --call-once
/var/tmp/redwood-fingerprint-probe --inspect-channels
/var/tmp/redwood-fingerprint-probe --connect-channels
```

The fixed-command C client refuses a kernel without the expected diagnostic
prefix and never accepts arbitrary SMC addresses or arguments.

## Next prepared test: IRQ and SYSCONF (not flashed)

The saved base DT `/teegris` node contains GIC SPI 222/223, level-high, corresponding
to absolute INTID 254/255. The next manually armed `fp.tee-sysconf 1` prototype
requires a successful complete channel handshake. It claims these otherwise
unused IRQs, with conflict checks, and sends `{event=254, panic=255, flags=0}` in
CPU0's dedicated AUX buffer to SMC 0xb2000009. It reads only the first four response
words: secure OS version, CPU count, big-core mask and flags. CPU count and mask
shape are validated; a zero SMC result alone is insufficient.

Handlers mask on the first event and increment counters. No firmware scheduling,
thread pool, log drain or matching is started in the ISR, and panic events
invalidate the diagnostic's result. This avoids an interrupt storm while the full
scheduler is absent. IRQs and backing buffers are retained safely until reboot
once firmware may reference them; this is not production IRQ handling. If an IRQ
is already claimed or fires before configuration, the exchange is refused. No
hardware call or IRQ attachment occurs at boot or in inspect mode.

This kernel cross-builds, and host tests cover gating, partial/error conditions,
invalid response shape and panic rejection. It is staged as
`out/fingerprint/kernel-sysconf-slot.bin`, not flashed. After an approved reboot,
repeat the version/channel sequence, then use:

```sh
/var/tmp/redwood-fingerprint-probe --inspect-sysconf
/var/tmp/redwood-fingerprint-probe --exchange-sysconf
```

## IRQ and SYSCONF: hardware test passed

The user approved a third reboot. A fresh backup of the running channel kernel was taken and verified
(`out/fingerprint/kernel-before-sysconf-b630a525.bin`); the loader verified the IRQ/SYSCONF slot's write. After boot
native MKB was protected and locked before first unlock (1 / AKS 0x1). Version and channel prerequisites were repeated
(`x0=0 x2=0`; `retained AUX=8 service=1 result=0 cpu_mask=0x1 user_mask=0x1`), then the exchange:

`fp.tee-sysconf: result=0 valid=1 os=0x4010000 cpus=8 big_mask=0xc0 flags=0x1 event_irqs=0 panic_irqs=0`

The secure OS reports version 0x4010000 (TEEGRIS 4.1), 8 CPUs and big cores 6-7 (mask 0xc0, the M5 pair), flags 0x1.
No event or panic interrupt arrived: with no normal-world scheduler yet, the secure world has nothing queued that
would raise one. A later inspect returned the cached result. This is the last step of Samsung's tzdev initialization
before scheduling; it does not load a trusted application, power the sensor, enroll or match.

## This phone's own trusted applications (captured firmware)

`device/firmware/vendor-tee/` (the phone's stock `/vendor/tee`, not in git) holds the TEEGRIS trusted applications; the
last six bytes of each UUID are ASCII. Relevant here:

| UUID tail | Name | Size | Role |
| --- | --- | --- | --- |
| `46494e474502` | `FINGE\x02` | 3,529,221 | the fingerprint TA (QBT2000 matcher, templates) |
| `474154454b45` | `GATEKE` | 23,205 | Gatekeeper: passcode-verification auth tokens |
| `4b45594d5354` | `KEYMST` | 424,833 | Keymaster: keys bound to those tokens |
| `42494f535542` | `BIOSUB` | 15,717 | biometric support |
| `5345435f4652` | `SEC_FR` | 8,945,133 | Samsung face recognition (not needed) |

So the device-matched matcher exists locally: the "locate legitimate binaries" item of the plan is answered for the TA
itself (the normal-world BAuth client protocol still comes from the reference libraries). Loading it needs the
normal-world half of Samsung's TA loader (tzdaemon over inter-world sockets) and a scheduler first.

## Vendor-client reference located

A public Exynos990 vendor reference was located at
`Exynoobs/proprietary_vendor_samsung_exynos990-common`, commit
`715080389b102e4d175a9244a84910ee549d3d67` (lineage-19.1). Selected ARM64 ELF client
libraries and tzdaemon were downloaded only into ignored
`out/fingerprint/vendor-reference/` for local protocol analysis. They are not
executed on the phone, linked into the port, or committed. They are not verified
to match the phone's exact stock firmware version.

Exported symbols include BAuth enrollment, identification, HAT, password and
session operations, plus TEEC and Samsung TEECS calls. The identification wrappers
use TEEC InvokeCommand IDs 5 (init), 6 (do) and 7 (final). The common transfer path
uses a registered partial in/out shared-memory parameter. The TEEC client expects
`/dev/tzdev`, `/dev/tziwsock` and a loader daemon path. These are observed protocol
clues, not a ready-to-use matching implementation; no guessed packets were sent.
Different TA/loading modes and shared-memory layouts must be resolved before
invoking enrollment or identification on actual firmware.

## Enrollment protocol (working, 2026-11-14)

Sources: Ghidra decompiles of the FINGE TA (`device/firmware/vendor-tee/...46494e474502`, ELF after the 8-byte
`SEC3` header, Ghidra image base +0x100000, dispatcher `FUN_001210f0`) and of `libbauthserver.so` /
`libbauthtzcommon.so` (`out/fingerprint/vendor-reference`). Tools: `tools/re` (Ghidra 12.2, JDK 25, decompiler
built for arm64, `scripts/Decomp.java`).

### Shared buffer layout (one TEEC shared memory, `FP_SHM_SIZE` 0x2ee00c)

| Offset | Meaning |
|---|---|
| +0x0, +0x4 | command number (both words; Samsung writes the command, not 1) |
| +0xc | op code sent to the TA |
| +0x10 | string argument (user id / CBGE slot number as text, <= 0x100) |
| +0x110 | data in (ENROLL_INIT: finger index; CONTROL: payload) |
| +0x114, +0x118 | ENROLL_INIT: 7, authenticated storage path (legacy "fingerprint") |
| +0xa010 | generic data length (non-control commands) |
| +0xfa110 | CONTROL_OP data length |
| +0x177004 | response status (echoes the command) |
| +0x177008 | response op code (r[0]); +0x177010 timeout (r[2]); +0x177018 progress (r[4]) |
| +0x177014 | response data / CONTROL_OP output; CONTROL op 67 slot byte goes here on input |
| +0x177024 | ENROLL_DO template out (length at +0x1db024) |
| +0x271014 | CONTROL_OP output length (in: room, e.g. 0x96000; op 66: slot number) |
| +0x2ee008 | TA error (BAuth result) |

### TA commands (dispatcher case = command number)

| Cmd | Name | Notes |
|---|---|---|
| 0 / 1 (client 1) | PREPARE | op 2 = first call. **Resets any active enrollment** (`DAT_0045cdd4 = 0`). Takes/returns encrypted matcher metadata (<= 0xa000) on later calls (op != 2, data > 0). |
| 2 | ENROLL_INIT | op must be 1 (else 51). Starts a new enrollment (QFP_EnrollBegin) only if none is active or the restart flag is set; otherwise **continues** and re-arms ENROLL_DO. |
| 3 | ENROLL_DO | per-touch state machine: op 1 -> answers 4 (wait finger, timeout -1); op 4 -> 5 (capture); op 5 -> 6 (image taken); op 6 -> result: error 0 + progress, or 39 + quality flags. At progress 100 the TA commits (QFP_EnrollCommit) and copies the template out. Calling DO again after a result re-processes the old image (refused 39). |
| 4 | ENROLL_FINAL | first call after INIT runs a delete/cleanup path; **ends the enrollment**. Not used between touches. |
| 10 | CANCEL | resets the enrollment. |
| 12 | CONTROL_OP | sub-op at +0xc (below). |

Working loop (kernel `fp run 3 1 SECONDS 4`): PREPARE once (`tee-up.sh 1:2`), `fp einit 1`, then per touch
DO 1 -> 4 (wait for the finger held 150 ms on the spot) -> 5 -> 6 -> result -> ENROLL_INIT -> next touch, until
progress 100.

### TA errors seen

| Error | Meaning |
|---|---|
| 39 (0x27) | sample rejected; quality flags in the response (not an image-quality failure by itself) |
| 29 (0x1d) | contamination check failed (only runs when CBGE basis exists) |
| 51 (0x33) | bad state / wrong op (e.g. INIT with op != 1, DO without INIT) |
| 67 (0x43) | capture failed (TA sg_malloc failure after CBGE without psf calibration) |
| 68 (0x44) | QFP -180 (capture/scan failure: zero-size image, missing psf config) |

QFP-to-BAuth map (`map_qfp_error_to_buath_result`, TA 0x25450): -180 -> 68, -140 -> 51, -190 -> 36, -200 -> 1001,
-192 -> 73, -303 -> 33, -216 -> 32, -130/-120 -> 49, unknown -> 29.

### Quality flags (libbauthserver `finger_image_quality_map`)

| Flag | Android acquired code | Meaning |
|---|---|---|
| 0x08000000 | 1 PARTIAL | only part of the finger |
| 0x04000000 | 1003 LIGHT_TOUCH | press harder |
| 0x01000000 | 1001 WET_FINGER | wet |
| 0x00002000 | 3 IMAGER_DIRTY | dirty |
| 0x00000100 | 4 TOO_SLOW | |
| 0x00000008 | 1002 DUPLICATED_IMAGE | same as last touch |
| 0x00000002 | 5 TOO_FAST | |
| 0x80000000 | 2 INSUFFICIENT | combined with one of the above |

TA reasons -17/-14 -> 0x80000008, -16 -> 0x80000100, -15 -> 0x80000002, -24 -> error 70.

### CONTROL_OP sub-ops (command 12)

| Op | Use |
|---|---|
| 69 | CBGE start; then follow the answered ops: 68, 70 (timeout = ms), 67 |
| 67 | CBGE data update, per slot 0..15: slot as string at +0x10, slot byte at +0x177014, out room 0x96000 at +0x271014. Slots 0 and 8 return ~368 KB each. |
| 66 | CBGE data load: slot string, blob at +0x110, length at +0xfa110, slot number at +0x271014 |
| 71 | CBGE finish |
| 84, 83, 65 | used by Samsung around wait-interrupt / finger removal (not needed for enroll) |
| 22 (0x16) | SET_ENROLL_SESSION: client-side flag only in libbauthtzcommon, never sent to the TA |

CBGE (background calibration) works and clears the IPC request, but with CBGE data loaded the capture path needs
`/data/vendor/misc/qti_fp/psf/psf_calibration.dat` (in the TA's own VFS; produced only by factory PSF tests) and
otherwise fails (29/67/68). Enrollment runs without CBGE.

### Hardware signals

- GPA1-3 = sensor firmware IPC, active low, pull-up (Samsung `qbt2000_common.c`: every IPC = CBGE_REQUIRED, msg 29).
- Finger down/up on stock comes from the touch panel: HAL `request(SEM_REQUEST_TOUCH_EVENT=22, 2 down / 1 up)`
  (LineageOS hardware/samsung `aidl/fingerprint/Session.cpp`). Here the kernel watches the touch panel itself:
  sensor spot x 22000-43500, y 44000-60000 (0-65535 units), held 150 ms.


### Factory calibration (2026-11-14 19:20): the fix for hard presses

Stock loads the phone's factory CBGE data at boot (FPBAuthServiceStorage readCBGEData, `/efs/biometrics/meta/`, on the
**sec_efs** partition): `cbge_0.dat` (368096 B, slot 0), `cbge_8.dat` (368024 B, slot 8), `bg_estimation*/bg_basis.dat`,
`bg_metadata.dat`, `sensor_id`. Without it the ultrasonic image has no background model: only hard, perfectly placed
presses pass (LIGHT_TOUCH/PARTIAL). Copied (private, never committed) to `/var/root/fp-calib/cbge_N.dat`; kernel
`fp cbgeefs` loads every slot with CONTROL_OP 66. Order: tee-up (PREPARE) -> `fp cbgeefs` -> `fp 10:1` -> `fp 1:2` ->
`fp einit 1` -> `fp run 3 1 300 4`. Result: easy presses, varied angles accepted, 100 % in ~16 presses.
Capture trigger: 400 ms steady contact inside x 27500-38000, y 46500-55500 (Samsung qcom,position centre 32.34 mm up).

### Identify (2026-11-14): runs, never matches yet

IDENTIFY_INIT (cmd 5): op 1, +0x8 chunk (0, or 0xffffffff = re-arm without data), +0x10 user id string, template at
+0x110 (len +0x64110), encrypted metadata at +0x64114 (len +0x64514), 0x45 bytes params at +0x64518. Template from
ENROLL_DO at 100 % (+0x177024, len +0x1db024; header "FPV3" "1000"), saved to /var/root/fp-template.bin. TA decrypts it
(get_uniquekey kdf) and parses it (version 51.2). IDENTIFY_DO (cmd 6) runs the same 4->5->6 machine; error 32 (0x20) =
no match. Every attempt so far: no match. Missing vs Samsung: the enrollment HAT and sealed secure-ID metadata
(CONTROL 0x2d SAVE_SECAUTH_ID refuses with "sid is 0"). Their absence is a known authentication gap,
but the cause of the no-match results remains unproven; see the protocol audit.

### Still to do

1. Identify (TA cases 5/6/7) against the enrolled template.
2. Persist the template (+0x177024 out at progress 100) and PREPARE metadata; feed back after boot.
3. iOS bridge (BiometricKit / Touch ID) with passcode fallback.

## Remaining implementation and traps

1. Version compatibility returned zero on hardware. Next validate the prepared
   shared-memory handshake; stop on firmware errors rather than guessing SMC IDs.
2. Implement a narrow TEEGRIS transport: per-core AUX buffers, shared-memory
   registration, event/panic IRQs, scheduling, inter-world sockets and userspace
   loading/storage services as required by the actual firmware. Samsung's
   reference init order registers several such services; one SMC is not tzdev.
3. Resolve the signed fingerprint trusted application's real client protocol and
   matching result verification. Locate legitimate matching HAL/client binaries
   from this device's own stock firmware if the captured subset lacks them.
   Do not assume Android ELF/HIDL services can execute directly under Darwin.
4. Implement protected enrollment and template lifecycle. Old Android enrollment
   data may not survive the current userdata layout; never promise reuse or read
   template/TEE storage to bypass enrollment. Passcode consent occurs on-device.
5. Connect verified matching to a keybag-bound native authentication operation
   and the actual iOS BiometricKit/LocalAuthentication provider. Do not store the
   user's passcode for replay or expose a “match=true” notification as authority.
6. Validate wrong/right finger, after-boot passcode, cancellation, replay, lockout,
   app policy queries, passcode changes/removal, and repeated locks. Keep backend
   capability unavailable until these prerequisites exist.

Further traps:

- XNU uses 16-KiB pages; Samsung's shared-memory ABI derives page-frame numbers
  and AUX buffer sizes from its Linux page configuration. Resolve the actual
  secure page size/PFN width before sharing memory. Do not blindly pass XNU PFNs
  or expose adjacent allocations in a larger host page.
- Per-core AUX channels require CPU affinity/lifetime handling; a migrating XNU
  thread cannot assume the same channel remains selected across calls.
- Preserve secure storage and EFS. Firmware presence does not grant permission
  to overwrite secure enrollment/keymaster/gatekeeper state.
- Verify signed/protected result provenance and bind fresh challenges to one
  unlock request. A sensor IRQ, native UI callback or root CLI argument is not
  a fingerprint match proof.
- This remains software-keybag authentication on the iOS side, not Apple SEP
  equivalence, Apple Pay, attestation or full keychain access-control support.
- User credentials are entered only on the device. Never log them, embed them
  in commands/test vectors, or enable verbose native keybagd logging.

## Reproduce checks

```sh
python3 tools/fingerprint/test_inventory.py
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/fingerprint/test_policy.c -o /tmp/fp-policy && /tmp/fp-policy
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/fingerprint/test_tee_probe.c -o /tmp/fp-tee-probe && /tmp/fp-tee-probe
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/fingerprint/test_channels.c -o /tmp/fp-channels && /tmp/fp-channels
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/fingerprint/test_sysconf.c -o /tmp/fp-sysconf && /tmp/fp-sysconf
python3 tools/passcode/test_build_integration.py
```

Read the phone's own DTBs without copying them into the repository:

```sh
python3 tools/fingerprint/inventory.py /path/to/boot.dtb /path/to/dtbo.dtb
```

The status probe cross-compiles with the iPhoneOS16.5 SDK, ARC/blocks, and
Foundation, LocalAuthentication and IOKit frameworks. No special authentication
entitlement or match permission is requested. Kernel build: Exynos GICv2
DEVELOPMENT. Tests validate code/policy/guard behavior; they do not validate a
physical fingerprint match. The IRQ/SYSCONF kernel is flashed and its exchange passed (above). No physical fingerprint match has been attempted.

## Primary references

- [Samsung Exynos990 kernel reference](https://github.com/ExtremeXT/android_kernel_samsung_exynos990/tree/69515fbb7/drivers/fingerprint)
  and its `drivers/misc/tzdev` implementation, Kconfig, board DT and clock tables.
- [Apple biometric/passcode requirements](https://support.apple.com/en-mt/guide/security/sec9479035f1/web).
- [Android fingerprint HAL architecture](https://android.googlesource.com/platform/docs/source.android.com/+/0fda4ae8f8f65df3360a3170059b5ea11c99f7ec/src/devices/tech/security/authentication/fingerprint-hal.jd).
- [Android authentication tokens and trusted matching](https://source.android.com/docs/security/features/authentication).

- [Public Exynos990 vendor-client reference](https://github.com/Exynoobs/proprietary_vendor_samsung_exynos990-common/tree/715080389b102e4d175a9244a84910ee549d3d67).

## Protocol audit: distinguish matching from authentication

This audit reads both the normal-world client (`libbauthtzcommon.so`,
`libbauthserver.so`) and the FINGE TA. Addresses below use Ghidra's +0x100000
image base, not runtime addresses. Runtime trace addresses relocate at boot.
Generated decompiles, firmware, templates, HATs and calibration remain local;
they are not source files to distribute.

### Confirmed response layout

The original parser confused the internal match-index field with the public
wire reply. TA wrapper `FUN_0011993c` calls `FUN_00111a38`, maps the internal index
through `FUN_001206b8`, and constructs a different response:

| Absolute shared-buffer offset | IDENTIFY_DO meaning |
| --- | --- |
| `0x177004` | echoed command, must be 6 |
| `0x177008` | response operation |
| `0x17700c` | inner TA error |
| `0x177010` | timeout |
| `0x177014` | quality flags |
| `0x177018` | matched score |
| `0x17701c` | enrolled bio/finger ID; zero is not a valid match |
| `0x177020` | **not populated as the match index by this wrapper** |
| `0x2db048` | returned 69-byte HAT, if present; never log it |
| `0x2db08d` | token-present flag (written as an unaligned word) |
| `0x2ee008` | outer TA error |

`FUN_001210f0`, case 6, has a no-match branch that returns **error 0x20 with
op 0 or 0x4d**. Op 0x4d is not match success. A zero-filled/unpopulated `r[6]`
is not proof either. The old `r[6] != -1 || op == 0x4d` check is removed.

The new parser checks transport, buffer bounds, response command, both errors,
terminal op, expected finger ID and the token-present flag. The score word is
logged as an opaque diagnostic; Samsung's wrapper does not require it to be positive.
It calls that result **SENSOR MATCH candidate**, never verified authentication:
the HAT MAC, challenge, secure user ID, freshness and keybag binding still need
verification before any native unlock consumer can use it. No such consumer is
added. Synthetic tests include empty/truncated replies, a wrong command, a failed
transport, mismatched errors, no-match/op77, missing token flag and wrong IDs.

`IDENTIFY_INIT`'s returned ID list is at `0x177014`, with length at `0x1771a4`.
The single-finger prototype now requires exactly `1:`; malformed, duplicate,
empty or unexpected IDs stop the test before capture. The request is cleared
in full as Samsung's client does. Both init commands now validate their command
responses; ENROLL_INIT clears and checks the TA error rather than returning only
the TEEC transport result.

### Repaired decompiler analysis

`QFP_DatabaseInject` at `0x15341c` uses two-instruction outlined branch stubs at
`0x10fb2c`, `0x10fbac`, `0x10fbb4`. Ghidra initially treated these jumps as calls
and split the function into misleading short pieces. Its reconstructed function
spans `0x15341c..0x15361f` plus those outlines. The same issue occurs in
`decode_each_templ_ver3` (`0x11f750..0x11fbdf`).

The reproducible helper is [DecompileOutlined.java](../tools/fingerprint/reverse/DecompileOutlined.java).
Run it with `-readOnly`; it joins only a two-instruction outline that branches
back into the supplied function range. The ELF and saved Ghidra project are not
modified. See [the reverse-engineering instructions](../tools/fingerprint/reverse/README.md).

The complete database function deserializes every supplied template before an
optional clone/transformation controlled by the flag array. Seeing a flag of zero
alone does **not** prove the template was skipped. Likewise, SID/HAT fields must
not be guessed from partial decompiles.

The fully reconstructed `decode_each_templ_ver3` checks outer and inner HMACs,
decrypts the container, checks the Android user ID, creates the bio ID list and
copies the decrypted payload into the internal template array. The pre-reboot Init
result `1:` is evidence that the cached template was decoded and identified; it
was not a forced disk read or a successful comparison with a new capture.

### Calibration and persistence corrections

Error 68 was previously interpreted as a request to recalibrate. That was a
hypothesis contradicted by subsequent capture diagnostics. The enrollment loop
now stops on this error and preserves the working factory calibration; it does
not automatically run `fp cbge`. Manual factory calibration loading remains.
Short template writes now report failure; oversized template input is freed;
raw template-header words and unpopulated response words are no longer logged.
Saved-file readability does not prove a fingerprint can authenticate after reboot.

### Remaining work and next live check

1. Deploy the audit kernel only after reboot approval. First verify its status
   marker, software passcode and re-lock behavior; retain a read-back kernel backup.
2. Restore the existing factory CBGE slots using the documented working sequence,
   then run Identify Init against the existing template. Stop if the TA rejects
   it or returns an unexpected ID list. Do not start a fresh enrollment by default.
3. Trace one bounded Identify Do attempt with the correct reply fields and
   correlation to the TA's capture/VerifyInject path. A no-finger dry run can
   validate state transitions, but cannot establish matching. Ask for same-thumb
   and different-finger trials only after the request sequence is checked.
4. Resolve the actual comparison failure from evidence. Do not lower matcher
   thresholds, fabricate a positive result or assume a missing HAT is the cause.
5. Implement legitimate challenge-bound enrollment/authentication and native
   passcode/keybag integration. Android's [Gatekeeper documentation](https://source.android.com/docs/security/features/authentication/gatekeeper)
   describes secure-world signed tokens and secure user IDs; a custom unsigned
   HAT or arbitrary local SID is not a substitute. Do not overwrite Gatekeeper
   state or export its key while researching this.

Validation (host only):

```sh
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I xnu/files/bsd tools/fingerprint/test_wire.c -o out/fingerprint/test-wire
out/fingerprint/test-wire
clang -std=c11 -Wall -Wextra -Werror tools/fingerprint/test_policy.c \
  -o out/fingerprint/test-policy
out/fingerprint/test-policy
python3 tools/passcode/test_build_integration.py
```

Host tests and kernel compilation do not qualify fingerprint unlock or prove a
live match. Audit-v1 is installed; exchange-v2 has not been installed.

### Post-reboot findings and structured-client preparation

The prior session confirmed there are no saved PREPARE outputs or sealed
secure-ID metadata. It never sent a storage-path command and never forced a disk
reload; all successful Init checks reused the cached template. Treat the outer
HMAC failure as new evidence rather than assuming it caused the earlier no-matches.

Both sides must be read together. The complete TA control dispatcher
`FUN_001121d0` contains:

- `0x16`: `BAUTH_OP_CODE_SET_ENROLL_SESSION`, writes the low byte of the input
  **length** to context `+0x48`. However, `BAuth_Control_OP` at client `0x106038`
  intercepts 0x16 and changes its local session flag without invoking the TA.
  Thus the prior agent was correct about the stock client flow. Do not treat the
  unused TA handler as proof of a missing required command.
- `0x30`: `BAUTH_OP_CODE_SEND_STOREPATH`, accepts up to 100 input bytes and copies
  them to context `+0x1902ca`.

`encode_each_templ_ver3` (`0x11e4d0`) incorporates that 100-byte path into its
outer HMAC. Its decoder (`0x11f750`) reconstructs the same authenticated input.
`get_uniquekey` (`0x117c98`) uses TEES_DeriveKeyKDF with zeroed 16-byte inputs;
this audit has neither exported its derived key nor proved its stability across
sessions. The [original TEEGRIS research](https://www.usenix.org/system/files/sec22-shakevsky.pdf)
(Appendix C) describes KDF binding to secure task UID/group, which is a lead to
verify against this firmware, not proof of this device's current failure cause.

Samsung's `common_prepare` (`libbauthserver`, `0x14abbc`) reads metadata type 0,
chooses op 1 for an existing record or op 2 for first initialization, then saves
valid returned metadata. `BAuth_Prepare` (`libbauthtzcommon`, `0x1044c8`) uses input
`+0x10` / length `+0xa010`, output `+0x177014` / length `+0x181014`. This output was
not persisted by the previous generic-command experiments.

The exchange-v2 bridge adds fixed private request/reply files and whole-packet
validation for a restricted FINGE command set. Its kernel command guard rejects
overlap with any other fingerprint command. The host also takes a shared lock
across checkouts before upload. The host refuses the running audit-v1 kernel
before upload/invoke; this was verified with the real phone and in host tests.
It never automatically retries a TA command, resets Gatekeeper, signs a fabricated
HAT or connects results to iOS unlock. All payload files stay mode 0600 in private
directories. Eight host tests plus the C parser/request tests pass; the kernel
compiles. Root-only file/transport behavior on exchange-v2 still needs live validation.

### Confirmed legacy storage-path fix and current diagnostic boundary

`BAuth_Enroll_Init` in `libbauthtzcommon.so` at `0x1047a4` explicitly names its
last argument `store_path` and copies it to `+0x118`. The service at `0x14d9ec`
passes its storage path (`service +0x144a9fa`), not a display label. Restoring the
legacy `"fingerprint"` value on the live exchange-v2 kernel changed the original
file's result from outer-HMAC failure/error31 to successful Init/finger1. KDF
per-boot instability was a hypothesis; this result supplies a simpler confirmed
explanation for the observed restore failure.

The latest source names this constant `FP_STORE_PATH`, preserves it during
Enroll Init and supplies it at Identify Init `+0x6455d` on every call. The Python
builder uses `LEGACY_STORE_PATH` too. The new kernel also opens research files
nonblocking so a wrong FIFO type cannot hang before type validation; that change
is built, not installed or hardware-qualified. Existing regular-file transport,
bad-mode, hardlink and symlink rejection were exercised on exchange-v2.

The live view now uses one private multiplexed SSH connection to read the kernel
message buffer. It does not poll/restart rwlogd. A byte cursor avoids replaying
old matches, and the view distinguishes REGISTERING YOUR THUMB from MATCHING
SAVED THUMB. Sensor candidates remain explicitly unverified; native iOS unlock is
not enabled. Host cursor/view tests pass.

QDBL data is separate from disabled level-2 text logs. The client
`FPBAuthService::getAdditionalLog` (`0x14bcc8`) uses CONTROL `0x4c`. The TA
`qfp_get_qdbl_info` copies and clears a ring up to 0x8000 bytes. The research
request used an explicit 0x8000 output capacity, not an arbitrary small buffer.
Record headers are `<u8 0x28, u8 type, u16 LE size, ...>`; schema comes from
`0x15d124`, `0x15e9f0`, metric offsets from `0x15d644`. The private capture parses
exactly to 4,984 bytes. Use `tools/fingerprint/qdbl.py` for an allowlisted summary.
The eight type-7 records all show opaque metrics `b3=8,b4=1,b5=255`; the eight
type-11 records show `matched_flag=0, metric_cb=2, aed=0`. The subsequent comparator audit below explains b3 and b5. Do not label
metric_cb=2 as a known failure reason or change thresholds based on these values.

### Offline comparator audit (15 November 2026)

Reconstructed `qfs_verify_to_db_` (`0x1883b0..0x188bdc`, including outlined
stub `0x10fbc4`) writes metric `b3=8` only after collecting an eligible template
and receiving return code zero from `qfs_em_match` (`0x259768`). All eight saved
verify records have that value. These attempts therefore reached the actual
comparison engine; they did not terminate at an early empty-template check.
This does not prove that every template or image input was correct.

Metric `b5=255` is the low byte of the engine's returned template index (-1).
Metrics `b6` and `b7` come from the result doubles at offsets `+0x38` and `+0x40`.
The saved pairs are `(1560,2300)`, `(1252,2300)`, `(1070,2300)`, `(1390,2300)`,
`(1012,2300)`, `(1404,2300)`, `(985,2300)`, `(1364,2300)`. The second value is
populated from global unsigned short `0x5003ac`; the first comes from the core
engine's result field. Their precise acceptance semantics still need tracing;
keep the decoder names opaque and do not change thresholds. `metric_cb=2` is
also not an established failure reason.

Next inspect core matcher `0x258740`, its backend call `0x26d1e8`, the writers of
`0x5003ac`, and stock capture setup. No new phone test or kernel flash is required
to complete that analysis. HAT/SID integration remains necessary for legitimate
native authentication, but is not established as the cause of these no-matches.

## Rejection-only UIKit preview (15 November)

The user deferred matching research to build enrollment and rejection UI first.
See [the preview implementation](../tools/fingerprint/ui/README.md) and
[latest handover](../FINGERPRINT-HANDOVER.md). A separate Settings page and opt-in
lock-screen prompt now have compiled source connecting real sensor results, with
cooperative cancellation. They have not been installed or device-tested. This
does not register a native BiometricKit/LocalAuthentication provider or enable
fingerprint unlock. Only real no-match results show Try Again; errors remain
unavailable, and unverified candidates require passcode. Existing secure-state
and software-passcode requirements remain unchanged.

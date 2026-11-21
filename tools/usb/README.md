# Native USB mux and Finder

`usb_mux=1` adds a native USB mux interface alongside the existing CDC serial
and Ethernet functions. The device name is Jesvi IOS. Stock macOS usbmuxd and
MobileDevice.framework handle discovery, trust, TLS sessions, and AFC; no Mac
helper daemon is needed for normal use. The on-device lockdownd is socket
activated and listens only on loopback and its native local control socket.

The mux worker connects to native services with actual kernel TCP sockets.
Stock lockdownd checks TCP_PEERPID and classifies those loopback peers as USB.
The IOKit mux/interface services publish actual USB configuration state so
native lockdownd can handle Trust and disconnect notifications. The transport
does not fabricate pairing responses, activation records, or account tokens.
A separate, explicitly enabled local Finder compatibility policy is described below.

USB interface 4 uses bulk OUT 0x05 and IN 0x85. The research interoperability
mode uses VID/PID 05ac/12a8 for the built-in desktop mobile-device driver match.
Its serial string matches this port's existing synthetic MobileGestalt identity;
it is not an Apple-issued identity or activation credential. The older serial
and ECM-only modes retain their existing descriptors when `usb_mux=0`.

The worker supports sixteen connections, bounded queues, host receive-window
flow control, and split mux frames up to 64 KiB. A single mux packet can span
several 16 KiB USB receive transfers; upstream usbmuxd uses a 48 KiB transmit
MTU. USB disconnect clears queues and closes kernel sockets. All socket I/O is
outside the DWC3 polling path. Native install/check-in services remain subject
to their existing entitlement and pairing requirements.

## Build and diagnostics

The standard x1slte command line enables `usb_mux=1`. Build the kernel and userland normally. The latter includes
`com.redwood.usbmux-lockdown.plist`. Kernel, root APFS image, and loader command
line must all be deployed together. The payload builder checks the final,
resolved command length after root-window substitution.

- `bash tools/usb/check.sh`: host framing/descriptor checks with ASan and UBSan,
  including incomplete and oversized headers and a 48 KiB split frame.
- `python3 tools/usb/mac-list.py`: list public discovery fields through the
  Mac's existing `/var/run/usbmuxd`, without reading pairing records.
- `python3 tools/usb/mac-probe.py DEVICE_ID`: read native device type, display
  name, and OS version over the actual USB connection.
- `bash tools/usb/build.sh`: compile/sign the on-device QueryType diagnostic
  with the locally supplied SDK. `lockdown-probe loopback` is a userspace
  negative control; root `sysctl -n kern.usb_lockdown_query` uses the accepted
  kernel peer path. The Unix socket is a local control channel, not a host
  lockdown stream.

## Verification on SM-G980F, iOS 16.5, 2026-11-11

Native Mac discovery, QueryType, DeviceName=Jesvi IOS, and ProductVersion=16.5
passed. The user saw and approved the native Trust prompt. Apple's native
AMDevicePair, ValidatePairing, StartSession and StartService(com.apple.afc)
returned success. An AFC test created a UUID-named file, wrote/read/verified
its contents, and removed only that file. Pairing remained valid after a USB
link restart. The user confirmed Finder shows Jesvi IOS, iPhone X and storage.
A 256 KiB deterministic-pattern write/read/verify/cleanup then passed after
reboot, with the existing pairing retained. The initial 16 KiB packet limit
was corrected to reassemble 48 KiB host frames. Finder loading remains a
separate UI result: the user confirmed the spinner clears, then clarified that
Finder's management panel reports Activation Failed while the device summary
still shows name, battery and storage. Native AFC was usable at that point; local Finder compatibility was added in
the follow-up below. Backup, restore and media sync are not established by
transport connectivity alone. A native application-list query
returned -402653091 while AFC still passed; that result alone did not establish document sharing.

Native pairing initially failed despite lockdownd holding its required
entitlement. The research AMFI stand-in has no private OSEntitlements object;
CS_OPS_ENTITLEMENTS_BLOB therefore returned success with an empty output. The
kernel now falls back to its existing embedded XML blob, retaining the normal
process/signature checks. On the live daemon, XML output changed from empty
to the existing 5351-byte blob; DER stayed 3488 bytes. No entitlements are
added. Pairing progressed to the normal locked-device/Trust flow and succeeded.

## iCloud

Full native Apple Account sign-in and sync are not established by USB pairing.
The native MAEGetActivationStateWithError read returned `Unactivated`. Existing
activation diagnostics identify unsupported UIK operations in MiniAppleKeyStore
and the absent Secure Enclave-backed activation/device identity. This USB work
does not provide those keys, Apple-issued certificates, or push credentials.
An iCloud Drive location in Files is not evidence that an account is signed in
or synchronizing. No account credentials were requested or used in these tests.

Protocol references:
[usbmuxd device.c](https://github.com/libimobiledevice/usbmuxd/blob/master/src/device.c),
[USB MTU/MRU definitions](https://github.com/libimobiledevice/usbmuxd/blob/master/src/usb.h).
The host daemon socket protocol is a different layer from USB wire framing.

## Local Finder compatibility

The user opted into a local-management compatibility mode after native Finder
reported Activation Failed. `libredwood-finder.dylib` is injected into lockdownd
only. When the native state is Unactivated with no underlying API error, it
presents Activated to that process so Finder can open its management tabs and
native local services. It does not write activation records, certificates or account tokens. Other processes, including the
native activation-state diagnostic and Apple Account services, still receive
Unactivated. FactoryActivated is deliberately not used because native lockdown
would skip its normal Trust prompt for that state.

To disable this policy, create
`/var/mobile/Library/Preferences/org.jesvi.finder-local.disabled` and reconnect
Finder. Remove that flag to re-enable it. Unexpected flag-access errors also
leave the native state in place. The library clears DYLD_INSERT_LIBRARIES so
child processes do not inherit it.

The kernel injection configuration now holds 4096 bytes, matching init's
existing reader; the new default configuration is 540 bytes. This preserves
all existing Sileo, Files, picker and other insertion entries instead of
replacing one to fit the old 512-byte limit. Prebuilt-only builds include the
new library too. `bash tools/usb/build-finder.sh` rebuilds it independently.

Live verification: the Mac read Activated and BrickState=false while the native
on-phone API still returned Unactivated. The disabled flag restored the Mac's
Unactivated response. Pairing and a 256 KiB AFC write/read/verify/cleanup still
passed. The user confirmed Finder's management tabs open, then supplied a
screenshot showing total/free capacity but a grey usage bar and disabled backup
controls. Native app lookup succeeded (188 apps); secure service startup for
backup2, installation proxy, house arrest, notification proxy and mobilesync
also succeeded. MobileBackup2 version exchange, device-ready and Hello
negotiation then returned error code 0 without a backup request. This does not
establish that an actual backup or media sync can finish. Backup WillEncrypt metadata was unavailable. A bounded backupd
probe did not populate it and was unloaded; backupd is not enabled by this fix.
No fake storage categories or backup/encryption capabilities are supplied.

`mac-management.c` is a read-only native Mac diagnostic for the relevant public
state/capacity fields and app count. Build with
`xcrun clang -Wall -Wextra -Werror -framework CoreFoundation tools/usb/mac-management.c -o out/mac-management`
and pass the discovered UDID. `--probe-services` starts and closes native secure
service connections without sending a backup/restore or account request.

The persistent build was reboot-tested: init installed the full 540-byte
configuration, including the original Sileo and Files entries and the separate
Finder library. Native outside-process activation remained Unactivated; paired
USB management, app lookup (188 entries), secure service probes and the 256 KiB
AFC roundtrip passed after reboot. The encryption metadata and grey controls
are not declared fixed by these tests. Finder's device UI cache was refreshed
for the subsequent UI check.


### Local app documents

The hidden local provider host advertises `UIFileSharingEnabled` and
`LSSupportsOpeningDocumentsInPlace`, with display name **Jesvi Files**. Finder
lists this app in Files; drag files onto its row for immediate transfer. Sync
is not the app-document transfer action. Its Documents folder is separate from
Safari Downloads and the native provider storage; existing folders are not
moved or exposed wholesale.

The native desktop House Arrest helper requires escrow metadata even on this
port's unprotected HFS data volume. For successful native Pair responses that
have no escrow data, the local policy supplies a persistent opaque marker in
that optional field. This is **not an encrypted escrow bag or a device key**.
It is stored root-only at
`/var/root/Library/Lockdown/jesvi-local-documents.token`; do not rotate it while
retaining a pairing that references it. Normal native Trust, certificates,
paired TLS and per-app access checks still run. No real escrow data is replaced.

Only house-arrest and notification-proxy StartService requests bearing that
exact marker are normalized to omit it, and only when the research USB kernel
is present, the native keybag reports disabled/no passcode, and `/private/var`
is HFS without content protection. Disable the local-mode flag above, introduce
a protected filesystem/passcode, supply any other escrow data and the native behavior remains. Requests using
our marker for other services are rejected through a reserved nonexistent
service name (native InvalidService), before keybag processing. Returning
EscrowLocked would make MobileDevice discard the document marker globally.
Backup and encryption service access is never granted by this normalization. This cannot provide encrypted backup or Apple Account activation.

A real 256 KiB document write/read/verify/remove passed using Apple's native
House Arrest API. The app's Documents folder was initially empty; a small
`Start Here.txt` was added on the test device with drag/drop instructions.
The staging script does not seed user documents on fresh builds.


The final image was rebooted with the library in `/redwood/lib`, and native
Trust approval renewed the Mac's optional document marker. Three native
256 KiB document roundtrips then passed, including after daemon restart and
unsupported escrow-service requests. House Arrest and notification proxy
accepted the local marker; backup2, installation proxy and mobilesync requests
requiring that marker returned InvalidService without discarding it. Their
ordinary service startup without escrow remains separate. The disable flag
restored Unactivated/BrickState=true; removing it restored document transfer.
Outside-process native activation remained Unactivated throughout.

For pairing diagnostics, legacy AMDevicePair can report PasswordProtected
while the real condition is a pending Trust dialog. AMDevicePairWithOptions
with ExtendedPairingErrors reports PairingDialogResponsePending explicitly.
Wait for the user to approve native Trust before retrying; unlocking alone
is not approval. No protected-device or Trust check is bypassed here.


Final Finder UI check remains unresolved. The user corrected an accidental
confirmation: the device/file visibility was not confirmed after the refresh.
Native document-transfer tests pass, but this does not establish Finder UI
visibility. Backup/media controls and storage categories also remain limits.


Follow-up UI diagnosis: Finder's sidebar returned after restarting the Mac's
AMPDeviceDiscoveryAgent and Finder. The Files list still excluded the system
provider host. Native MCM bundle-container registration reclassified the same
host as User, preserving its data/plugin containers. The filtered sharing query
changed from zero apps to one; the user then confirmed the app and Start Here.txt
appear in Finder. The host remains hidden on the phone's home screen.

Default builds include `register_finder_host` and its launchd job. It copies the
system host once into a native bundle container and registers it with native
LaunchServices. Existing app data is never copied or replaced. The helper exits
after registration; launchd retries failures at a 30-second throttle during
service startup. Existing copied bundles are retained, not overwritten on image
updates. The live helper and native registration were tested; the new startup
job has not yet had a separate reboot test. Grey backup/media and category-bar
limitations remain unchanged.

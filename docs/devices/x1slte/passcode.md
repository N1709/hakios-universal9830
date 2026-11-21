# Software passcode work (experimental)

Original symptom: Settings accepted a new passcode and its confirmation but did
not enable it. The original kernel always reported AKS flags 0x6 (no passcode).
The user enters credentials only on the phone; diagnostic tools never request or
log the real code. The implementation and live validation below supersede that
initial failure.

Two concrete blockers were found:

1. MiniAppleKeyStore hard-codes a no-passcode state. Lock/unlock return success
   without checking a secret; create/save/load/change-secret were unsupported.
2. `userland/mkjobs.py` excludes keybagd. MKBKeyBagChangeSystemSecret calls the
   MobileKeyBag shared service, so the native setup service is absent too.

## Implemented foundation

`SoftwareKeybag.h` implements a software-only verifier: random 128-bit salt,
PBKDF2-HMAC-SHA256 with 100,000 rounds, constant-time comparison, bounded inputs,
wrong-code rejection, lock/first-unlock state, old-code verification on change
or removal, and exponentially increasing retry delays after five failures
(1 second up to 60 seconds). Temporary password/KDF buffers are explicitly wiped.

The kernel adapter implements the observed iOS 16.5 libaks ABI for temporary bag
creation, opaque save/load, unload, system-bag selection, lock, unlock, state,
UUID and change-secret. A saved protected bag loads locked before first unlock.
Flag 4 changes produce a separate candidate bag so the original verifier is not
changed before native MobileKeyBag can persist/install its replacement. Root is
required for bag export, replacement and passcode changes. Queries and checking
a code do not automatically succeed. Unsupported SE-derived operations fail.

The code is gated by **`softpasscode=1`**, off by default. It is enabled on the
current test phone alongside the existing cooling changes. The SpringBoard
bring-up unlock helper refuses dismissal when a passcode is set or keybag state
is unknown; `autounlock=0` remains active.

Native keybagd now runs through a dedicated job with a restrictive umask. The
`no-effaceable-storage` device-tree capability selects Apple's software storage
path. Native change-secret epilogue verifies the installed secret rather than
returning unconditional success. `softkeybag-init` restores the saved software
bag locked before handing control to launchd; invalid or insecure existing files
stop startup rather than silently reverting to no passcode. Production keybagd
verbose logging is disabled because native log messages can contain credentials.

On-device isolated bag tests passed. Native empty-to-empty system transactions
passed before the user enabled a real code. The user then confirmed Settings
keeps the passcode enabled; native state is protected/unlocked (AKS 0x4, MKB 0),
and the native saved keybag is private (0600).

## Live result and repeat-lock fix (14 November 2026)

The approved notification kernel was flashed and read back by the loader, then
booted with the saved protected bag. The user confirmed the initial passcode
prompt, but later locks still allowed a swipe through. SpringBoard reported
“authenticated, a passcode” while native MKB reported locked (1), AKS flags 0x5.
The open padlock was consistent with this UI authentication mismatch. A successful
first unlock alone was therefore insufficient validation.

`software_passcode_lock.h` bridges `com.apple.iokit.hid.displayStatus` display-off
events to the native authentication controller's
`revokeAuthenticationImmediatelyForPublicReason:`. The observed iOS 16.5 method
locks the keybag, invalidates cached lock state and revokes authentication. It
never grants authentication or handles credentials. This immediate-lock policy
is scoped to `softpasscode=1`; native no-passcode handling is retained. It uses
one event subscription and one startup check, with no monitoring loop. A shared
associated-object guard prevents duplicate installation by the standalone tweak
and built-in library.

The bridge is integrated into `inject_sb.c` for future images. On the current
phone, the standalone `RedwoodSoftwarePasscode.dylib` and SpringBoard-only filter
are installed in `/var/jb/Library/MobileSubstrate/DynamicLibraries/`, on persistent
data. SpringBoard was restarted without rebooting. The user then confirmed that
the **second unlock requires the code**. Kernel state subsequently returned to
protected/unlocked (0x4). Further reboot testing of the standalone bridge and UI
wrong-code testing were not performed in this session; isolated native and host
wrong-code tests already passed.

The notification kernel emits IOGeneralInterest events outside the bag lock.
Local iOS 16.5 `MKBEventsRegister` (0x1ae395488) registers this interest, and
`aksNotificationCB` (0x1ae37e8b8) maps `0xe3ffd00d` to state-change and
`0xe3ffd00f` to first-unlock. These notifications alone did not establish reliable
repeat-lock enforcement on this port; retain the display-off bridge. A user-client
backup notification port is a separate interface.

The user clarified that the swipe-to-passcode animation concern was its short
activation threshold (roughly 10–20% of a swipe), not necessarily dropped frames,
and accepted the current transition. This implementation does not change the
swipe recognizer, animation duration, blur or passcode transition sequence.

## Speed and file-security refinements

A protected record can only be created with a nonempty secret. Empty credential
probes now reject without running 100,000 HMACs, preserving the same failure
counter and retry-delay behavior. Nonempty verification retains the original
PBKDF2 work factor. The host test counts HMAC calls to verify this fast path and
checks that rejection still increments failures. This optimized kernel is built
as `out/mach.x1slte-softpasscode-optimized` but is **not flashed**.

Boot restore now opens a root-owned private parent directory without following
symlinks, uses directory-relative file lookup, refuses multiply-linked or
non-regular files, and opens nonblocking so a FIFO cannot hang PID 1. Raw file
buffers are wiped through volatile writes. The hardened helper cross-compiles
and successfully parses the current phone's saved private bag with `--check`.
It is staged in `prebuilt/userland/softkeybag-init`; the current boot used the
previous helper. These changes are intended for the next prepared image, not a
reason to reboot a working test session.

## Clean-build and agent handoff

The tracked S20 `x1slte.cmdline` now enables `softpasscode=1` and keeps
`autounlock=0`. The default `x1slte.sh payload` uses this tracked profile rather
than the old minimal bring-up line. `metal_profile=1` was removed to leave room
under the loader's 550-character resolved-command limit, including local Bluetooth
address expansion; display preset selection was not changed by this update.

`userland/software-passcode-config.sh` enables the full integration by default
for explicit x1slte and unprofiled builds in this checkout when the tracked boot
line enables it. Explicit other DEVICE profiles retain their behavior, and an
explicit SOFTWARE_PASSCODE override is respected. The common x1slte profile also
sets SOFTWARE_PASSCODE=1. An intentional opt-out must account for any saved code;
never boot an existing protected device with just half this integration removed.

Both source builds and PREBUILT builds include the immediate-lock SpringBoard
code and restore binary. The clean assembly test starts with an empty fixture,
adds the tracked binaries/job/startup wrapper, and verifies a second assembly is
idempotent. It also checks profile selection, explicit overrides, the boot-line
budget and presence of the bridge in the tracked binary. This does not claim a
fresh full IPSW/firmware image was built or flashed in this session.

`AGENTS.md` points future repository agents here. `SHIPPING.md` records the release
entry; the shared workspace `NEXT.md` links this worktree and the integration
warning. Other existing worktrees are separate branches: they must merge or
cherry-pick the accepted commits before deploying, rather than assuming local
main updates their files automatically.

## Limits

This is software authentication for this SEP-less development port, not a Secure
Enclave emulator or equivalent hardware security. It does not add encrypted
storage/class keys, SEP-backed keychain access control, Face ID/Touch ID, UIK
certificates or Apple attestation. Existing key wrapping remains reversible.
Root/kernel access can modify the verifier; exported records permit offline
passcode guessing. Retry counters are volatile and are reset by reload/reboot;
no hardware anti-hammering, rollback prevention or cryptographic erase is claimed.
Existing real SEP keybags are rejected, not converted or made accessible.

The KDF runs only on credential operations, not in a monitor or polling thread.
Read-only state calls do not run the KDF. The experimental global state lock
serializes bag operations; latency must be measured on-device before enabling it.

## Evidence from the locally extracted iOS 16.5 cache

AppleKeyStore exports and call sites establish these selectors:

| Selector | Operation | Input shape |
| --- | --- | --- |
| 2 | create bag | 6 scalars; password pointer/length at 2/3 |
| 3 | save bag | handle scalar, opaque structure output |
| 4 | unload bag | handle scalar |
| 5 | set system bag | handle, system ID, optional password pointer/length |
| 6 | load bag | opaque structure input, handle scalar output |
| 7 | state | handle, flags scalar output |
| 8 / 0x20 | lock | handle |
| 9 / 0x0c | unlock | handle, user password pointer, length |
| 0x0f | change secret | 11 scalars, old pointer/length at 3/4, new at 5/6 |

Addresses (unslid): aks_create_bag 0x1f22d38e0, aks_load_bag 0x1f22d3b94,
aks_save_bag 0x1f22d3de8, aks_change_secret_se 0x1f22d3f1c,
aks_unlock_device 0x1f22d7788. MKBKeyBagChangeSystemSecret at 0x1ae386ca0
calls sharedService/changeSystemSecretFromOldPasscode:ToNew:withOpaqueData:.
These are development ABI observations, not a published stable interface.

## Validation and deployment

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror tools/passcode/test_software_keybag.c -o /tmp/test-software-keybag
/tmp/test-software-keybag
python3 tools/passcode/test_native_adapter.py
python3 tools/passcode/test_restore_files.py
python3 tools/passcode/test_build_integration.py
```

The host test compares the actual KDF against CommonCrypto's independent PBKDF2
implementation and verifies wrong-code rejection, change/remove, binary secrets, saved reload
requiring authentication, rate limits and malformed records. The actual kernel adapter also passes host tests under address/undefined-behavior
sanitizers with narrow IOKit mocks: privilege and input checks, transaction cloning,
serialization/reload and system installation. These mocks do not open a real
keystore. The kernel builds for Exynos GICv2 DEVELOPMENT. The SpringBoard library and iOS probe cross-compile.

`tools/passcode/keybag-probe.c` is read-only by default. Its isolated tests use
non-system temporary bags and were validated on-device. Native transaction
probes refuse to run when a passcode is present; do not use a real passcode in
command arguments, logs or test tools. The user enters it only on the phone.

`tools/passcode/prepare-root.py` adds the native keybagd job, guarded SpringBoard
library and boot restore helper to a prepared root. `SOFTWARE_PASSCODE=1` opts
userland builds into this integration. Kernel artifact:
`out/mach.x1slte-softpasscode`. Private deployment images and backups remain in
ignored `out/passcode-deploy-20261114`; never commit them.

The restore-file fixture test runs the actual read-only parser against private
synthetic files. It checks modes, parent permissions, hardlinks, symlinks,
interrupted writes, FIFOs and malformed plist data. Only fixture path and expected
owner are substituted for the host; the shipped helper still requires UID 0.

To build the standalone bridge against the local SDK:

```sh
clang -target arm64-apple-ios16.5 -isysroot xnu-deps/theos-mac/sdks/iPhoneOS16.5.sdk \
  -fblocks -fobjc-arc -dynamiclib -O2 -Wall -Wextra -Werror \
  userland/src/software_passcode_lock.m -framework Foundation -o /tmp/RedwoodSoftwarePasscode.dylib
codesign -s - -f /tmp/RedwoodSoftwarePasscode.dylib
```

Install that library with `userland/RedwoodSoftwarePasscode.plist` into the
persistent tweak directory above, then restart SpringBoard during an agreed test
window. Normal images include the same bridge in the prebuilt SpringBoard library.
A full phone reboot still needs user confirmation.

## Traps and recovery

- Treat Settings-enabled, first-boot unlock and repeat-lock enforcement as three
  separate checks. A locked kernel bag does not guarantee a locked UI here.
- Keep the guarded bring-up unlock helper and `autounlock=0`; never restore an
  unconditional-success credential operation to fix a UI integration problem.
- Native keybagd RPC callers need their normal device/verify entitlements. An
  unentitled probe's EPERM is not evidence the actual Settings flow is broken.
- Native persistence needs `no-effaceable-storage` and change-secret epilogue
  support. Candidate flag 4 must not mutate the live bag before save/install.
- Do not enable keybagd verbose logging: native log strings can include codes.
- Never run empty-to-empty system transaction experiments once a code is set.
  The diagnostic tools guard this condition; use temporary non-system bags.
- Preserve the legacy default UUID so enabling this backend does not arbitrarily
  change existing keychain identity. Never import an actual SEP bag.
- Restore saved protected bags locked before UI startup. Invalid existing files
  stop the launchd handoff rather than falling back to a no-passcode UI.
- Runtime retry counts are volatile. Root/kernel modification and offline attacks
  remain outside this software gate's protection.
- Keep private recovery images, keychain backups and captured logs in ignored
  `out/`. The known-good cooling recovery and pre-passcode keychain backups are
  retained locally; do not erase current keybags as a casual recovery step.
- Rebuilding all userland from stale inputs can drop live firmware/job changes.
  Prepare from the archived current tree, include only intended additions, and
  validate APFS contents before flashing. The legacy APFS verifier mishandles
  absolute symlinks; validate them as symlinks rather than following host paths.

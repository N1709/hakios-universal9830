# Apple Files on the HFS+ data volume

Apple Files now lists local storage, Downloads and available capacity on the
S20. The user confirmed the UI; real `FPCopyOperation` and `FPMoveOperation`
checks passed, including contents and source/destination existence. The check
creates a UUID-named test directory and removes only that directory on success.

## Default image build

`userland/build.sh` now builds and includes:

- `libredwood-files.dylib`, injected into only `fileproviderd` and
  `LocalStorageFileProvider` through the existing kernel mechanism;
- a hidden `org.hakios.LocalFilesHost` app containing the user's original local
  provider extension. This registers its data and app-group containers.

The provider is **moved**, not duplicated, while preparing the system image.
Its manifest entries move with it, preserving owners, links and empty locale
directories. The original extraction and its hardlinked files are not edited.
The host has no UI or background work; no polling service or kernel change is
required. Caller-supplied INJECT is still an explicit override of defaults.

`sysimage/select-system.py` now retains the 8,981,616-byte provider executable,
which the generic 4 MiB framework resource limit previously discarded.
A previously pruned extraction needs to be refreshed from the user's own IPSW
before rebuilding the system image. Adding the library to an old boot image
alone does not restore a missing extension on a fresh data volume.

For an existing phone, the tested runtime equivalent is a hidden host under
`/var/mobile/Library/HakiosLocalFiles/LocalFilesHost.app`, registered with
`uicache -p` using the existing containers. Its Apple executable/resources come
only from the user's extracted IPSW and are never committed here. No user
documents or FileProvider databases were reset.

## Build and checks

```sh
tools/storage/build.sh
```

Outputs under `out/files/`: the compatibility library, LocalFilesHost,
`storage-probe` and `check-operations`. SDK defaults to the local Theos
16.5 SDK; SDK/CC/LDID can be overridden. The library/host are also provided in
`prebuilt/userland` for the existing prebuilt image workflow.

`storage-probe` is read-only by default. It compares statfs, Foundation
filesystem attributes and the four NSURL capacity APIs. Its explicit
`register` command calls LaunchServices for the original framework extension;
this alone does not populate its container metadata and is not a complete fix.

`check-operations PATH_TO_DOWNLOADS` exercises FileProvider's real copy/move
operations on a newly created UUID folder. Sign this diagnostic with
entitlements extracted from the user's own fileproviderctl executable before
running it as mobile; it does not belong in the boot image. Failures keep the
UUID directory for diagnosis. No fixed device UUID is compiled into the tool.

Validation completed:

- library/host/diagnostics compile, shell syntax and diff whitespace pass;
- actual IPSW listing selects the omitted executable;
- staging keeps one provider, relocates locale metadata and preserves source
  hardlinks;
- mobile `fileproviderctl enumerate com.apple.FileProvider.LocalStorage`
  completes, lists Downloads and reports available capacity;
- both transfer operations pass and their disposable directory is removed;
- the original main boot image rebuilds byte-for-byte from its staging tree;
  the updated boot image changes only inject.conf and the Files library,
  growing by 8 KiB.

The latest camera kernel and boot arguments were preserved for the Files
boot-volume update. Recovery and BOOT writes were read back successfully.
A subsequent camera reboot (13:49:46 kernel) logged the library present and
init configuring its injection automatically; the camera task's next packed
image also contains the updated boot volume. This confirms boot-time inclusion
without another image change. UI and native transfer checks above were live
checks before that reboot.

## Root causes and source

Main upstream checked at `63fc2b4633bac3af8e57dadab9f5a8d4cb940438` still drops
the provider executable. The Sapphire port has the matching HFS+ compatibility:

https://github.com/Angelpro09xd/ios-sapphire/commit/04f94d5c6577d65bf9e112ed801c8b1fce9fff47

`userland/src/inject_files.c` adapts that implementation. Its origin and pending
file-specific license coverage are recorded in NOTICE/THIRD_PARTY_LICENSES.md.
It permits a writable local HFS+ FileProvider library, handles the port's absent
sandbox-container/URL-token services, and supplies guarded fallbacks for six
APFS-only filesystem helpers. The function entry signatures matched on this
16.5 phone; other signatures are logged and left untouched.

There was also a stale standalone registration from an earlier repair attempt
at `/var/mobile/Library/LocalStorageRepair`. It had no data/app-group metadata.
After targeted unregistration of that stale entry and an incomplete system
entry, registering the hosted extension supplied the existing containers and
allowed discovery. Plain registerPlugin: alone left container metadata empty.
The provider's bundle identifier stays com.apple.FileProvider.LocalStorage.

Only /private/var is writable data: statfs and Foundation correctly report
about 108 GiB total and 105 GiB free. The full read-only APFS system image is
expected to report zero free bytes; it is not the data volume.

## Save/import picker follow-up

The same resource cutoff also omitted the 5.5 MB `SaveToFiles` and
`com.apple.DocumentManager.Service` executables. Their bundle metadata alone
was present, producing “app extension could not be found” for Safari saving
and app import pickers. Both executables are now retained and moved into the
same hidden host, with their manifest metadata. The compatibility injection
also names these two executables; the complete default list remains below the
current kernel's 512-byte configuration limit.

Safari import/export and the TrollStore + picker were user-confirmed after
this repair. TrollStore needed to be closed/reopened once because its running
UI had cached the missing extension. No picker UI replacement or installer
binary patch was needed. The earlier copy/move checks tested provider data
operations; this follow-up covers the separate document-picker executables.


## Finder app documents

The host is named **Jesvi Files** and advertises native file sharing and
in-place document access. Finder's Files tab can expose this app's Documents
folder through House Arrest; it does not expose the whole filesystem or merge
Safari Downloads into that folder. Copy files by dragging onto the app row.
The Sync button is separate. See `tools/usb/README.md` for the unprotected local
Finder compatibility policy and its pairing requirements. Grey backup/media
controls and category storage accounting are not resolved by the Files fix.


Finder filters the sharing list to user apps. `register_finder_host` uses native
MCM/LaunchServices to register a containerized copy of the hidden provider host
under its existing identifier, preserving data/plugin containers. The default
launchd job runs this once per boot and retries startup failures. Both app and
file visibility were confirmed after this registration. No uicache executable
or jailbreak bootstrap is required by the helper.

## Native-registration regression (2026-11-20)

The framework-only `registerPlugin:` path created a LaunchServices proxy with
no entitlements, data container or group-container mapping. A successful call
and a non-nil proxy did not mean the provider could run: FileProvider exposed
Photos and iCloud domains but no local domain. Re-running that helper did not
repair the metadata.

`register_finder_host` now prepares a hidden registration host automatically
from the three immutable framework extensions plus `/redwood/bin/LocalFilesHost`.
It uses the existing `org.hakios.LocalFilesHost` container and local-storage group,
so documents stay in place. The source framework bundles remain unchanged.
The host's `SBAppTags = [hidden]` is included in its Info.plist and registration
record; it supplies metadata without a home-screen icon. This is an internal
registration app, not a replacement Files interface.

Readiness requires all three extension executables and Info.plists, each plugin's
data-container URL, and LocalStorage's group mapping and entitlement. Incomplete
standalone registrations are removed before the complete record is registered.
An incomplete generated host is renamed aside, never deleted with its contents.
Failure returns nonzero so the launchd job retries.

Validation: helper builds with the iOS 16.5 SDK; live repair/clean-install checks
are pending while the phone's USB connection is intermittent. Do not treat proxy
presence alone as a passing Files test.

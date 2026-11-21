# Cellular Settings UI

Status: **installed; page visible and user-confirmed** (2026-11-11, ~10:00).
This can be installed independently of the untested loader/UEFI memory changes.

What it adds to Settings:

- A green Cellular row after Bluetooth, replacing the native row if present.
- A native Cellular page with a Cellular Data switch that saves the user's
  preference across Settings launches.
- Network: **No Service**; Calls: **Unavailable**. The footer explains that the
  setting applies when cellular service is ready.

The switch stores an intended setting. It does not claim to start a radio or
route traffic before the Samsung driver exists. No fake SIM, signal bars,
carrier name, CommCenter replies or call-success state is installed.

## Build and install

From `work/hackios-cellular`:

```sh
bash tools/cellular/build-ui.sh
python3 -B tools/cellular/install-ui.py install --session YOUR_SESSION
```

The installer requires that YOUR_SESSION already owns the research workspace's
normal phone lock. It refuses a free lock or another session's lock and does
not acquire, steal or extend one itself. The first attempt exited 3 while B
owned the phone, before SSH/uploads. After B's last heartbeat (09:08:09) expired
under the project's 45-minute rule, the normal lock tool granted codex-cellular
the phone at 09:53. Only this tweak was installed; no reboot, firmware update,
modem command or camera change was made.

The current phone holder can install the prepared artifact using their own
session name without merging branches, rebuilding the system image or rebooting.
Close and reopen Settings after installing. Only the Preferences process loads
this tweak. No automatic process restart is performed.

The installer puts `RedwoodCellular.dylib` and its executable filter into
`/var/jb/usr/lib/TweakInject`. Files are first uploaded under inert names, then
renamed; an already mapped dylib is never overwritten in place. Previous copies
are saved under a timestamped `RedwoodCellular-backup-*` directory. Source and
artifacts contain no Apple binaries or vendor firmware; generated artifacts
remain ignored in `out/cellular`.

Remove, under the same lock rule:

```sh
python3 -B tools/cellular/install-ui.py remove --session YOUR_SESSION
```

Removal moves only this tweak and its filter into a backup directory. Reopen
Settings to unload it. The saved data preference can remain for a later install.

## Implementation and backend contract

- Source: `userland/src/cellular_settings.m`, an independent Objective-C dylib.
  It uses the port's existing Preferences tweak loader; no edits to the shared
  `libredwood`, Settings executable, MobileGestalt or system image.
- Root hook: only `PSUIPrefsListController -specifiers`. An inherited method
  receives a subclass override instead of patching `PSListController` globally.
  The hook waits for dyld images, runs on the main queue, and refreshes an
  already-created root controller. iOS 16.5's local cache confirms
  `PSListController`'s `_specifiers` NSArray backing ivar; it is looked
  up by name without hardcoded offsets.
- Preference: mobile user's `com.redwood.cellular`, boolean
  `CellularDataEnabled` (default off). Change notification:
  `com.redwood.cellular.data-changed`. Future Samsung session service must read
  this domain in the mobile-user scope, enforce the actual session/route state,
  and publish real registration/call status before these static status values
  can be replaced. A preference notification alone is not a packet transport.
- Filter restricts loading to executable `Preferences`.
- Bounded lifecycle diagnostics in the mobile Preferences directory:
  `com.redwood.cellular-ui.log`. It records process ID and menu/row/page/preference
  events only; no SIM identifiers, carrier/account names or other user data.

## Runtime correction and evidence

The first installed version hooked `PSUIPrefsRootController`, which exists in
this iOS build but was not the visible iPhone root. A live controller inspection
showed `PSSplitViewController -> UINavigationController -> PSUIPrefsListController`.
The correction hooks the displayed list class. Existing Settings was restarted
alone, and `uiopen --url prefs:root=MOBILE_DATA_SETTINGS_ID` opened our page.

Final observed process 377 emitted `menu-hook-installed`,
`root-specifiers-entered`, `cellular-row-ready` and `cellular-page-opened`,
and stayed running. The user then explicitly confirmed seeing Cellular Data,
Network — No Service and Calls — Unavailable. Earlier process 313/353 logs are
failed UI attempts, not proof of a working page. No screenshot was claimed.

## Validation checklist

- [x] Compile arm64 for iOS 16.5 with `-Wall -Wextra -Werror`.
- [x] Ad hoc sign and pass `codesign --verify --strict`.
- [x] Inspect linked frameworks and parse the executable filter plist.
- [x] Confirm local iOS 16.5 class/backing-ivar names without committing Apple data.
- [x] Confirm installer refuses another session's phone lock without device writes.
- [x] Install as phone holder; reopen Settings; reach the corrected Cellular row.
- [x] Open the page; process stays running; user confirms switch/status are visible.
- [ ] Verify back navigation and preference persistence across closing/reopening.
- [ ] Toggle, close/reopen Settings and verify preference persistence.
- [ ] Remove/reopen and verify the normal Settings root returns.
- [x] Leave the corrected UI installed and visible for the user.

LTE/calls still require the Samsung modem implementation. The visible page is
UI progress and a saved preference, not a working cellular connection.

# Native Apple Account experiment

Isolated worktree: `hackios-apple-account`, branch `feat/s20-apple-account`,
based on accepted graphics commit `9e0d7cbe`. This does not modify the boot,
keybag/passcode chain, graphics configuration or existing account identities.

## First checks, 2026-11-16

- Phone: native `akd` and `accountsd` are running, one launch each. The installed
  akd job publishes `com.apple.ak.anisette.xpc` and the other AuthKit endpoints.
- Mac: macOS 27.2.1, arm64. AOSKit and AuthKit are present. The older
  `AOSUtilities retrieveOTPHeadersForDSID:` method exists with the expected ABI,
  but the local probe receives an empty dictionary. This does not establish
  that every native Mac authentication path is unavailable.
- The current AuthKit Anisette controller exists on both platforms. Plain
  diagnostic clients receive `NSCocoaErrorDomain 4099`; that result alone is
  not evidence that Settings has a broken XPC connection.
- On the phone, an explicitly signed probe with Preferences' existing
  `com.apple.authkit.client.internal` entitlement reaches the service and gets
  `AKAnisetteError -8004` with provisioning disabled. No Anisette data is
  returned. The exact underlying cause and normal provisioning outcome are
  unverified. Do not turn this into an authentication-success stub.
- User has authorized one native Settings test using a secondary account,
  entering credentials and verification codes on the device. It showed “Verification Failed — MID is invalid.” A later trial reached the
  password screen but still reported invalid MID. Full sign-in is not verified.

## Reproducible probes

`tools/apple-account/mac-anisette-probe.m` reports method compatibility and,
with `--generate`, only whether the two expected header values are nonempty.
It never prints or persists those values. This generates metadata, not an
account sign-in.

`auth-metadata.m` lists Anisette-related Objective-C method names and ABI types.
It calls no authentication or provisioning operation.

`anisette-fetch-probe.m --fetch-no-provision` requests existing data through
AuthKit with provisioning explicitly false. The separate `--provision-if-needed`
flag invokes normal device provisioning without account credentials or resets.
A bounded native provisioning attempt returned underlying ADI error -42410;
the existing-data read reports -45061 (not provisioned). Results contain only availability
booleans and error domain/code. The iOS entitlement file matches one permission
already held by Settings. It is for this development kernel; it does not grant
that permission to arbitrary executables on a stock Mac.

Compile native Mac probes with `clang -fobjc-arc -framework Foundation`. For the
phone, use the iPhoneOS16.5 SDK, arm64 target and `-Wl,-no_fixup_chains`, then
sign the fetch probe with `ldid -Stools/apple-account/anisette-fetch-ios.entitlements`.
Run the phone probe as mobile in its user bootstrap context. Bound diagnostic
process runtime when experimenting with synchronous private APIs.

## Privacy and next steps

- Enter account passwords and verification codes only in native device UI.
  Never put them, account cookies, header values, account databases or keychain
  backups in source, command arguments or logs. Do not extract Safari sessions.
- Do not enable verbose AuthKit/keybag logging or retain authentication request
  bodies/headers. Capture only necessary error codes/status metadata.
- Read-only probe artifacts belong in ignored `out/apple-account/private/`.
- First obtain the actual Settings failure or success. If Anisette is the
  verified blocker, evaluate a private provider and a precise native adapter.
  Do not deploy an HTTP helper or alter client identity speculatively.
- Mac-assisted app signing and messaging projects demonstrate individual
  protocols, not universal Settings/iCloud support. Full account sign-in, App
  Store, iMessage and other services need independent verification.

Research starting points: [Anisette implementation](https://gist.github.com/JJTech0130/fa240e86f639c3a3519599417ffccf61),
[SideStore Anisette documentation](https://docs.sidestore.io/docs/advanced/anisette),
[OpenBubbles Mac helper](https://github.com/OpenBubbles/Mac-Hardware-Info).

## Isolated provider and native adapter

A credential-free experiment using the x86_64 slice of this Mac's generic
`CoreADI.framework/adid` succeeded under the reviewed Unicorn emulator:

- Upstream os-findmy-e commit `d4d2a832baf47d1cf43d989879950ac2aec45f47`.
- Generic Apple binary SHA-256 `5347c971c08187f0b3f605bd61fe5566eb5f54a04b5679d063935c4e0ad251d2`.
- Entry point obtained from LC_MAIN rather than the older backend's fixed offset.
- Virtual caller uses static akd code-signature role `adi-client=347420559`.
  This is executable metadata, not a host serial, account or hardware secret.
- Identity is randomly minted by the provider. One Apple provisioning exchange
  succeeded; subsequent offline reads return nonempty MID/proof. This does not
  prove an Apple Account login is accepted.
- Host identity/store and access token stay in ignored private state with mode
  0700 directories/0600 files. No real iPhone identity, keychain, Safari session,
  account database, account password or verification code is copied.

`emulated-provider-probe.py` defaults to offline status. `--provision` is explicit,
contacts only Apple HTTPS endpoints, and records its attempt to avoid silent
reprovisioning. It emits booleans/stages/codes, never header values.

`private-provider-server.py` binds 127.0.0.1 only. Token-authenticated requests
accept exactly a protocol version and random nonce; credential/extra fields are
rejected. Replies contain a short-lived machine-data snapshot and matching nonce.
No account login endpoint exists. The S20 reaches it through a host-key-verified
SSH reverse tunnel, using loopback at both ends. Live boundary checks passed:
unauthenticated request refused, extra fields refused, authenticated data fresh.

`anisette-lab-bridge.m` is a temporary injection into Preferences and akd. It
validates private method/property ABI, callback descriptor, response nonce/time,
header types and routing value. It supplies actual provider-generated
AKAnisetteData, coherent virtual machine headers and the daemon's private data
read. Apple's public XPC client permission checks and account/password/2FA
verification remain in place. An unentitled diagnostic client is still refused.
No authentication-success stub is introduced.

Initial bridge failure: USB tunnel loss exposed an NSError autorelease lifetime
bug, crashing Preferences/akd. Fixed by keeping error out-parameters in the
caller's pool. Native sync/async and native header formatter tests now pass with
both connected and unavailable helpers. The formatter reports an error rather
than proceeding with unusable machine data. Hook originals are captured before
publishing their replacements.

The earlier Settings deep link `prefs:root=APPLE_ACCOUNT` also faults this image
in Preferences' missing-specifier handling, including before the bridge trial.
Open Settings normally by bundle/icon instead. Do not patch this unrelated fault
into the authentication adapter.

The initial client-only adapter did not cover akd's private native data read.
Version 4 additionally handles
`AKNativeAnisetteService._unsafe_anisetteDataWithRoutingInfoForDSID:withError:`.
An **unmodified**, properly entitled client now receives real machine data over
native XPC; the unentitled client still returns 4099. User sign-in retry with this
correction returned account/password/verification failure wording rather than invalid MID;
no verification prompt or successful login was confirmed. This does not prove password
or machine-identity acceptance. See APPLE-ACCOUNT-HANDOVER.md for the latest state.
No full native iCloud/App Store support is claimed.

### Native payload consistency correction (v6)

A fresh web sign-in was confirmed by the user after password rotation. A
credential-free construction probe then demonstrated a real adapter gap: the
native CPD used virtual MID/routing but retained native device ID and serial,
and omitted the virtual local-user ID. The generic HTTP-header smoke test did
not cover this path.

`audit-native-payload.py AKD_BINARY` reproduces the relevant static call sites
and constants, refusing any binary other than the reviewed hash. It requires
capstone and never accesses a device, account or network.

`native-payload-probe.m` is a temporary, one-shot dylib for akd. It creates a
fresh empty context and invokes parameter construction only, without executing
an SRP request. Its private report contains only presence and equality booleans
for selected machine metadata, never the parameter dictionary or credentials.

The v6 bridge wraps the native parameter completion with ABI and layout guards.
`payload-identity.h` aligns device ID/local-user ID/serial in CPD and additional
headers and the top-level client description. Native machine ID/routing must
already match. Native proof and unrelated parameters are preserved. Unsupported
MLB/ROM metadata is omitted. Missing identity or an unknown layout returns a
laboratory error rather than using inconsistent data.

Validation: compile/run `test-payload-identity.m` with Foundation on the Mac;
fixtures test immutability, preserved proof/capabilities, alignment and refusal
of missing/mismatched identity. On-phone native sync/async and HTTP formatter
smoke tests pass. Empty-context native construction confirms CPD device ID,
local-user ID, serial, MID/routing and client description match the provider.
The one-shot diagnostic is unloaded after testing. A user sign-in result with
v6 is still pending; this is not evidence of server acceptance.

### iCloud account request (v7)

Subsequent v6 user trials reached native SRP success and AuthKit completion
without a reported error, followed by AALoginAccountRequest HTTP400 and native
com.apple.appleaccount:400. Settings text corruption was separately reproduced
and worked around by hiding Metal discovery only in Preferences; user confirmed
readability. No completed native iCloud account was observed.

The iCloud login request builds identity headers independently of the AuthKit
formatter. V7 adds a scoped AALoginAccountRequest.urlRequest adapter for HTTPS
setup.icloud.com, preserving the native account verification and body while
overlaying validated provider headers and the Device-UDID alias. The pure helper
has synthetic preservation/scope tests. An account-free, unsent native fixture
confirms selected headers match the virtual provider. This fixture does not
establish which headers a real account originally supplied or server acceptance.
Native userInfo.client-id is unchanged because its expected binding is unverified.
When provider metadata is unavailable the iCloud getter returns the original
native request and records an event; this fallback is not a fail-closed guarantee.

`network-error-probe.m` records only hostname, HTTP status, request/response class,
error presence and bounded NSError domain/code chains. It never inspects native
authentication-result contents or request/response bodies. Its native response
and loopback smoke tests preserve original behavior. These diagnostic injections
are temporary and must be removed independently during rollback. Latest runtime,
user trial and rollback details are at the top of APPLE-ACCOUNT-HANDOVER.md.

### Native user-owned Hackintosh helper (v8)

User provided a working Intel macOS15.8.1 Hackintosh, natively signed into the
secondary account, and explicitly authorized using it for comparison/helper
research. Credential-free probes confirm native AOSKit machine proof and
AAAbsintheSigner signature availability. Device ID getters agree across native
frameworks. No account store, keychain, password or token is accessed by this
helper; the generic proofs are machine-level material and remain private.

`hackintosh-provider.m --check` emits availability only. Its `--stdio` mode is
for a broker-owned SSH pipe exclusively: it emits short-lived machine metadata
and signatures, never run/forward its replies as ordinary tool output. The
protocol accepts only version/nonce. Signing input is fixed empty data, no
account or arbitrary payload. Routing84215040 is the known AltServer native-Mac
hint, not a value retrieved from this Mac. Other selected identity fields and
software versions are native. The helper does not reset or provision identities.

`native-provider-server.py` binds Mac loopback only, uses token-authenticated
requests and pinned SSH, and preserves nonce/expiry checks. Phone loopback49742
reaches broker loopback49741 over a reverse SSH tunnel. Proof bytes exist only
in this private transport and process memory. There is no public listener or
account-login endpoint. This research requires both computers awake/connected.

V8 validates optional validation data and native User-Agent metadata. It supplies
X-Mme-Nas-Qualify to the scoped iCloud login request while omitting it from the
generic GSA formatter. On-phone unsent request and daemon construction tests
pass, as do boundary/error/preservation checks. These tests do not establish
server acceptance or whether empty-input validation is sufficient for this
service. User sign-in trial is pending. See handover for exact live rules,
owned processes and rollback; nothing is integrated into the boot/system image.

## Runtime and rollback

This is a host-dependent temporary trial, not clean-image integration. Accepted
main/master, the graphics image, kernel, passcode chain and system image remain
unchanged. No firmware or donor-device security setting is changed.

Live setup uses Mac loopback port 49731, SSH reverse forwarding to phone loopback
49732, and private connection configuration under
`/var/mobile/Library/RedwoodAppleAccountLab/`. Only generated lab metadata goes
through that tunnel; account input stays in native Settings and Apple's HTTPS.
Wi-Fi forwarding is used because this phone's USB network can go inactive.

To unload: remove only the temporary anisette bridge rule from kern.exec_inject
(or restore the saved pre-trial rule when no unrelated edits exist), restart akd
and close/reopen Settings. Restoring the rule alone does not unload an already
injected library. Stop the owned tunnel/provider processes and remove the private
connection file. Keep the isolated provider state private for diagnosis; do not
reset/reprovision it automatically. A reboot naturally restores the shipped root
configuration, but ask before rebooting for this experiment.

A secondary account reduces account-data exposure, not the possibility of Apple
rate limits or account restrictions. Keep the genuine iPhone unmodified. Do not
use public shared identities or borrow a donor's keychain. Native authentication
remains experimental until each stage has been verified.

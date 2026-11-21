# Apple Account laboratory tools

## Current shared installation

One tested library now serves native account/store processes. Read
[shared-layer deployment](../../docs/x1slte-apple-account-shared-layer.md) and the
current handoff before older individual-adapter instructions below. Build with
build-shared-layer.sh; manage with shared-layer.py status/rollback. The native
iCloud account remains authenticated/active with warm-up complete. Media &
Purchases is the current unverified failure; media-error-events.log captures
native NSError codes. The Mac helper and pinned hotspot IPv6 connections remain
required. Runtime injection is configured for the current boot.


## Verified account setup — 2026-11-17

Native AppleAccount now stored and reports authenticated=true, active=true,
warmingUp=false after successful real Apple responses and background verification.
Update-account banner disappeared. Current user test: individual iCloud/device/
FindMy/AppStore pages; their complete functionality is not yet established.
The private Mac helper is still required. Read current handoff before older
historical unresolved-state notes below. Account-health-v2 uses the existing
private account-presence read entitlements; an unentitled count0 is not proof
of account removal. No account credentials or tokens are printed by health tool.


## Current hotspot access — 2026-11-17

Use `ssh -F out/apple-account/private/account-hotspot-ssh.conf s20 ...` now.
The hotspot is IPv6/NAT64;192.0.0.2 is translated, not a peer address. Both devices
were verified against original pinned keys over link-local IPv6. Task-private SSH
config preserves original global configuration unchanged. Current helper host is
`user@hackintosh.example` via BonjourIPv6 and original host-key alias.

Current services:

```sh
python3 -u tools/apple-account/phone-helper-tunnel.py --ssh-config out/apple-account/private/account-hotspot-ssh.conf
python3 -u tools/apple-account/native-provider-server.py --host user@hackintosh.example --ssh-config out/apple-account/private/account-hotspot-ssh.conf --helper hackintosh-provider-v8 --key out/apple-account/private/hackintosh_ed25519 --known-hosts out/apple-account/private/hackintosh-known-hosts --token-file out/apple-account/private/broker-token
```

The account is stored (count1), initial server/services and preflight save/CDP have
succeeded. Final setup/update remains unverified after recent network outage.
Current auth adapters: bridge16 Preferences/accountsd, original bridge14 akd;
public-session-v5 Preferences/accountsd; delegate-session-v2 accountsd with fresh
credential-free metadata endpoint (no old90sec snapshot dependency). Passive daemon
probe1 logs application service statuses. Always read current handoff before older
runtime rows below.


Start with [the current handoff](../../docs/APPLE-ACCOUNT-HANDOVER.md).
Native iCloud sign-in remains unresolved. These tools do not bypass Apple account
verification. Never supply account credentials as tool arguments or use the
passwords pasted in chat. User enters secrets in the phone's native UI.

All commands below run from this worktree. `out/apple-account/private/` is ignored;
it holds machine proofs/state, the private SSH key and broker token. Never commit,
print, distribute, or indiscriminately search its contents.

## Coordinated Media capture (current)

Read the latest handoff first. Media sign-in remains unresolved. Use media-capture.py prepare/watch to capture one full attempt with timed per-request metadata and protected decrypted response files; request bodies are excluded. media-transaction-recorder-v1.dylib is installed as an additive recorder, replacing unused observer addons. Byte-for-byte native HTTP200/401 fixture tests passed under root and mobile. Keep raw files in ignored private out; never print or commit them. The older pending comparison instructions below are historical.

## Current pending test

Read the current handoff before old instructions below. The armed comparison now runs **on the phone**, public NSURLSession worker11 after native401, with exact phone body/helper snapshot. Old observer `mac_http_status` label is misleading for this test; local service logs worker_target=phone. Both Mac HTTP200 and application service status0 were confirmed in the preceding comparison. Current helpers: native provider7, broker auto-recovery, phone-helper-tunnel.py supervisor. No working native S20 sign-in yet.

## Active components

| Source | Current deployed artifact | Purpose |
| --- | --- | --- |
| anisette-lab-bridge.m | phone bridge15 Preferences; bridge14 akd | Native data/header/signing adapter |
| hackintosh-provider.m | Hackintosh ~/.local/share/redwood-account-lab/hackintosh-provider-v7 | Native machine metadata and scoped signing |
| native-provider-server.py | local process, loopback49741 | Private pinned-SSH broker |
| network-error-probe.m | phone passive9 Preferences; passive7 native daemons | Passive error/status and failed-response summaries |
| srp-error-probe.m / srp-error-category.h | phone /var/tmp/srp-error-probe-v3.dylib | SRP outcome and exact native failure categories |
| icloud-signing-metadata.h | included by phone/helper | Strict non-secret metadata schema and exact-byte digest |
| payload-identity.h / icloud-request-identity.h | included by bridge | Selected metadata alignment, native credential/body preservation |

Older `private-provider-server.py`, `emulated-provider-probe.py`,
`nac-provider-probe.py` are historical/emulated paths; do not substitute them into
the active native setup without a justified experiment.

## Access and runtime inspection

Check workspace coordination before device changes:

```sh
~/ios-port-research/tools/team.sh who
~/ios-port-research/tools/phone-lock.sh status
```

A new agent should acquire/claim using its own session name after checking current
ownership. Preserve host checks. Do not reboot without user approval.

```sh
ssh -o ConnectTimeout=4 s20 '/redwood/bin/rwsys kern.exec_inject'
ssh -o ConnectTimeout=4 s20 'tail -30 /var/mobile/Library/RedwoodAppleAccountLab/status.log; tail -15 /var/mobile/Library/RedwoodAppleAccountLab/network-events.log'
```

Do not print phone connection.json or local broker-token. Logs are metadata only;
`account_failed_response_summary` is emitted only after failed iCloud setup, not
when AuthKit fails before that stage.

Native helper check, safe boolean output only (may initialize native signing
context, but does not perform an account login):

```sh
ssh -i out/apple-account/private/hackintosh_ed25519 -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=out/apple-account/private/hackintosh-known-hosts -o ConnectTimeout=5 user@hackintosh.example '.local/share/redwood-account-lab/hackintosh-provider-v7 --check'
```

**Never run the helper's --stdio mode directly in a terminal or text-returning
tool.** Its replies contain short-lived proof material. The broker consumes them
privately in memory.

## Restarting owned transport if it has stopped

First inspect listening port/process command. Do not start duplicate brokers or
kill arbitrary Python/SSH processes. Last observed PIDs are in the handoff; they
are not durable. Stop an existing process only after verifying its command and
ownership. Keep both Macs awake.

Run broker in a persistent tool/shell session:

```sh
python3 -u tools/apple-account/native-provider-server.py --host user@hackintosh.example --key out/apple-account/private/hackintosh_ed25519 --known-hosts out/apple-account/private/hackintosh-known-hosts --token-file out/apple-account/private/broker-token
```

Run the owned reverse tunnel in a separate persistent session if absent:

```sh
ssh -o Hostname=192.0.2.10 -o HostKeyAlias=10.77.0.2 -o ConnectTimeout=4 -o ExitOnForwardFailure=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=2 -N -R 127.0.0.1:49742:127.0.0.1:49741 s20
```

Phone connection.json already targets127.0.0.1:49742 with the existing private token.
Do not generate a new identity, rotate the token or edit configuration merely to
restart transport. Old emulated ports49731/49732 are separate idle standby paths.

## Rebuilding current sources

Current source corresponds to bridge v13, native helper v5 and passive probe v5.
Use a new artifact version for edits; never silently overwrite the meaning of an
old evidence artifact. The helper and bridge must share the same schema header;
the Python broker independently mirrors that validator.

```sh
AA_LAB_SDK=~/theos/sdks/iPhoneOS16.5.sdk
clang -target arm64-apple-ios16.5 -isysroot "$AA_LAB_SDK" -dynamiclib -fobjc-arc -fblocks -framework Foundation -Wl,-no_fixup_chains -Wall -Wextra tools/apple-account/anisette-lab-bridge.m -o out/apple-account/private/anisette-lab-bridge-v13.dylib
ldid -S out/apple-account/private/anisette-lab-bridge-v13.dylib
clang -target arm64-apple-ios16.5 -isysroot "$AA_LAB_SDK" -dynamiclib -fobjc-arc -fblocks -framework Foundation -Wl,-no_fixup_chains -Wall -Wextra tools/apple-account/network-error-probe.m -o out/apple-account/private/network-error-probe-v5.dylib
ldid -S out/apple-account/private/network-error-probe-v5.dylib
clang -target x86_64-apple-macos13 -fobjc-arc -fblocks -framework Foundation tools/apple-account/hackintosh-provider.m -o out/apple-account/private/hackintosh-provider-v7
```

SCP uses the same pinned SSH arguments above. Re-sign CLI probes that need native
AuthKit access using `ldid -Stools/apple-account/anisette-fetch-ios.entitlements`.
Do not replace installed injection rules wholesale: read/verify/replace only the
specific account rule, then close Settings and restart affected native daemons.
Changing the rule alone does not unload the old library from existing processes.

## Focused verification

| Tool | What it establishes | What it does not establish |
| --- | --- | --- |
| bridge-smoke.m (phone bridge-smoke-v4) | Sync/async data delivery, callback ABI, native formatter | Successful Apple authentication |
| icloud-request-probe.m (phone icloud-request-probe-v11) | Unsent account-free native three-service request and body-bound signature; distinct service caches | Apple accepts login/identity |
| network-probe-smoke.m (phone network-probe-smoke-v3) | Error preservation, loopback status, failed-response privacy | Meaning of real server rejection |
| native-icloud-contract-probe.m | Mac/phone account-free native request structure | Correct real account credential handoff |
| test-icloud-signing-metadata.m | Strict schema permits known metadata, refuses tokens/unknown services | Complete sign-in compatibility |
| account-identity-match.m | Explicitly requested identifier comparison, counts only | Password validity or session validity |

Schema check on local Mac:

```sh
clang -fobjc-arc -framework Foundation tools/apple-account/test-icloud-signing-metadata.m -o out/apple-account/private/test-icloud-signing-metadata
out/apple-account/private/test-icloud-signing-metadata
```

The following phone fixture sends NO login request and supplies NO account:

```sh
ssh -o ConnectTimeout=4 s20 '/var/jb/usr/bin/su mobile -c "/var/tmp/icloud-request-probe-v11 /var/tmp/anisette-lab-bridge-v13.dylib"'
```

Do not enable RW_LAB_LOOPBACK_DELIVERY unless the deliberately account-free local
receiver/tunnel fixture is prepared; its port49743 was removed after testing.
The network smoke's HTTP501 case depends on the existing private loopback49732
standby responder. If absent, prepare an owned local-only501 responder before
running it; do not replace it with an Apple account endpoint.

Native contract probe must dlopen IntlPreferences on Mac (already in source).
Never attach account credentials to its nil-account fixtures.

## Restrictions and known gaps

- No unconditional authentication-success stubs or disabled access checks.
- No donor iPhone identity/secret extraction, Mac account token/keychain export,
  Safari cookie reuse, broad packet capture or TLS interception.
- Native helper DSID/routing semantics remain unverified for all sign-in stages;
  the hook ignores requested DSID and uses a documented compatibility routing hint.
- Some request-construction fallbacks use the original native path on helper loss.
- Current tools/docs are untracked local work. Do not git clean. Keep private out
  excluded from source commits and handoff bundles.
- Full acceptance requires completed native account state/UI. Individual Apple
  services need their own tests after that; GSA success alone is insufficient.

`icloud-credential-contract.m` uses an in-memory synthetic account only. Phone
checks with and without bridge13 produce the expected Basic authorization. It
never saves an account or sends a request. `test-srp-error-category.m` verifies
exact native-label classification and rejection of arbitrary/private text.

Passive v6 adds `credential-handoff.h`: 120-second in-memory keyed equality
checks between native AuthKit output and the outgoing iCloud authorization.
No fingerprint/key/credential values are emitted. Validate with
`test-credential-handoff.m`; stale or cross-process references are inconclusive.

Latest deployed bridge14 aligns native AppleAccount legacy `X-Apple-MD` /
`X-Apple-MD-M` as well as AuthKit I-MD aliases, using one provider snapshot.
Passive7 records only pair presence/equality. Probe-v12 verifies this without
sending a login. Older versioned commands above reproduce historical fixtures;
use current artifacts identified by the handoff for the live setup.

Passive8 adds selected native authentication-context fields, keyed alternate-DSID
comparison and server challenge scheme. See `docs/APPLE-ACCOUNT-BINARY-NOTES.md`
for native PET provenance and `annotate-objc-stubs.py` for static analysis.
No Mac sign-out/re-login is required for these diagnostics.

Passive8 was rolled back to7 after a pre-password4099 regression; causality
not yet established. Read the latest handoff before deploying it.
`auth-service-health.m` verifies a read-only native AuthKit RPC without login
or printing passcode state. Native RPC and no-provision Anisette checks pass
after rollback/restart; UI password-screen verification remains pending.

Current passive9 runs only in Settings and removes pre-call context inspection.
Native daemons retain passive7. `network-probe-smoke-v4` exercises synthetic
authentication success/error callback preservation before loopback tests.
It never invokes a real authentication method. See latest handoff for split rules.

`native-login-log-summary.py` runs on the working Mac during a user-initiated
native re-login. `--system-log` uses already available noninteractive sudo only
for log visibility. It stores fixed summaries, never raw messages. Native
DTrace attach remains blocked by that Mac's current security configuration.
Collector runtime details are ignored private artifacts, referenced in handoff.

Latest bridge15 preserves native iCloud User-Agent instead of forcing the
nominal AADeviceInfo value; other behavior matches bridge14. It runs only in
Settings, while akd remains on14. `export-account-static-text.m` exports static
framework text and selected import names from its OWN process for binary
analysis, never daemon/account memory. See binary notes for the successful
Mac comparison and the proven PET-versus-raw-password mapping.

Owner-authorized one-shot PET comparison is isolated from the original native
machine-proof broker. `native-pet-comparison.m` sends one native Mac iCloud
request with the freshly returned S20 PET through private stdin; it never saves
an account. `pet-comparison-server.py` uses a separate one-use bearer and local
49751/phone49752 tunnel. `pet-comparison-trigger.h` runs only with a private
one-shot phone config and after native401. It retains the PET in memory at
most120s and never logs it. See latest handoff for authorization, exact state,
interpretation limits, and cleanup; never print connection/config/token files.

The one-shot comparison was disarmed before obtaining a PET after another
pre-password4099. Its local server/tunnel stopped. Current Settings diagnostic
is restored to9. `code-signature-health.m PID` is a read-only csops/selected
entitlement probe; no native access check is modified. See latest handoff.

Current one-shot test uses `daemon-pet-comparison.m` in akd, with Settings left
on known-working passive9. The local comparison server waits for the existing
phone401 metadata before invoking native Mac worker. Read latest handoff;
client-side passive10 remains rolled back. Use code-signature-health-v3:
CS_OPS_IDENTITY is11; earlier v1/v2 used the wrong operation for identity.

Critical deployment trap: an already-mapped signed dylib overwritten at the same
phone pathname can lose process code-sign validity on this port. Observer1 trial
showed missing CS_VALID and EINVAL identity/entitlement reads; fresh immutable
observer2 restored validity, with no security bypass. Every binary edit after
a test needs a NEW pathname. Do not overwrite a tested artifact. Current live
observer is2; Settings remains passive9. Read latest handoff before deployment.

SRP3 now records native failed-server-response errorCode and resync/reprovision/
URLswitch/retry/secondary-action flags via a passive original-preserving hook.
`test-srp-server-summary.m` validates argument/completion identity and excludes
raw server text. Fresh artifact used; native code validity verified after load.

Use native-pet-comparison-v3, which builds a native request, signs its body via
native AAAbsintheSigner and uses public NSURLSession for reliable HTTP status.
Worker1 had an incorrect private AARequest callback ABI (native passes
request,response,error), so its status0 result is inconclusive. Worker3's
200/401 loopback transport and absent-Authorization fixture are verified.
Do not reuse worker1/2 or mistake the synthetic SRP8291/-20101 fixture for a
real server response. Current live phone libraries did not change for this fix.

FairPlay restoration: `fairplay-service.py` can register/status/rollback the
owner-supplied original binary/job. KeepAlive disabled; pending native clients
can still request relaunch. Stock code currently exits0; no working service claim.
Current shared992a records static AMS image offsets on errors; account remains
saved/active but authentication flag is currently false. Read handoff top.

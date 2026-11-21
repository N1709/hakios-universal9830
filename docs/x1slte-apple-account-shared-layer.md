# Shared native Apple Account layer

Live experimental deployment: 2026-11-17. The native iCloud account is stored,
active; authentication state must be checked after service restarts (latest false). Media & Purchases authentication
remains under investigation. Individual Apple services need their own verification.

## Current architecture

One immutable library contains the working account adapters. Native clients share
the same private machine-data provider through their normal account processes.
The live helper is the user-owned Hackintosh; a pinned IPv6 connection supports
the current hotspot. This deployment requires that helper to remain reachable.

| Component | Function |
| --- | --- |
| anisette-lab-bridge.m | Native Anisette delivery, consistent AuthKit metadata and scoped exact-body iCloud signing |
| icloud-public-session.m | Public ephemeral transport for native AALoginAccountRequest while retaining original request builder, parser and callback |
| icloud-delegate-session.m | Native AALoginDelegatesRequest transport with fresh credential-free machine metadata |
| media-response-probe.m | Passive decoded store status/error summary, preserving original task/result/error |
| media-error-boundary.m | Native NSError code-only creation/description observer, with original results preserved |
| media-error-probe.m | Passive native promise NSError domain/code chains, preserving arguments and BOOL return |
| shared-layer-scope.h | Explicit native process scope |
| shared-layer.py | Install/status/rollback, signed artifact readback and targeted rule updates |

Configured processes: akd, accountsd, appleaccountd, amsaccountsd, appstored,
itunesstored, storeaccountd, Preferences and AppStore. The absent storeaccountd job
was not created; the rule covers it if naturally launched. Current running account
and store jobs were restarted; UI apps load the layer on their next launch.

Shared placement does not establish compatibility with every Apple protocol.
Transport substitution is still scoped to the tested login/delegate request
classes. Other classes retain native handling. Apple account verification,
response parsing, service errors and standard TLS certificate checks remain.

The delegate request's credential-bearing body stays on the phone and goes only
to Apple. The helper's fresh-metadata endpoint accepts no account payload and
returns eight machine fields. Exact-body signing separately enforces the narrow
non-secret device/service schema. Old body signatures are not attached to a
credential-bearing delegate body.

## Deployment and recovery

Current artifact:
`/var/jb/usr/lib/libredwood-account-layer-9b41e2a7df42db8b.dylib`.
The path is immutable. Build/sign into a temporary file and publish a new hashed
filename for every source change; never overwrite a mapped signed dylib on this
port. The build script includes adapter/header/build-script content in its hash.

The installation replaced four old account-adapter rules with one shared rule.
Unrelated graphics, Metal-off, media/camera, passcode and passive diagnostic rules
were preserved. Only the relevant running account/store services were restarted.
No reboot or account reset was performed.

```sh
tools/apple-account/build-shared-layer.sh
python3 tools/apple-account/shared-layer.py status --ssh-config out/apple-account/private/account-hotspot-ssh.conf
```

For a new installation, use the returned artifact with `plan` and then `install`.
An existing active shared layer is refused until its state is reviewed. Installer
state is kept privately in ignored out/apple-account/private/shared-layer-active.json.
It records the replaced rules and artifact digest; it contains no account tokens.

Rollback:

```sh
python3 tools/apple-account/shared-layer.py rollback --ssh-config out/apple-account/private/account-hotspot-ssh.conf
```

Rollback removes the exact installed rule and restores only the replaced account
rules, preserving unrelated changes made after installation. Native services reload
the previous adapters. The saved account and its data are retained.

The library is installed on disk; current exec-injection selection is a runtime
configuration for this boot. Boot-image/launch persistence has not been added.
After reboot, inspect rules before reapplying a configuration. Helper and tunnel
startup still use the task's persistent local sessions, not a new boot agent.

## Verification completed

- Combined library sync/async Anisette and native formatter checks pass.
- Native unsent iCloud request produces an exact-body signature and matched fields.
- Both public login and delegate proxies preserve real local HTTP200 and401
  through native AARequest parser/callback; fixtures supply no account.
- Native validation/alert callback observers preserve success/failure, object
  identities and original completions, including three-argument callbacks.
- Media promise observer preserves result/error identities, duplicate flag and
  native YES/NO return values; the synthetic test performs no authentication.
- Install/rollback rule test preserves concurrent unrelated graphics and Metal
  rules and rejects unexpected process targets.
- Remote installed artifact SHA256 matches the signed local file.
- Live akd, accountsd, amsaccountsd, appstored and itunesstored retain valid code
  signatures, native signing identities and readable selected entitlements.
- AuthKit read-only health passes after shared deployment. Native account remains
  count1, authenticated=true, active=true, warmingUp=false.

## Current next test

User opens Media & Purchases and continues once with secrets entered only on the
phone. Read media-error-events.log for native NSError codes and network-events.log
for service status. Initial HTTP200 to auth.itunes.apple.com does not establish
application acceptance. Do not infer password mistakes from a generic UI failure.

Private configs, SSH keys, machine proofs, credentials and responses stay excluded
from source. Diagnostics emit fixed code/presence metadata. The old comparison
that forwarded a fresh PET is disarmed and is not part of the shared library.

## Native Media signing assistance (trial pending)

The shared layer now includes media-signing-bridge.m for native Mescal sign/prime/verify operations through the user-owned Mac. native-media-provider.m calls native Apple signing and verification; no licensing/verification success stubs. Signatures and invalid-signature rejection passed synthetic end-to-end tests. AppStore login/download remains unverified.

Enable broker with --media-helper native-media-provider-v1. Media signing bytes can contain credential values selected by Apple's policy; owner authorized this private transfer. Only bounded bytes cross pinned SSH in memory; no raw data/proof logging or disk capture. The existing iCloud schema remains separately restricted to device/service metadata. Helper is mandatory for this experimental route.

The Settings MacBook device label comes from the provider's borrowed Mac identity. Changing only its label/model does not create a coherent iPhone identity.

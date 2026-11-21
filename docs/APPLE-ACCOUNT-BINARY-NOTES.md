# Native account authentication findings

Historical analysis below. Current 2026-11-17 native account is authenticated/active
with warm-up complete; Media & Purchases remains under investigation. See
[the current handoff](APPLE-ACCOUNT-HANDOVER.md) for live evidence and runtime.

2026-11-16. Static iOS16.5/20F66 analysis plus live metadata. No successful login.

## What the outgoing credential actually is

The native akd method
`_shouldContinueAuthenticatingForUsername:password:serverResponse:didShowServerUI:continuationData:error:context:completion:`
at `0x10004180c` calls server response `passwordEquivalentToken` at
`0x100041a7c`, calls its `stringValue`, and stores it under
`AKAuthenticationPasswordKey` at `0x100041ab0..0x100041ac0`.

The `passwordEquivalentToken` getter at `0x10000e260` retrieves the service
identified by the static CFString at `0x100120058`. Its character data at
`0x10010fbf0` is `com.apple.gs.idms.pet`. The raw password is handled separately
under `AKAuthenticationRawPasswordKey` around `0x100041d90`.

Therefore live equality between the iCloud Basic authorization and the AuthKit
username/password-key result means the PET handoff matches. It does not prove
server acceptance, token audience, expiry or account context. Safari tests a
separate authentication path.

## Native iCloud provisioning header names

Shared-cache AppleAccount
`AADeviceProvisioningSession addProvisioningInfoToURLRequest:sendEmptyValues:`
at `0x19ed963d4` reads `mid_data` and `otp_data` from `deviceProvisioningInfo`.
Header constants referenced through `0x1d762aad8` and `0x1d762aae0` resolve to
`X-Apple-MD-M` and `X-Apple-MD`. These differ from AuthKit's I-MD names.

Bridge14 now supplies consistent legacy and AuthKit pairs. Real Settings7895 /
akd7894 capture proves both legacy values present and matching their aliases,
selected machine fields matching provider, body-bound signature present, no
signing-error, and AuthKit/PET authorization equality. iCloud still returns401.
Thus aligning the names did not suffice.

The same native provisioning class calls ADIOTPRequest and handles an
account-not-provisioned condition. A standalone native Hackintosh comparison can
produce generic DSID-2 proofs, but no scoped proof for its signed-in numeric DSID.
This CLI result does not establish the daemon's provisioning state. No account
reprovisioning, erasure or sign-out has been attempted.

## Response interpretation

AALoginAccountResponse `_parseResponse:responseBody:` at `0x19ed76870` immediately
returns for status other than200. Its success-body fields include status,
status-message, delegates and dsid. The current live401 body is a plist with no
recognized numeric detail under the bounded passive parser. Neither the HTTP401
nor the UI's generic password message proves raw password entry was wrong.

## Reproducible binary tool

`tools/apple-account/annotate-objc-stubs.py` reads a thin arm64 Mach-O, existing
`dyld_info -fixups`, `otool -tvV` and optional `otool -ov` outputs. It maps selector
rebases through Objective-C message stubs, including tail branches, and names
method entries. It annotated9187 calls in the supplied akd binary. Assertions
verified the known PET call and method entry. It operates only on static files.

The original binary, firmware-derived disassembly, fixups and annotated output
remain ignored local artifacts. Do not commit or bundle them.

## Successful native Mac comparison and static credential mapping

User completed native macOS re-login with SMS verification, Mac password,
calendar merge and Find My prompts; account fully signed in. Sanitized collector
observed a new PET, raw-password result selection, password-auth selection and
AALoginAccountRequest in that order near16:02:18UTC. Unrelated401/500 occurred
before that login sequence; without request correlation do not assign those
statuses to the successful iCloud request.

Exported only the local diagnostic process's loaded AppleAccount static text,
selector slots and fixed known AuthKit-key import names for x86 disassembly.
No protected daemon attachment, heap/account/token read, or security-setting
change. Static exports remain ignored firmware-derived artifacts.

Mac text-relative offsets:

- `_onqueue_updateAppleAccount:withAuthenticationResults:`0x74062 reads
  AKAuthenticationPasswordKey at0x7409e, retains it inr15 and passes it to
  credentialWithPassword at0x74130..0x74142, then setCredential.
- Separately reads RawPasswordKey at0x740ca and passes that value via r12 to
  `_aa_setRawPassword:` at0x74161..0x74172.
- `ACAccount aa_password`0x97f84 calls credential then password; it does not
  select the separate raw-password property.

Thus the observed native Mac path also uses the AuthKit password-key/PET
credential. Raw-password substitution is not justified by its log message.

## Native request User-Agent preservation trial

The native Mac nil-account prototype has client-info but NO explicit User-Agent;
previous false equality with AADeviceInfo was absence, not a different explicit
header. Same absence holds for the phone prototype before overlay. Bridge14
forced the nominal AADeviceInfo User-Agent; bridge15 removes only that iCloud
override, preserving native request/session selection. This is a concrete
compatibility correction, not proof it explains401. Body, Authorization,
provisioning pairs and body-bound signature are preserved.

Synthetic preservation test and phone unsent request/signing/AuthKit smoke pass.
Mac provider-v5 and broker unchanged; fresh machine proofs remain available
after native Mac re-login. Only Settings loads bridge15; akd stays bridge14 to
avoid unnecessary daemon restart. Real acceptance pending.

# Historical Apple Account research notes

Superseded by [APPLE-ACCOUNT-HANDOVER.md](APPLE-ACCOUNT-HANDOVER.md).
Older runtime/version statements below are historical, not current instructions.

# S20 Apple Account research handover

## Current: v13 still401; passive v5 detailed error capture ready

- User reports same error after service metadata correction. Real pid7735:
  AuthKit completion no error, final iCloud login HTTP401; validation header1,
  no signing-error header, all selected machine comparisons match native cloud
  provider. V13 did NOT resolve remaining rejection. Sanitized evidence saved
  v13-real-401-events.json. No completed native iCloud account demonstrated.
- New account-free native-icloud-contract-probe.m compares native Mac and iOS
  AALoginAccountRequest: both use /setup/signin/v2/login and same root/user keys.
  Thus merely calling this an iOS-only endpoint is unsupported. Both native
  AARequest user agents also differ from AADeviceInfo getter; no new UA change
  made. Client-id is separate from machine UDID; no body rewrite attempted.
  Mac probe requires IntlPreferences dlopen to supply NSLocale category; first
  isolated probe without it threw unrecognized selector, then fixed probe passed.
  No host services/accounts changed, no network requests sent by these probes.
- Added passive network-error-probe-v5: on FAILED AALoginAccountRequest only,
  <=64KB response plist/JSON yields fixed allowlisted numeric code fields and
  fixed message category (no raw text, unknown names, token/account values).
  Successful bodies are NOT inspected. Original response and completion preserved.
  Does not change authentication. Tests demonstrate token/account/text exclusion,
  success-response ignore, native NSError preservation and loopback HTTP behavior.
- LIVE bridge v13 unchanged; diagnostic now v5 Preferences akd accountsd
  appleaccountd. Unrelated rules preserved; Settings closed, affected native
  daemons restarted. Native helper-v5/broker27833/tunnel3471 unchanged.
- Next user-entered attempt is DIAGNOSTIC, not claimed fix: inspect new
  account_failed_response_summary. Earlier response bodies were never retained,
  cannot recover detailed code from old capture. Do not repeat header tweaks
  without new evidence or blame password based on generic HTTP401.


## Current v13: native iCloud service metadata; real retry pending

- V12 user says same error. Real pid7704 has AuthKit completion without errors;
  actual final iCloud request has validation_header_present1, native signing error
  header0, all selected machine headers match. Still HTTP401 from login account.
  Thus missing signature/header delivery does NOT explain remaining rejection.
  Saved sanitized v12-real-proof-401.json. No repeated user retries requested while
  researching; do not infer bad password from this generic error.
- Native Hackintosh --check comparison found AADeviceInfo.clientInfoHeader and
  userAgentHeader are BOTH available and DIFFER from the AuthKit generic values
  the adapter reused for iCloud. Native AADeviceInfo.appleIDClientIdentifier also
  differs from machine device-ID (normal different concepts; body unchanged).
- Provider-v5 uses native AADeviceInfo service-specific client info/User-Agent
  ONLY for scoped iCloud signing requests. Generic GSA remains native AuthKit.
  No fabricated OS/bundle version or Windows profile introduced. No credentials
  forwarded and exact-body/signing whitelist unchanged.
- V13 keeps cachedCloudHeaders separate from cachedHeaders/cachedAt. Scoped iCloud
  fetch no longer overwrites generic AuthKit metadata; iCloud diagnostics compare
  against cloud snapshot. Credentials, native body and server verification untouched.
- Native unsent three-service request passes: service_specific_metadata_separated1,
  matching cloud header comparisons, proof present and signing-error header absent.
  Generic sync/async AuthKit smoke passes too. No account login by tools.
- LIVE v13 bridge Preferences akd; v4 passive probe, SRP probe, Settings-only
  Metal-off and all unrelated rules preserved. Settings closed, akd restarted.
  Helper-v5 uploaded. Broker exec27833 on49741, reverse tunnel3471/phone49742.
- Next: one user-entered trial on phone with both Macs awake. Read actual final
  result and selected header/proof flags. Still no demonstrated iCloud sign-in.
  Do not claim service-header mismatch proven root cause until real success.


## Current v12: explicit request header delivery; real trial pending

- User reports wrong-username/password dialog and confirms TWO v11 attempts.
  Trace pid7675 confirms both: AuthKit completion without errors and required
  result keys present (values not read); native_request_metadata_signed occurs
  for both; final AALoginAccountRequest HTTP401/com.apple.appleaccount401.
  Three-service real schema is now accepted. This does not prove bad password.
- Task.currentRequest validation header absent despite native signature callback.
  Supplemental/session headers may not be reflected in currentRequest; absence
  does NOT alone establish proof absent on wire. No broad packet/TLS capture used.
- V12 cloudRequest obtains exact-body signature under the SAME strict schema and
  attaches it directly to NSURLRequest along with matching machine metadata.
  Original account auth/body unchanged. Native supplemental signing hook retained.
  Unknown bodies still never forwarded. This removes supplemental-header delivery
  ambiguity; server acceptance still unproven. No login success/auth bypass.
- Account-free native three-service fixture sent via real NSURLSession to a
  temporary private LOOPBACK receiver: received proof header, strict allowed body,
  no Authorization or signing-error header. No Apple account request sent. Native
  full signing context also passes. Receiver and its extra tunnel removed after
  test. Report native-signing-v12-loopback.json (only booleans/schema names).
- LIVE bridge v12 Preferences akd, passive v4, existing SRP and Settings Metal-off
  preserved. Settings closed and akd restarted. Helper-v4/broker23793/tunnel3471
  unchanged. User opens Settings normally for one final-placement trial; check
  actual validation_header_present=true and final server result. If still401,
  do not call credentials wrong or claim full sign-in works; investigate remaining
  iCloud handoff/device compatibility. No password reset or tool credential use.
- Sanitized two-attempt evidence v11-two-attempt-events.json in ignored private out.


## Current v11: third service descriptor allowed; real retry pending

- V10 real attempt: AuthKit completion no error, iCloud login HTTP400. Fixed-label
  shape checks: callback supported; root/protocol/user schema/UUID/language/timezone
  all valid; delegate count3, unknown delegate count1, existing mobileme/IDS schemas
  valid. Therefore refusal was the two-service limit/additional service, not secrets
  detected in root/user or callback ABI. Unknown third name/values were not logged.
- Static installed GameCenterAppleIDAuthenticationDelegate declares service
  com.apple.gamecenter; it has delegateServiceIdentifier/response processing only,
  no extra request-parameter method. Added ONLY com.apple.gamecenter with EMPTY
  dictionary and raised max descriptors to3. Real third service is inferred from
  installed plugin, not directly observed; do not claim confirmed until real path
  logs native_request_metadata_signed. Unknown/nonempty Game Center data refused.
- Validators updated in phone header and Python broker; native helper-v4 compiles
  same header. XML/binary synthetic three-service acceptance and token/unknown
  service rejection pass. Native account-free three-service AALoginAccountRequest
  full signing test passes through real helper: signature present, no error header,
  no account supplied, no request sent. Artifact native-signing-v11-three-delegates.json.
- LIVE bridge now anisette-lab-bridge-v11.dylib Preferences akd; passive v4 probe,
  Settings-only Metal-off, SRP probe/unrelated rules preserved. Settings closed,
  akd restarted. Native helper-v4 uploaded; broker restarted exec23793 on49741;
  tunnel3471/phone49742 unchanged. No reboot/account/cache reset/credentials used.
- Next: user opens Settings normally and performs ONE real test. Inspect actual
  native_request_metadata_signed and HTTP/error before claiming progress/success.
  If shape still refused, do not weaken credential guard or dump unknown content.
- Sanitized v10 structural evidence saved v10-shape-events.txt in ignored private out.


## Latest v9 real trial: iCloud400, schema guard refused; v10 diagnostic ready

- User reports could-not-communicate popup. Actual v9 Settings pid7622:
  authentication_completion error_present0, result_available1, expected username/
  password/DSID/alternateDSID keys present (presence only, values never read).
  AALoginAccountRequest HTTP400/com.apple.appleaccount400; native signing header
  absent; selected machine identity headers all match native provider.
- native_signing_metadata_not_forwarded confirms strict signing guard refused
  real request or callback shape. It did NOT forward unknown data to helper.
  Unsent empty-account fixture success does not establish live schema compatibility.
- Built/tested/deployed anisette-lab-bridge-v10.dylib Preferences akd, replacing
  only v9 rule. Signing guard and broker/helper unchanged. V10 emits fixed-label
  booleans/counts for root/user/delegate grammar and callback ABI when refused.
  No unknown field names, values, body, passwords or tokens emitted. Native
  unsent signing fixture still passes, no account operation performed by tools.
- Settings closed, akd restarted; next user opens Settings normally and performs
  ONE diagnostic attempt to identify exact structural guard failure. Leave popup
  open. Read signing_* events, preserve strict guard; no blind claim of fix.
- Sanitized real v9 evidence: ignored out/apple-account/private/
  v9-correct-account-events.json. Current passive probe remains v4.


## Current state: v9 native metadata signing correction deployed

This section supersedes older runtime/trial descriptions below (historical).
No successful native iCloud sign-in has been demonstrated.

- User confirms fresh web sign-in works. At explicit request, compared the
  supplied identifier with Hackintosh MobileMeAccounts preferences and S20 IDMS
  usernames: matches present on both. Only counts/booleans retained; no password
  used or checked by tools. S20 AppleAccount count remains zero, IDMS count two.
- Correct-account v8 trial: GSA SRP success and AuthKit completion without error,
  followed by AALoginAccountRequest HTTP401. This does not establish bad password.
- Native disassembly established AAURLSessionContext signs aa_HTTPBody, while v8
  inserted an empty-data signature before native signing. Comparative unsent,
  account-free full-context fixture: v8 native NAS absent/error header present;
  v9 native NAS present/error header absent. Neither fixture sent login requests.
- v9 hooks _additionalAbsintheHeadersForData:completion: and signs exact metadata
  bytes through native Hackintosh AAAbsintheSigner. Phone, broker and helper each
  enforce a narrow plist schema (device UUID, language, timezone, fixed delegate
  names/protocol versions); unknown/credential/token fields are rejected. Exact
  SHA256 binding verified in memory. No arbitrary signing endpoint, credential
  forwarding, account authentication bypass or request-body logging.
- Updated helper is ~/.local/share/redwood-account-lab/hackintosh-provider-v3.
  Native loopback broker49741 runs updated native-provider-server.py (exec56114);
  Wi-Fi reverse tunnel (exec3471) exposes phone loopback49742. Tokens/config private.
- LIVE injection now anisette-lab-bridge-v9.dylib Preferences akd, and passive
  network-error-probe-v4.dylib Preferences akd accountsd appleaccountd. V4 records
  signing-error header presence and AuthKit result-key presence, never values.
  Settings-only Metal-off and all unrelated injection rules preserved. akd,
  accountsd and appleaccountd restarted; Settings opened normally (no deep link).
- Full native signing fixture, sync/async bridge smoke and passive probe error
  preservation smoke passed. Invalid credential/token schema rejection passed.
- Next: one user-entered phone sign-in with both Macs awake; leave error open.
  Inspect native_request_metadata_signed and actual final HTTP/error outcome;
  do not infer success from intermediate GSA completion. If schema refuses actual
  metadata, preserve privacy guard; do not dump body or broaden to account secrets.
- Current phone owner codex-apple-account, transferred with explicit user consent.
  No reboot, account removal, keychain reset or Hackintosh sign-out performed.


Updated: 2026-11-16. No successful native account sign-in has been demonstrated.

## Latest user correction: native-provider retry used the wrong account

- V8 trial reached AuthKit completion without reported error, then native
  AALoginAccountRequest HTTP401 / com.apple.appleaccount:401. On the actual
  completed request, validation_header_present=true and every selected machine
  identity comparison matched the Hackintosh provider. AppleAccount count0,
  idms count2. The added IDMS account's meaning was not inferred or inspected.
- User then explicitly said the latest attempt used the wrong account and
  requested another attempt. Therefore this401 is not a valid failure test of
  the intended working secondary account. No credential-hand-off diagnosis is
  established. Earlier HTTP400 captures are unchanged evidence.
- No additional adapter change. Native helper freshness/nonce/validation checked
  privately; Settings restarted normally for a fresh page. Native provider port,
  v8 bridge, v3 diagnostics and Settings-only Metal-off verified. No account
  removal, token/keychain/cache reset, host account change or tool-driven login.
- User asked to use the working Hackintosh secondary account with secrets only
  on phone. Correct-account result is pending; keep both computers awake.

## CURRENT: native Hackintosh provider / v8 trial pending

- User added the dedicated SSH public key. Successfully connected as
  user@hackintosh.example using the task key and pinned private known-host file.
  Hackintosh is Intel x86_64 macOS15.8.1 build24H32. No administrator permission
  needed for the probes/helper. Isolated binaries under
  ~/.local/share/redwood-account-lab/. No account/keychain/database reads,
  account sign-out, host security changes or reboot.
- Direct availability comparison: Hackintosh AOSUtilities supplies nonempty
  native MID/OTP; its AAAbsintheSigner supplies nonempty NSData signatures for
  both public fixture bytes and empty data, no error. S20 standalone probe had
  no signature/-4403. This establishes an available helper source, not that
  missing validation caused the live400. Native AOS/AuthKit/AppleAccount device
  ID getters agree on Hackintosh. Controller existing-data read still gets
  NSCocoa4099 (client permission); machine generation works through AOSKit.
- New hackintosh-provider.m: --check prints only availability. --stdio is a
  persistent **private SSH RPC**; its replies contain machine proof material.
  NEVER run it in a tool that prints replies. Broker consumes it in memory.
  Accepts exactly version+nonce, never account credentials or arbitrary signing
  input. Uses native machine IDs/serial/client info/local-user ID, native AOS
  machine proof, and an AAAbsintheSigner signature over empty data; nothing
  is read from account/keychain stores or donor iPhone. Proof body-binding/server
  acceptance is not established by the unsent tests.
- Explicit limitation: routing hint84215040 follows AltServer's native-Mac
  implementation, not a retrieved routing value. Other selected identity values
  are native. Native User-Agent/OS/framework versions are used instead of old
  hardcoded emulated-Mac version strings. No profile values/proofs were printed
  or persisted. Sources/presence reports live in task tree/ignored out.
- native-provider-server.py is loopback-only127.0.0.1:49741, bearer-token guarded,
  persistent pinned SSH to Hackintosh helper-v2. Rejects extra/credential fields;
  verifies nonce/freshness/schema, closes stale timed-out transport. SSH helper
  stderr discarded, no proof logging. Native broker exec session **42902**.
- Reverse Wi-Fi SSH tunnel exec session **3471** forwards phone127.0.0.1:49742
  to Mac127.0.0.1:49741. Both endpoints private; Hackintosh has no added TCP
  server/launch agent. Existing token reused. Phone connection.json now port49742;
  prior private config saved as connection-before-hackintosh.json (secret token:
  do not print). Old emulated broker/tunnel remain idle standby; no requests mix
  sources in currently configured Settings/akd after their restart.
- V8 bridge validates optional X-Mme-Nas-Qualify/User-Agent fields and overlays
  validation only on HTTPS setup.icloud.com AALoginAccountRequest. Generic GSA
  formatter omits NAS-Qualify. Native AuthKit CPD metadata stays aligned. Original
  Apple credentials/verification and native body remain untouched. Earlier
  cloud-getter fallback to original request on helper loss still applies.
- Tests pass: helper availability, broker unauthorized/extra-field refusal,
  fresh private nonce-bound response; updated synthetic native-input preservation
  tests; on-phone sync/async/HTTP formatter; **unsent** account-free native iCloud
  fixture includes validation and matching identity; empty-context daemon CPD
  matches all selected identity fields and client description. One-shot daemon
  construction probe was removed after testing. No account login by tools.
- CURRENT account rule: /var/tmp/anisette-lab-bridge-v8.dylib Preferences akd.
  CURRENT passive probe: /var/tmp/network-error-probe-v3.dylib Preferences akd
  accountsd appleaccountd. v3 additionally records validation-header presence
  and selected machine identity equality booleans at task completion; no
  credential headers/body contents. SRP probe and Settings-only metaloff remain.
  akd7491 last started; Settings reopened normally. Preserve unrelated rules.
- User asked for ONE secondary-account trial, keeping Hackintosh and this Mac
  awake. Outcome pending. Read network-events.log after reply; verify both actual
  task validation_header_present and account result. Native iCloud/App Store/
  iMessage functionality is not yet verified. No more blind password retries.
- Rollback current native setup: restore lab connection config only from owned
  private backup, remove/revert only owned bridge/probe rules preserving others,
  restart affected processes to unload. Stop only owned native broker42902 and
  tunnel3471. Keep emulator state private. Hackintosh helper exits on SSH EOF;
  no permanent service was installed. Dedicated SSH authorization can be removed
  later by its codex-apple-account-lab comment if user wants it revoked.

## Latest: V7 still fails; working Hackintosh comparison being connected

- V7 trial again produced AALoginAccountRequest HTTP400 / native
  com.apple.appleaccount:400, with icloud_identity_headers_aligned emitted.
  The header correction is insufficient. Native authentication still completes
  without a reported error. User was asked to pause further retries.
- Unsent account-free fixture identifies endpoint /setup/signin/v2/login.
  Native fixture userInfo.client-id is a nonzero36-character UUID. No live body
  or credential values were inspected/retained. Native signing test for public
  fixture bytes returned no signature, com.apple.AppleAccount.Error:-4403;
  meaning/cause of this signing error is unestablished.
- Protocol research distinguishes Anisette from separate NAC/validation data
  used by newer sign-in paths. Missing NAC is a hypothesis for live400, not
  proven by the nil-account fixture or standalone signing failure.
  Primary references: OpenTagViewer docs/findmy-export/02-mobileme-delegate.md
  and pypush revision6336bba82697b838497ffd774f23e5640c877a85/emulated/nac.py.
- Added nac-provider-probe.py, hash-pinned old pypush emulator + generic Apple
  IMDAppleServices binary. Copies no donor profile: derives synthetic data from
  existing lab identity plus generic model/board descriptors. Default offline
  setup succeeds, network0/account operations0. One explicit credential-free
  online test fetched Apple's public certificate over verified HTTPS, then
  bounded emulated initialization timed out before validation handshake POST.
  No NAC proof generated. Do not claim this helper works. Attempt marker at
  private nac-runtime/online-attempt.json prevents silent retries. All upstream
  network functions are disabled; wrapper owns HTTPS transport. Do not run
  upstream generatenac.py (prints proof) or reuse upstream data.plist identity.
- User confirms an actual working Hackintosh, signed in natively with the same
  secondary Apple Account. User explicitly authorizes comparison and requests
  packet/service diagnostics. No packet sniffer was found on S20; no TLS
  interception or broad packet capture has been performed. Existing metadata
  capture already establishes serverHTTP400 rather than transport timeout.
- Hackintosh access supplied: user@hackintosh.example. SSH reachable. First-contact
  host key saved via accept-new into private hackintosh-known-hosts, changed keys
  remain refused. Batch login rejected for missing authorization. No password
  requested or used.
- Generated dedicated key at ignored out/apple-account/private/hackintosh_ed25519
  (public key .pub, comment codex-apple-account-lab). User will be given commands
  to append public key to Hackintosh ~/.ssh/authorized_keys as jesvi. Await “done”.
  Next: use -i dedicated key, IdentitiesOnly=yes, StrictHostKeyChecking=yes and
  private known-hosts path. Start with read-only macOS version/architecture/tool
  checks; prepare same metadata/signature-presence probes, no keychain/token or
  account DB extraction. No host account sign-out, security change or reboot.

## Current trial: v7 targeted iCloud login request headers

- Second captured retry identified native `AALoginAccountRequest`, HTTP400,
  native `AALoginAccountResponse.error = com.apple.appleaccount:400`. Native
  SRP result again returned success/no error and AuthKit completion no error.
  Follow-up gsas HTTP401 also exists; full account sign-in remains unverified.
- Static native code at 0x19edcf5d8 proves AALoginAccountRequest.urlRequest builds
  its own Device-UDID/device/provisioning headers outside AKAppleIDSession's
  generic formatter. Native AARequest.bodyDictionary at0x19ed7c678 builds
  protocolVersion/userInfo; userInfo.client-id is obtained independently from
  AADeviceInfo.appleIDClientIdentifier. The role of that client-id is unverified;
  it was deliberately not changed.
- Added `icloud-request-probe.m`: constructs AALoginAccountRequest with **nil
  account** and empty delegates, never sends it or saves an account. It uses
  only laboratory machine metadata. The only body decoding is this account-free
  fixture; live authentication bodies are not decoded or collected. Output is
  equality booleans and fixed fixture key names, never values/credentials.
- V6 fixture demonstrates distinct native identity headers not matching the
  virtual provider. Missing provisioning headers in this nil-account fixture
  are NOT proof they were missing in the real account request.
- V7 hooks **only** AALoginAccountRequest.urlRequest for HTTPS setup.icloud.com.
  New `icloud-request-identity.h` copies the native request and overlays the eight
  validated provider headers plus Device-UDID matching virtual device ID. It
  preserves URL/method, native body, Authorization and unrelated input. No body
  rewrite, token extraction, success override or new account verification path.
  If metadata cannot be obtained, logs icloud_identity_unavailable and returns
  the original native request; do not describe this fallback as fail-closed.
- `test-icloud-request-identity.m` synthetic fixtures pass scope/metadata
  validation, immutability and preservation of opaque authorization/body/method.
  On-phone unsent native fixture with V7 confirms all selected metadata match
  provider, Device-UDID agrees with X-Mme-Device-Id; no account supplied, no send.
  Native sync/async/HTTP formatter smoke tests also pass.
- CURRENT account rule:
  `/var/tmp/anisette-lab-bridge-v7.dylib Preferences akd` (replacesv6). akd7353
  and Preferences7359 restarted. Preferences emitted icloud_request_adapter_installed.
  Metal-off Settings and v2 passive network/error probes remain as before.
  Preserved prior rules in ignored injection-before-v7.txt and diagnostic events
  in login-account-http400-events.jsonl. Provider state/token unchanged.
- Asked user for one trial with this concrete header change; awaiting result.
  Do not claim iCloud success until result/account state verified. No genuine
  iPhone access, account database copy, reset/reprovisioning, reboot or permanent
  system/graphics/passcode change.

## Most recent captured failure: iCloud setup HTTP 400

- User completed the first instrumented retry and reported “Error open”. Capture
  shows native SRP result `success:true` with no CFError, AuthKit client completion
  with `error_present:false`, then setup.icloud.com HTTP400 in Preferences PID7248.
  This narrows the observed failure beyond the original MID/password stage; it
  does not prove completed iCloud registration. Follow-up gsas.apple.com HTTP401
  was also observed from old akd PID7239; do not assume it is unrelated or causal.
- Metadata-only snapshot preserved privately as `icloud-http400-events.jsonl`.
  NSURLError -999 entries were cancellations; do not label them timeouts.
- Enhanced `network-error-probe.m` v2 adds native **AARequest class name** at
  completion and native **AAResponse NSError domain/code** when its getter is
  called. No request or response contents are inspected/printed/persisted.
  Synthetic native AAResponse error propagation plus loopback tests pass.
- CURRENT diagnostic rule is
  `/var/tmp/network-error-probe-v2.dylib Preferences akd accountsd appleaccountd`,
  replacing v1. Account services restarted; Settings reopened normally.
  Install markers: accountsd7282, appleaccountd7285, akd7288, Preferences7294.
  Preferences/native AppleAccount completion+error hooks both report installed.
- User asked for one more **diagnostic** attempt to identify request class and
  native error behind400. No registration fix was claimed; await this second
  result before another retry. Working Settings-only Metal-off and v6 identity
  bridge unchanged. No real account authentication performed by tools.
- Metadata helper `--appleaccount` lists method metadata only; `--addresses`
  derives unslid method addresses from loaded Mach-O headers without accessing
  accounts. Output saved privately as `appleaccount-method-addresses.txt`.
  Relevant cache addresses: AALoginAccountRequest.urlRequest 0x19edcf5d8,
  AALoginDelegatesRequest.urlRequest 0x19ed7ecd0, AARegisterRequest.urlRequest
  0x19ed7d930, AASetupAssistantSetupDelegatesRequest.urlRequest 0x19ed73d44,
  AARequest._handleDataTaskCompletionWithData:response:error: 0x19ed69a30.
  Local read-only disassembly retained under private out; no body captures.

## Latest continuation: readable UI, registration error capture armed

- User confirmed rendering is fine with Settings-only Metal-off. Screenshot
  IMG_0238.PNG independently confirms exact text “Could Not Sign In / Could not
  communicate with the server.” IMG_0239.PNG shows “Signing in to iCloud…”. These
  do not establish successful authentication or completed account registration.
- USB SSH timed out; Wi-Fi SSH at 192.0.2.10 with HostKeyAlias=10.77.0.2
  works. Broker port 49731 is still listening. Use the verified Wi-Fi route.
- Existing SRP probe log contained only its installation marker: no error from
  the previous user attempt was captured. Do not infer an error code from it.
- Native account/auth/push/CloudKit/security daemons are present and running.
  Count-only probe still reports AppleAccount=0, idms=1.
- Added `network-task-metadata.m`, which constructs but never resumes a task and
  inspects method metadata. Confirmed private CFNetwork completion ABI and native
  AuthKit client authentication-completion ABI.
- `network-error-probe.m` now records **only** Apple/iCloud hostname, HTTP status,
  bounded NSError domain/code chain and PID at task completion; AuthKit completion
  records only error presence/domain/codes. No headers, request/response bodies,
  authentication result contents or account fields are inspected/logged. Requests
  and completions are passed unchanged to original methods.
- `network-probe-smoke.m` passed on-phone: loopback HTTP501 and connection-refused
  -1004 were recorded while preserving normal completions. No account operation
  or Apple request was made by the smoke test.
- CURRENT RULES retain v6 bridge, Settings-only Metal-off and SRP diagnostic;
  added `/var/tmp/network-error-probe.dylib Preferences akd accountsd appleaccountd`.
  Removed the one-shot rendering probe rule. Restarted these user daemons and
  relaunched Settings normally. Original unrelated rules preserved; baseline at
  `out/apple-account/private/injection-before-network-probe.txt`.
- Installed markers seen for accountsd PID7232, appleaccountd PID7236, akd PID7239.
  Background gsa.apple.com completion returned HTTP200 without a transport error;
  this establishes connectivity to that request, not account-login success.
- User asked for exactly one sign-in attempt with diagnostics active, leave the
  error open and report “error open”. Await result. Do not repeatedly request
  retries. Read private `network-events.log` and `request-events.log` via Wi-Fi.
  Account bridge and provider identity unchanged; no resets/reprovisioning,
  stored account removal, system image changes or reboot.

## Latest: v6 reaches verification UI; Settings text corruption

- User reports reaching the verification-code UI and entering a code, followed
  by two unreadable dialogs. Inspected latest screenshot IMG_0235.PNG privately:
  several labels are rendered as horizontal glyph noise while main title/body,
  icons and other labels are readable. This establishes rendering corruption,
  not its precise shader/texture cause.
- M2M confirms Preferences PID6693 used Metal; no GPU fault was found in that
  bounded capture. Closing/reopening Settings restored text for the user.
- User retried independently and could read “Could not sign in” / “could not
  communicate with the server.” Exact full wording remains user-reported.
  No successful iCloud sign-in claimed.
- Read-only `account-presence.m` uses Settings' existing account permissions and
  reports **counts only**: AppleAccount=0, idms=1. This does not prove a valid
  signed-in session, token validity or iCloud registration. No credential/name/
  identifier access and no account DB copies.
- Temporarily prepended `/var/jb/usr/lib/libredwood-metaloff.dylib Preferences`
  to existing injection rules and restarted Settings. Only Settings' Metal
  discovery is hidden; no systemwide graphics policy/driver/image changed.
  Baseline preserved privately as `injection-before-settings-rendering.txt`.
- New one-shot `settings-render-probe.m` presents a harmless display-test alert
  on Settings activation. V2 probe reports `metal_available:0`; user readability
  confirmation pending. Current temporary rule:
  `/var/tmp/settings-render-probe-v2.dylib Preferences`. Remove it and restart
  Settings after check. Do not use blind authentication retries to test graphics.
- Native USB `shot` read the boot/log buffer instead of the current app on this
  image; do not interpret its black/log capture as the actual Settings display.
- `srp-error-probe.m` logs only Apple hostname, BOOL result, and bounded NSError
  domain/code chains at the native SRP transport boundary. It never reads the
  request context, headers/body or response payload. Installed and ready in akd
  using `/var/tmp/srp-error-probe.dylib akd`; private file `request-events.log`.
  No error from the user's previous attempt was captured retrospectively.
- Active account adapter remains v6. Broker/tunnel sessions unchanged. Await
  display-check confirmation before requesting a single diagnostic sign-in.
  Remove these diagnostic rules in addition to account bridge when rolling back;
  preserve all unrelated rules. No reboot performed or authorized.

## Continuation at 18:03–18:10 IST

- User confirmed password rotation and a successful **fresh** web sign-in. The
  user explicitly replaced the previous codex-gps session and authorized taking
  the phone lock. Current owner is `codex-apple-account`.
- The previous host broker was no longer listening. Restarted the same private
  state/token without reprovisioning. Current tool sessions: broker **42269**,
  Wi-Fi reverse tunnel **86624**, same loopback ports and verified host key.
- New offline `audit-native-payload.py` pins akd SHA-256
  `a99db1b0090cab40900a3ad2576379969ffd2e2c75b2b333833367cc4efb8473`.
  Static code confirms `AKSRPContextHelper.authenticationParametersWithCompletion:`
  creates CPD independently of `AKAppleIDSession`'s HTTP formatter.
- New `native-payload-probe.m` constructs parameters from an **empty** fresh
  authentication context inside akd. It calls no login/request execution API,
  supplies no username/password/code, and records booleans only. Baseline with
  v5-audit: CPD machine ID/routing matched provider, CPD device ID/serial did not,
  and provider local-user ID was absent. This is a demonstrated adapter gap,
  not proof that it caused the server's account/password error.
- V6 adds a guarded native parameter-completion adapter using
  `payload-identity.h`. It preserves the native proof and unrelated envelope
  entries, aligns CPD/additional device ID/local-user ID/serial and the top-level
  client description, and removes unsupported MLB/ROM metadata. It refuses
  missing/unknown layouts and machine-ID/routing mismatches; no success stub.
- Synthetic fixture test passed, as did on-phone sync/async/native HTTP formatter
  smoke tests. Empty-context native probe with v6 passed: CPD machine ID, routing,
  device ID, local-user ID and serial match provider; top-level client description
  matches. Boolean report saved privately as `native-payload-v6-report.json`.
- **CURRENT INJECTION:** original unrelated rules preserved, account rule now
  `/var/tmp/anisette-lab-bridge-v6.dylib Preferences akd`. One-shot payload probe
  rule removed; akd restarted and Settings relaunched normally. Initial probe
  baseline rules saved privately in `injection-before-payload-audit.txt`.
- User has been asked for one sign-in attempt with v6. Await the outcome; do not
  ask for another retry without reviewing it. No account login was performed by
  tools. Genuine iPhone remains untouched. No reboot or permanent system change.
- Remaining gaps: requested DSID is still ignored by the original daemon adapter;
  other native device/model metadata and post-authentication stages are not fully
  validated. A passed construction probe is not server acceptance.

## Workspace and scope

- Worktree: `~/ios-port-research/work/hackios-apple-account`
- Branch: `feat/s20-apple-account`, based on accepted graphics commit `9e0d7cbe`.
- Research documentation and tools are currently untracked/uncommitted. Review `git status` before integrating; do not overwrite unrelated work.
- Detailed design and probe instructions: [x1slte-apple-account.md](x1slte-apple-account.md).
- Original task: explore private Mac-assisted authentication for native Settings on the S20 port. Weather investigation was stopped at the user's request; account work is not a verified Weather fix.
- No kernel, boot image, passcode chain, permanent graphics configuration or genuine iPhone changes were made by this experiment.

## Latest user-visible result

1. Original native Settings sign-in failed with “Verification Failed — MID is invalid.” Screenshot wording was checked privately; temporary local screenshot copies were removed.
2. Initial client-only bridge still produced invalid MID.
3. Corrected daemon-side bridge allowed an unmodified entitled native client to fetch machine data over XPC. An unentitled client remained refused.
4. User's next native sign-in returned “Apple ID or password is invalid”, then wording approximately “Cannot Sign In — your Apple ID, password or verification code could not be verified. Try signing in again.” Exact latter wording is user-reported, not independently captured.
5. The account was created that day and is signed in only in Safari, with no trusted Apple device signed into that account. No 2FA screen or successful login has been confirmed.
6. Changed error wording does NOT establish that Apple accepted either the password or the emulated identity.
7. User was advised to pause S20 retries, change passwords exposed in chat, and verify a fresh web login. That check has not been reported complete. Never reproduce or use the credentials from chat in commands, files or logs.

## What was implemented and tested

- Mac AOSKit legacy header method exists but returned an empty dictionary.
- Native phone existing-data probe: AKAnisetteError -8004, underlying ADI -45061 (unprovisioned). One native provision-if-needed attempt returned -42410; its meaning remains unestablished.
- Reviewed upstream emulator: `sacca97/os-findmy-e`, commit `d4d2a832baf47d1cf43d989879950ac2aec45f47`.
- Isolated emulator uses a generic x86_64 slice of host CoreADI/adid, random virtual identity and synthetic state. No donor iPhone identity, Safari cookies or host keychain copied.
- Offline boot succeeded. One credential-free Apple provisioning exchange succeeded; later local reads returned nonempty machine ID/proof. This is not proof of account-login compatibility.
- Loopback-only token-authenticated broker returns generated metadata through an SSH reverse tunnel. It rejects extra fields including credentials and does not implement account login.
- Temporary Objective-C bridge injects into Preferences and akd. It replaces Anisette reads and overlays virtual identity headers while leaving Apple password/2FA checks in native code.
- Connected/disconnected sync and async calls, callback ABI and native header formatter smoke tests passed. Unauthorized clients remain refused.

## Important unresolved code concerns

- `anisette-lab-bridge.m` supplies emulated Mac identity headers inside native iOS authentication. The smoke test checks for MacBookPro13,2 client info; full request identity consistency has not been established.
- `nativeData` ignores its requested DSID; the provider uses its provisioned machine identity. Whether this is correct for all native sign-in stages is unverified.
- Native SRP/client-provided authentication data may carry other identity fields not covered by generic HTTP headers. Do not claim this is the cause without evidence.
- Data-delivery tests do not validate server acceptance, account status, password correctness, SMS verification or full iCloud support.
- Do not add unconditional authentication success, bypass account verification, extract donor secrets or keep retrying logins blindly.

## Runtime status and access

Last reported setup was enabled for trial version 4. Runtime has NOT been rechecked for this handover; do not assume the provider/tunnel or injection is still active.

- Host broker: loopback port 49731.
- Phone tunnel endpoint: loopback port 49732.
- Phone private config/status: `/var/mobile/Library/RedwoodAppleAccountLab/` (`connection.json`, `status.log`). Never print the config/token or machine headers.
- Last verified Wi-Fi SSH address: 192.0.2.10, using existing s20 SSH configuration and verified HostKeyAlias 10.77.0.2. USB SSH at 10.77.0.2 has been intermittent. Recheck addresses; do not disable host-key checking.
- Last phone-lock owner reported: `codex-gps`; current ownership is unverified. Consult workspace NEXT.md, TASKS.md and phone-lock status before device changes. Do not steal another worker's lock.
- No reboot was performed for this experiment. Ask the user before any reboot.

## Failures already understood

- Initial helper disconnect exposed NSError autorelease lifetime handling and crashed Settings/AuthKit. Error lifetime handling was fixed; disconnected-path tests subsequently passed.
- `prefs:root=APPLE_ACCOUNT` independently crashes this image's missing-specifier path, including before bridge testing. Open Settings normally from its icon; do not use that deep link.
- USB transport loss interrupted trial cleanup earlier. Wi-Fi tunnel was subsequently used. Always verify cleanup completed rather than infer it from attempted commands.

## Safe continuation

1. Read AGENTS.md, the detailed research document, workspace coordination files and current git status. Read passcode/SHIPPING requirements before any relevant system changes.
2. Confirm fresh web authentication with the user after password rotation; user enters all secrets directly in native UI or Apple's website. Do not use the pasted credentials or extract the existing Safari session.
3. If 2FA is enabled with no trusted device, Apple supports SMS/voice to a trusted number. A fresh account alone does not establish the failure cause. See https://support.apple.com/en-sg/102606.
4. Inspect current injection and process ownership without exposing private state. Keep diagnostic output to event names, availability booleans and error domain/code.
5. Review account-stage DSID handling and consistency between native authentication payload and emulated identity. Test changes without account login first. Do not request another user retry until there is a concrete justified change.
6. Record actual stage/outcome; native iCloud, App Store and iMessage each require separate verification.

## Rollback

- Inspect current `kern.exec_inject` first. Remove only the temporary Anisette bridge rule; preserve other workers' changes. A saved baseline exists privately at `out/apple-account/private/injection-before-native-trial.txt`, but restore it wholesale only after checking that no unrelated rules changed.
- Restart affected akd and close/reopen Settings after removing the rule: changing injection settings alone does not unload the library already in a process.
- Stop only the owned broker and SSH tunnel; remove the private connection config after unloading. Do not kill unrelated SSH processes.
- Keep provisioned virtual state private for diagnosis; do not automatically reset/reprovision it.
- Confirm normal native processes are running and the temporary bridge is absent. Release the phone lock only if held by this task.

## Files and privacy

Tools live under `tools/apple-account/`: metadata probes, native fetch probe, emulator probe, broker, bridge, smoke client and probe entitlements. Private dependencies, binaries, virtual identity/store, tokens and trial artifacts live in ignored `out/apple-account/private/`; never add that directory to git or distribute it with the handover. Do not run upstream header/status commands that print raw identity/proof values. This handover intentionally contains no account identifiers, credentials, OTPs, tokens or machine proofs.

## Mac application status0 confirmed; next worker runs ON PHONE

- Latest Settings10105 native iCloud401. Macworker10 supplied exact phone body/latest helper proof snapshot returnsHTTP200 and application rootstatus0, known mobileme/gamecenter/ids delegate statuses0. No account saved. This is stronger than prior HTTP-only comparison.
- Added --phone-worker to comparison broker. Next test uses SAME comparison source crosscompiled arm64iOS as fresh immutable /var/tmp/native-pet-comparison-v11, runs asmobile via SSH stdin, fresh public NSURLSession, native AA request construction/real server handling. No native completion responses replaced and no Settings/akd injection changed.
- Phone --build-test passes request construction; machine_metadata_availablefalse expected because iOS lacks AOSKit. Supplied snapshot overrides before transport. --snapshot-test with synthetic account tuple and account-free body/proof passes, no login sent.
- Comparison exec74734 worker11 --phone-worker, snapshot mode, after nativephone401 gate, one-shot rearmed. IMPORTANT old observer field says mac_http_status even though worker target NOW PHONE; local service event worker_target=phone is authoritative. No Mac logout required.
- Broker22104/helper7 and supervised USB1829 unchanged. Next user trial will locate native AAURLSession vs phone public-session behavior. No claimed fix yet.

## Snapshot comparison HTTP200; application-level status still unverified

- Phone Settings10093 / akd10074 AuthKit success, selected headers/credential match, iCloud401. Macworker9 supplied phone body/latest helper proof snapshot returnsHTTP200/noNSError. This is NOT yet confirmed application-level login success; previous workers discarded bodies.
- Added icloud-response-summary.h and synthetic test: numeric known status/error codes, tokens/error presence booleans, account-info presence, known delegate status nodes ONLY. No raw response/text/account/token values exported. Python broker applies independent fixed-schema sanitizer before local summary log.
- New immutable Macworker10 includes response status summary; synthetic privacy test and Mac account-free signing PASS. Current comparison exec49606 rearmed, same body/proof mode, no phone binaries changed. One fresh attempt needed because old response bodies/PETs intentionally discarded.
- Automatic USB reconnect supervisor1829 and broker22104/helper7 remain. Runtime JSON updated. Next inspect local service native_application_summary before claiming Mac app-level success.

## Connectivity healthy; proof-snapshot trial rearmed

- Both pinned Mac SSH and default phone USB SSH recovered. Worker9 account-free native signing PASS. Phone native auth RPC and bridge sync/async/metadata smoke PASS with fresh helper retrieval.
- Restarted owned previous comparison service94920 to load reconnecting watcher; current exec13455/worker9 snapshot comparison armed. Native broker22104/helper7, reconnecting USB supervisor1829. Current private runtime JSON updated.
- No new phone binary changes. One fresh S20 trial pending. Earlier stalled attempt did not complete this comparison.

## Latest stalled attempt: transport loss, recovery changes

- Settings10063 latest stalled trial records NSCocoaError4097 and broker-1004/unavailable, no completed PET comparison. USB reconnect recovered SSH; native auth RPC passed. Not a new server password rejection.
- Added phone-helper-tunnel.py supervisor (exec1829): pinned batch SSH, both loopback-only forwards,5sec keepalive/2 missed, retry2sec. Old single-shot tunnel gone.
- Fixed native broker: dead SSH child respawns on next fetch; failed initial warmup no longer kills HTTP service. Local synthetic subprocess test verifies successful fetch -> close -> next fetch recovers. Broker service exec22104/helper7, may be awaiting network.
- Comparison watcher source now reconnects after SSH EOF. Previous service86712 expired without tuple; not rearmed/restarted yet. Snapshot comparison remains pending; no new sign-in requested until reachability verified.
- Hackintosh114 became unreachable again (SSH/network timeout) despite earlier signing success. Need stable Mac network before further login. No phone binaries, boot profile or authentication checks changed. Latest completed comparison remains phone401 vs Mac200 with exact phone body/fresh Mac proof.

## Exact phone body also Mac200; proof-snapshot comparison ready

- Latest Settings10047 phone401, same credential tuple + exact schema-validated phone body through Macworker7 returns actualHTTP200. Mac generated fresh machine/signing proofs; cannot infer identical signed requests yet.
- New worker9 accepts narrow machine_headers snapshot (same exact10-field broker schema), aliases legacy MD and Device-UDID, preserves native UA selection, uses supplied body and validation proof without re-signing. No account saves; no response bodies recorded. Broker recent-signing endpoint now retains last validated body + generated proof snapshot in memory behind same private bearer, age90sec gate. This is latest generated snapshot, not a wire-byte capture.
- Mac worker9 --snapshot-test with synthetic account tuple and account-free phone body verifies snapshot_present/signature_preserved, no request sent. Phone native signing fixture also passes. Immutable phone binaries unchanged; Macworker8 built but not used, worker9 uploaded and frozen.
- Broker exec9397/helper7, comparison exec86712/worker9 with phone-body-token-file; USB combined59330 unchanged. One-shot config rearmed for next fresh S20 attempt. Runtime JSON updated. No proven fix yet.

## Three-service Mac also200; exact phone-body comparison ready

- Latest phone Settings10031 AuthKit success, selected headers/proofs/signature match, iCloud401. Same fresh tuple through worker6 three-service Mac request returned actualHTTP200/no NSError. Service list alone does not explain failure. HTTP200 does not prove delegate-level success; success response bodies discarded.
- New native worker7 accepts optional schema-validated icloud_body and substitutes exact bytes BEFORE native signing. Comparison service retrieves latest phone signing metadata from loopback broker via bearer-only GET /v1/recent-signing-metadata, age<=90sec and >=credential issuance-2sec. Schema validated in Python and Objective-C. Metadata only in memory, no credentials in body schema. Body/headers/proofs never logged. Phone binaries unchanged.
- Restarted broker exec43824/helper7; comparison exec80773/worker7 --phone-body-token-file private broker-token. USB tunnel59330 unchanged. Worker7 synthetic signing, phone unsent signature, unauthorized retrieval refusal and metadata schema tests pass.
- One-shot observer rearmed. Next S20 attempt compares phone body on Mac after native401; Mac200 would narrow to remaining headers/session/transport, Mac401 would implicate body differences. Do not substitute account tokens, bypass native checks, or claim password issue.

## Breakthrough comparison: same phone-issued PET, Mac HTTP200

- Real Settings10016 native AuthKit success, matched credential/alternateID, machine/provisioning/signature selected checks pass; phone AALoginAccountRequest HTTP401.
- Same fresh phone tuple passed to Mac worker5 AFTER native phone401: actual HTTP200, response_presenttrue, no NSError, no account saved. This establishes a request/context differential, not a globally rejected PET. HTTP200 alone does not prove every requested delegate succeeded; response body discarded.
- Worker5 requested only com.apple.mobileme. Phone requests three services. New immutable worker6 requests mobileme + gamecenter + private.ids protocol4, matching known phone fixture. Build and synthetic body-sign test PASS, no login sent by those tests.
- Comparison service exec54100 selects worker6; one-shot config rearmed. Broker55771/helper7 and USB combined tunnel59330 unchanged; no phone binary or native request changes. One S20 attempt required for fresh transient credential; no Mac logout.

## Connectivity restored after stalled attempts

- Two stalled attempts Settings9932 returned NSCocoaError4097, AuthKit restarted9951/9952; broker transport was absent. No completed iCloud credential rejection established for those attempts.
- USB reconnected, both forwards restored exec59330. akd9952 signature valid and read-only auth RPC passes. Hackintosh114 was temporarily unreachable, now pinned SSH and worker5 synthetic signing pass.
- Restarted owned stale native broker exec55771/helper7 and comparison service exec89608/worker5. Phone bridge smoke and account-free native iCloud signing fixture PASS with selected metadata matched, signature present, no signing error.
- One-shot config rearmed. No phone binary changes, account reset, reboot, or Mac logout. One fresh S20 attempt pending.

## Latest result: helper7 phone401, Mac comparison inconclusive

- Settings9713 AuthKit success, matched credential and alternateID, selected machine headers, legacy proofs and signature present; native iCloud401 persists.
- Worker4 comparison status0/response_presentfalse/errors[] is NOT a Mac rejection. Worker error stage was lost by broker. Broker now preserves fixed worker_failure_stage in its local summary.
- Subsequent account-free build test passed; provider empty-fixture check failed transiently; direct native validation probe and new worker5 --sign-test both passed. No root cause established.
- Worker5 adds account-free body-signing test, otherwise comparison unchanged. Comparison service exec5706 rearmed, USB tunnels unchanged, helper7 unchanged, phone binaries unchanged. One user attempt needed for fresh ephemeral credential; no Mac logout or parallel sign-in needed.

## Current continuation: AOS identity-format trial ready over USB

- Previous worker3 comparison returned real HTTP401; its public HTTP path omitted automatic machine metadata, so this is not conclusive evidence against the PET.
- Helper7 pairs AOSUtilities OTP with base64(UTF8(machineUDID)) local-user ID, following AltStore AOSKit implementation. This is a controlled trial, not a proven cause. Native deviceID coherence verified. Worker4 now supplies full machine metadata too.
- Native broker running helper7; USB reverse ports49742 and49752 restored. Comparison watcher now uses default USB SSH. Old Wi-Fi link timed out.
- Account-free phone native-request and bridge smoke tests PASS: aligned metadata, body-bound signature, no signing error, no account request. Comparison health returns expected unauthorized response.
- One-shot daemon observer2 rearmed; worker4 waits for native phone401 before comparison. No phone binary changed. Runtime session IDs in ignored private pet-comparison-runtime.json. User retry pending.

# S20 Apple Account handoff

Updated 2026-11-16. **Native Apple Account/iCloud sign-in is NOT working.**
Start here; older chronological notes are in
[APPLE-ACCOUNT-RESEARCH-HISTORY.md](APPLE-ACCOUNT-RESEARCH-HISTORY.md).

## CURRENT: corrected Mac worker3; first comparison readout was invalid

- Real Settings8319/akd8303 trial: SRP succeeded, AuthKit result available, native
  phone iCloud401 with matched PET/altDSID/provisioning/signature; validity intact.
  Daemon observer captured tuple; phone401 gate released Mac comparison.
- Worker1 returned http_status0/response_presenttrue/errors[]. This is INCONCLUSIVE:
  its native AARequest completion signature was WRONG. Static native callback
  0x19ed69ba8..0x19ed69bb4 passes request,response,error (three values afterblock),
  but worker treated first asresponse. No Mac acceptance/rejection established.
  Retrospective sanitized helper logs also show4099; do not count this as success.
- Corrected worker3 now constructs native AALoginAccountRequest, obtains a direct
  native AAAbsintheSigner body signature, then sends via public NSURLSession.
  Its response is actual NSHTTPURLResponse; bodies/tokens discarded. Does not
  rely on protected daemon AAURLSession/XPC context or wrong private callback.
  Native metadata/legacy provisioning selection retained, no account save.
- New --loopback-test uses a FRESH request (header dictionary assignment can
  merge retained fields), synthetic native body and ONLY content-type. Mac-owned
  local receiver verified real200 AND401 statuses and NO Authorization header,
  no Apple request. Evidence mac-worker-v3-loopback-tests.jsonl. Worker3 uploaded
  once under fresh filename and frozen. Worker1/2 should NOT be used.
- Latest comparison service exec7137 selects native-pet-comparison-v3; existing
  tunnel84482/phone49752 remains. Private one-shot config rearmed; endpointhealth
  passes. No phone libraries or daemon restarted for this readout fix.
- Real server code-20101/resyncTrue at request-events PID8291 was a SYNTHETIC
  selftest fixture, NOT a live server failure. Latest real SRP8303 succeeded.
- Live phone unchanged SRP3/daemonPETobserver2, Settingspassive9/bridge15;
  native daemonpassive7/bridge14. One corrected comparison attempt pending.
  Mac200 would localize phone request/context, Mac401 alone remains limited by
  standalone-helper differences; status0 remains inconclusive. No S20 sign-in.

## CURRENT: SRP3 server failure metadata / observer2 still armed

- User's account-information-incorrect trial Settings8275 / akd8255 stopped EARLY:
  AuthKit-7018, underlying AppleIDAuthSupport2. Exact SRP category from existing
  probe2 is selected_protocol_missing. No PET getter capture and no iCloud401;
  Mac comparison did not run. Both native processes retained valid signatures,
  correct identities and required internal entitlement. This is NOT previous
  code-validity regression or a captured iCloud401.
- Static AKSRPService _parseFailedServerResponse:withError:context:completion:
  at0x10005c66c reads native server errorCode and resync/reprovision/URLswitch/
  retry/secondary-action flags. These fields can distinguish server-directed
  recovery from generic dialog; absence of selected SRP protocol alone cannot.
- Added srp-server-summary.h and test-srp-server-summary.m. New passive SRP3
  observes that handler, logs only numeric server code and fixed boolean flags,
  then calls original with exact response/error/context/completion unchanged.
  Synthetic hook preserves arguments/completion and excludes raw text.
- Built/signed/uploaded fresh srp-error-probe-v3.dylib ONCE and kept unchanged
  after testing. Replaced only SRP2 rule with3. Closed Settings, restarted akd8303.
  Code-valid637534209, identitycom.apple.akd, entitlement reads succeed; health RPC
  and comparison tunnel pass. Real marker server_failure_hooked1 confirmed.
- LIVE: SRP3 akd, daemonPETobserver2 akd; bridge15 Preferences/14 akd;
  passive9 Preferences/7 native daemons. One-shot config stillarmed, comparison
  server26887/tunnel84482 stillawaiting first successful native result + phone401.
- One new user attempt needed: if early auth fails, inspect srp_server_failure_metadata;
  if auth succeeds then401, comparison can run. Do not reset provisioning or
  claim account lock/bad password from selected_protocol_missing alone.

## CURRENT: immutable observer2; code-signature validity recovery verified

- User's unknown-error trial: AuthKit-7001 with underlying com.apple.accounts7
  (SDK ACErrorPermissionDenied), no native iCloud request/401. Daemon observer1
  staged tuple, but local service waited for phone401, timed out502; Mac worker
  was NOT invoked. Do not call this a Mac PET rejection.
- Actual akd8198 csops status637534208 (0x26000000): CS_VALID bit ABSENT;
  CS_OPS_IDENTITY11 and entitlement7 returnedEINVAL22. This directly explains
  missing signing identity/entitlement reads in native process. No bypass added.
- Observer1 file had been rebuilt/reuploaded at SAME phone path after fixture
  loading. Suspected port stale vnode/page/signature cache. Same pattern may
  explain earlier passive8/10 local regressions; not proven for every old trial.
- Removed observer1/restarted: baseline akd8234 status637534209, VALIDpresent,
  identitycom.apple.akd, internalAuthKit entitlementpresent, querieserrno0.
- Built/signed/uploaded observer2 at a FRESH immutable path EXACTLYONCE, tested
  getter and then installed unchanged. akd8255 stays code-valid637534209,
  identity/entitlementsreadcorrectly and auth-service RPC passes. Evidence in
  akd-without-observer-signature-health.json / akd-observer-v2-signature-health.json.
- DEPLOYMENT TRAP: never overwrite any already-tested/mapped signed phone artifact,
  even before "live" deployment. New bytes require a NEW filename. Freeze final
  binary after tests; never "repair" by forcingCS_VALID or bypassing entitlement.
- LIVE extra observer rule now daemon-pet-comparison-v2.dylib akd; all other
  bridges/passives unchanged (Settingspassive9, bridge15; nativeakd14/passive7/SRP2).
- One-shot config rearmed privately after service/tunnel health. New service exec
 26887, existing comparison tunnel84482/exec78571 reused. Runtime file updated.
  NativeMacworker stillv1 and machine-proof broker/helper unchanged. Fresh user
  S20 attempt pending; waitabout60s after error, then inspect both logs.

## CURRENT: native-daemon one-shot comparison; Settings unchanged passive9

- Running Settings8156 verified with CORRECT csops operation11: signing identity
  com.apple.Preferences, native Apple-binary status bits match, internalAuthKit
  entitlement present. Read-only status/identity/entitlement queries errno0.
  Earlier probe versions mistakenly used operation6 for identity; false/unparsed
  output was probe error, NOT evidence of a bad app signature. Use v3 only.
- New daemon-pet-comparison.m hooks only native
  AKAuthenticationServerResponse passwordEquivalentToken getter in akd. Always
  returns exact original value. Optional one-shot capture uses native appleID,
  DSID, altDSID and PET string only; no raw password, success/admission bypass.
  Getter object/nil preservation test passes with synthetic class and configabsent.
- Settings remains on passive9; client-side passive10/8 are NOT reinstated.
  New observer rule `/var/tmp/daemon-pet-comparison-v1.dylib akd` appended only.
  Closed Settings, restarted native akd8198; observer marker with matchingpid,
  read-only auth-service RPC and private tunnelhealth pass. Normal Settings open
  requested; user may need to open icon manually as before.
- New comparison server --wait-for-phone-failure tails existing sanitized phone
  network-events over pinned SSH. It waits for a real post-capture native
  AALoginAccountRequest401 before invoking the Mac worker. No racing two endpoint
  requests. No new code in Settings beyond already-working passive9.
- One-shot config REARMED privately (0600mobile501), unlinks when native PET tuple
  captured. Raw reference expires120s; private transfer response timeout110s;
  local metadata observer waits45s for phone401. Status0/failure is inconclusive.
- Server exec84285, tunnel exec78571; runtime file updated. New daemon observer
  logs only /var/mobile/Library/RedwoodAppleAccountLab/pet-comparison-events.log.
  Read it for native_pet_comparison_completed; existing network-events retains
  original phone result/PET equality. First real comparison still pending.
- Live: bridge15 Preferences / bridge14 akd; passive9 Preferences / passive7
  native daemons; SRP2 akd; extra daemon observer1 akd. No kernel or passcode edits.
  Existing provider/tunnel49741/49742 unchanged. Both Macs must stayawake.
- Next user action: one S20 attempt, error left open, waitabout60s, check. Validate
  result against corresponding native phone401 and nativeAuthsuccess. No protected
  Mac token reuse, account save, raw secret logging or success claim permitted.
- Cleanup afterward remove only extra observer rule and one-shot config, restart
  akd/Settings to unload, stop verified owned49752 tunnel/service if stillrunning.

## CURRENT recovery: comparison disarmed; inspect Settings client identity next

- User's next attempt never reached password UI. Settings8103 captured repeated
  NSCocoa4099, while akd7960 stayed alive and read-only auth RPC still passed.
  No native_pet_comparison_started event: no phone PET reached Mac worker.
- Restored passive9 Preferences (from10), removed private native-comparison.json,
  closed Settings and restarted user501 akd. Native auth health passed again.
  bridge15 Preferences/bridge14 akd, passive7 daemons/SRP2/rendering preserved.
- Stopped only verified owned pet-comparison server and49752 reverse tunnel.
  Runtime marks experiment_stopped_before_token_capture; machine-data broker and
  original49742 tunnel remain separate. Do not reuse the unarmed experiment.
- New read-only `code-signature-health.m` reports target csops status flags,
  signing-ID equality with com.apple.Preferences and internalAuthKit entitlement
  presence. Never prints complete entitlement blob or changes any signing state.
  Built/signed/staged /var/tmp/code-signature-health. Settings not running during
  initial query, so no process-signing conclusion drawn.
- Asked user to open Settings and leave it open WITHOUT entering account again.
  Next: locate actual running Preferences PID and run health probe. Then inspect
  native akd admission/audit-token evidence, before reinstating any new diagnostic.
- Existing port code documents prior audit-token/pidversion entitlement failures;
  native csops_internal rejects mismatched token pid/version asESRCH. This is only
  a hypothesis for4099. No kernel, passcode, auth-success, entitlement-bypass or
  security-setting changes made. Do NOT copy the attention-entitlement fallback
  into authentication. Preserve native access checks.
- Sign-in remains unresolved separately (previous real iCloud401 afterAuthKit).

## CURRENT discriminating experiment: one-shot native Mac PET comparison

- bridge15 real Settings8056 attempt still401. PET and alternate-DSID equality,
  legacy provisioning pairs, provider identity/signature all match. User-Agent
  preservation did not suffice. No success claimed.
- User explicitly authorized deep own-device/account inspection, including
  credentials. Scope is now expanded for THIS one-shot comparison: a fresh S20
  PET/username/DSID/alternateDSID may transit privately in memory to the owner's
  Hackintosh. No raw password, Mac stored tokens, keychain or Safari session used.
  This supersedes earlier self-imposed phone-only PET constraint for this test.
- Separate new tools: native-pet-comparison.m (Mac native unsaved request),
  pet-comparison-server.py (local one-shot endpoint), pet-comparison-trigger.h
  (Settings after401), comparison-transport-health.m (no-bearer local health).
  Original machine-proof helper/broker remain unchanged and accept no credentials.
- Mac worker `.local/share/redwood-account-lab/native-pet-comparison-v1` reads one
  bounded fresh JSON envelope only on private stdin, constructs an in-memory
  AppleAccount and calls native AALoginAccountRequest performSignedRequest. Returns
  only HTTP status/error codes/booleans. Never saves account or returns tokens.
  --build-test passed with synthetic credentials and NO network request.
- Local server127.0.0.1:49751, tool exec44699; reverse SSH phone49752->local49751,
  exec56852. Exact runtime in ignored pet-comparison-runtime.json. Isolated bearer
  is private pet-comparison-token; NEVER print it. Rejects invalid/extra/raw-password
  fields; single valid use, auto shutdown after comparison or30min.
- Phone native-comparison.json (0600 mobile501) ARMED. Passive10 Settings captures
  only native AuthKit success tuple, unlinks one-shot config, retains references
  at most120s, triggers private Mac comparison only after native iCloud401. Phone
  request/completion unchanged. Logs only comparison started/status/error metadata.
  No PET/password written to any artifact. No context getter inspection.
- LIVE passive10 Preferences; passive7 daemons; bridge15 Preferences/bridge14 akd;
  SRP2 unchanged. Restarted Settings only, auth RPC healthy. Callback/error/privacy
  smoke and tunnel401 health pass; no real comparison request yet.
- User should perform ONE S20 attempt, leave error open, wait about60s, then check.
  Read native_pet_comparison_completed in network-events.log and local service
  output. Mac200 would strongly localize phone request construction/context.
  Mac401 alone does NOT prove bad PET: standalone CLI may differ from protected
  native daemon context. Status0/transport failure is inconclusive. Do not report
  successful S20 sign-in just because standalone comparison200.
- Cleanup after result: remove ONLY passive10/one-shot comparison config (or
  replace Settings with passive9), stop verified owned49752 tunnel; original
  provider49741/49742 must remain. One-shot service shuts down itself. No reboot.

## CURRENT trial: bridge15 Settings; native Mac login comparison complete

- User successfully re-logged native Mac with SMS, Mac password and setup prompts.
  Sanitized comparison captured273 initial records; private file
  mac-login-comparison-events.jsonl. Collector still bounded by its30min lifetime.
- Static x86 native framework analysis proves Mac also sets account credential
  from AuthKit PasswordKey/PET, while RawPasswordKey is stored separately.
  See APPLE-ACCOUNT-BINARY-NOTES.md for method-relative offsets and exact evidence.
  No raw-password substitution and no working Mac token extraction/replay done.
- Mac actual native account-free request has no explicit User-Agent. Prior
  nominal-vs-request inequality was absence, not a confirmed different value.
  bridge15 removes the forced nominal iCloud User-Agent override, preserving
  native selection. AuthKit metadata path and all credential/body/signing logic
  unchanged. Synthetic and phone unsent/signature/AuthKit smoke pass.
- LIVE bridge split: bridge15 Preferences, bridge14 akd. Passive split remains
  passive9 Preferences, passive7 akd/accountsd/appleaccountd; SRP2 akd.
  Restarted/reopened only Settings; akd7960 remains running; health RPC passes.
  No account changes on phone, reboot, security-setting change or claimed success.
- Active Mac helper remains provider-v5 and existing broker. provider-check-v6
  adds safe native request header-presence checks only. A staged provider-v6
  artifact is INACTIVE and should not replacev5 (intermediate experiment).
- One real S20 user trial pending with both Macs awake. If still401, do not
  reinterpret this header correction as established root cause.

## CURRENT comparison: native Mac re-login capture armed

- Real Settings8013 attempt: fresh PET authorization equality TRUE, alternate
  X-Apple-ADSID equality TRUE, all selected provider and legacy MD pairs match,
  signature present, no signing-error; iCloud still401, no WWW-Authenticate header.
  This excludes the measured account-ID mismatch too. No new phone adapter change.
- User explicitly authorizes deeper diagnostic inspection of their own Mac/account,
  and confirms willingness to repeat native logins. No credential needs sharing.
- SSH noninteractive sudo already works. csrutil reports debugging and DTrace
  restrictions enabled. Root dtrace probe listing against user akd2430 fails to
  grab process. Do not claim administrative access equals attach capability.
  No SIP change, reboot, daemon injection, sign-out or provisioning reset by tools.
- Added `native-login-log-summary.py`. Runs on observed Mac, reads native log stream
  via existing sudo, retains only fixed phase/request/endpoint labels, allowlisted
  NSError domain/codes and bounded HTTP statuses. Raw text/account/token/password
  values discarded; no private-data log configuration enabled. Privacy selftest
  passed locally and remotely.
- Detached30-minute collector running; exact PID/output/status are in ignored
  out/apple-account/private/mac-comparison-runtime.json. Start marker verified.
  Retrieve ONLY sanitized output and check child/collector liveness. Do not run
  a second collector needlessly. Expected user action: System Settings Apple
  Account sign-out/re-login, keep local copies if offered, S20 idle, then report.
- Mac helper remains installed. During native sign-out its account context may
  change; after re-login recheck --check and broker before a phone trial.
- Live S20 unchanged: bridge14, passive9 Settings/passive7 daemons, SRP2. Native
  login still unresolved; next evidence is Mac comparison, not another phone retry.

## CURRENT: Settings-only passive9 / daemon passive7 / bridge14

- User confirms password page restored after passive8 rollback/restart. Fresh
  attempt Settings7982/akd7960 still AuthKit success then native iCloud401;
  PET equality and machine/provisioning/signature checks pass.
- Static native aa_addAltDSIDAndRepairStateWithAccount at0x19ed67f0c reads account
  aa_altDSID into X-Apple-ADSID (CFString0x1d76378d8), repair state into X-Apple-I-Repair.
  AAiCloudLoginAccountRequester at0x19ed6b488 unconditionally creates modern
  AALoginAccountRequest and calls performSignedRequestWithHandler; no fallback
  branch in this method. No unsupported legacy endpoint substitution made.
- passive9 removes ALL pre-call context getter inspection from8, retains only
  post-result keyed alternate-DSID comparison and response challenge scheme.
- Extended network-probe-smoke-v4 replaces native authentication IMP ONLY in its
  isolated test process with synthetic completions before installing probe. It
  verifies exact success/error callback identity and never calls native auth.
  This test, failed-response privacy and local HTTP/error preservation pass.
- Live split: passive9 ONLY Preferences; passive7 akd/accountsd/appleaccountd.
  Native akd7960 left running to avoid unnecessary XPC churn. Settings restarted
  and reopened. Native auth-service read-only health check passes. bridge14/SRP2,
  rendering and unrelated injection preserved. New user test pending.

## CURRENT recovery: passive8 rolled back to7 after pre-password failure

- User could not reach password field after ID/Next. Settings7935 passive8 captured
  repeated native NSCocoaErrorDomain4099 before any AuthKit result. akd7932 remained
  alive; no Settings/akd crash file identified. Do not call this old iCloud401 or
  assert a broken app signature. Context trace did capture authenticationType2,
  serviceType1, no explicit service identifiers, appProvidedContext present.
- Replaced ONLY passive8 rule with prior passive7. Closed Settings, restarted
  user/501/com.apple.akd (launchctl maps old system identifier to this domain).
  bridge14/SRP2/rendering/unrelated rules remain unchanged.
- akd7960 loaded. Entitled no-provision native Anisette RPC returns data and proof.
  New auth-service-health.m performs read-only isDevicePasscodeProtected RPC,
  discards state value and records only error presence; native auth-service RPC
  completed without error. No login, credential/keychain access or state change.
- Reopened Settings via normal bundle launch. User password-screen verification
  pending. Rollback/restart recovered RPC health; exact cause of4099 is NOT proven.
  Do not reinstall passive8 until real authenticateWithContext behavior is isolated;
  offline smoke does not establish UI/XPC serialization correctness.
- Live is bridge14/passive7/SRP2. iCloud sign-in still unresolved separately.

## CURRENT capture: passive8; bridge14 unchanged

- User confirms any number of manual attempts is acceptable; keep them tied to
  discriminating tests. No need to sign out working Hackintosh.
- Real bridge14 attempt still401 despite matched legacy MD pair. AuthKit succeeded,
  outgoing username/PET equality TRUE, signature present, no signing-error.
- Added static executable ObjC annotation tool; see APPLE-ACCOUNT-BINARY-NOTES.md.
  Native akd definitively returns com.apple.gs.idms.pet as PasswordKey; raw password
  has a separate result field. Do not equate server's rejection with mistyped input.
- Added auth-context-summary.h/test and default-auth-context.m. Mac and phone empty
  context probes run without authentication. Summary emits selected integer/boolean
  flags and allowlisted service names only; unknown identifiers/content excluded.
- Extended ephemeral credential comparator with alternate-DSID keyed equality.
  Passive8 records native context at authenticate call, outgoing X-Apple-ADSID
  presence/equality against fresh AuthKit result, and WWW-Authenticate scheme only.
- Host/phone privacy and equality/expiry tests plus existing loopback/error smoke
  pass. Installed passive8 rule in place of7; Settings closed and akd restarted.
  bridge14/SRP2 and unrelated rules unchanged. New real capture pending.

## CURRENT trial: native AppleAccount MD header pair (bridge14 / passive7)

- Static native iOS method AADeviceProvisioningSession
  addProvisioningInfoToURLRequest:sendEmptyValues: at0x19ed963d4 reads mid_data /
  otp_data and writes **X-Apple-MD-M / X-Apple-MD**, not AuthKit I-MD names.
  Resolved header constants0x1d762aad8/0x1d762aae0 in the local cache.
- Prior adapter aligned only X-Apple-I-MD / X-Apple-I-MD-M; native legacy MD pair
  could remain missing or from a different identity. Existing real capture did not
  inspect that pair, so actual prior presence is unproven.
- bridge14 aligns both legacy MD header values from the SAME native helper
  snapshot as the I-MD pair. Preserves Authorization, body, method, endpoint and
  existing signature. Generic DSID-2 limitation remains: this change fixes header
  namespace/pair consistency, not demonstrated account-scoped provisioning.
- Local synthetic test replaces stale native pair without mutating input or
  credential/body. Phone account-free unsent request verifies both legacy headers
  present/equal to corresponding provider aliases, valid body-bound signature,
  no signing-error. Bridge smoke and passive privacy/HTTP preservation pass.
- passive7 adds presence/equality booleans for legacy iCloud header pair; preserves
  passive6 AuthKit-to-Authorization keyed comparison.
- LIVE rules now bridge14 Preferences/akd, passive7 Preferences/akd/accountsd/
  appleaccountd, SRP2 akd. Settings closed, akd restarted. Other daemons may retain
  old passive module until next restart; Settings/akd load new versions.
- One user sign-in trial needed. No success claimed; do not substitute a blind
  retry or assert namespace correction necessarily fixes iCloud401.

## Latest real v6 handoff result and native DSID comparison

- User's requested retry, Settings7857 / akd7853: AuthKit success with all expected
  result keys. Both cancelled and HTTP401 iCloud tasks report a fresh reference,
  Authorization present, equality with AuthKit username+credential TRUE, equality
  with DSID+credential FALSE. Selected provider metadata matches, signature present,
  signing-error header absent. This rules out a copying/serialization mismatch
  at the observed handoff; it does not establish token audience or server acceptance.
- v5/v6 failed-response summary still plist/unclassified, no recognized numeric
  codes. Native AALoginAccountResponse parses its delegate body only on HTTP200;
  do not infer an application error from status-message on an unauthorized response.
- Added and ran `native-dsid-scope-probe.m` on the working Hackintosh. Reads only
  known numeric DSID preference fields; never prints identifiers or proof values.
  Generic AOS DSID-2 proof available; actual signed-in account DSID lookup yields
  no proof. Native aa_addDeviceProvisioningInfoHeadersWithDSID: on an unsent
  request likewise supplies no MD, MD-M or routing for that DSID in this CLI context.
- Thus current helper's generic-DSID proof is not shown to reproduce native
  account-scoped provisioning. This is a remaining compatibility gap, not a proven
  explanation for401, and absence in a standalone CLI does not prove the signed-in
  Mac's daemons lack account provisioning. No reprovisioning/account change done.
- Live configuration unchanged by this analysis: bridge13 / passive6 / SRP2.
  No new user retry requested. Next inspect native provisioning-session/XPC context
  and requested token application/audience before changing credentials or headers.

## Continuation: live credential handoff diagnostic v6

- Added `credential-handoff.h` / `test-credential-handoff.m` to passive probe.
- At native AuthKit callback, constructs ephemeral keyed HMAC references for
  username/password and DSID/password Basic forms. Random key and digests stay
  only in process memory, replaced at next completion, unavailable after120s;
  comparison clears expired state. No credentials, digests or keys are logged.
- For only setup.icloud.com/setup/signin/v2/login, network completion reports
  reference freshness, authorization presence and the two equality booleans.
  A false match alone does not prove corruption: native code may legitimately
  choose another credential/token. No fresh reference means comparison inconclusive.
- Host and phone synthetic equality/mismatch/replacement/expiration tests pass.
  Existing failed-response privacy and loopback/error-preservation smoke pass.
- Configured `/var/tmp/network-error-probe-v6.dylib` in place of v5 only.
  Closed Settings, restarted akd; other daemons may still retain v5 until restart.
  Bridge13 and SRP probe2 unchanged. One discriminating user attempt pending.

## Continuation: SRP classification and synthetic credential contract

- Continued under coordination session `codex-apple-account-cont`.
- Rechecked native Hackintosh helper via pinned SSH: machine metadata and validation
  available. No sign-out, account changes, keychain extraction or login by tools.
- Newer live capture after the -7006 failure shows SRP success followed by the
  existing iCloud401. Passive v5 summary is plist/unclassified with no recognized
  numeric codes. Do not continue describing -7006 as the only/latest blocker.
- Inspected AppleIDAuthSupport __cstring directly in the local iOS shared cache:
  distinct labels include missing M2, wrong-size M2, bad M2, missing selected
  protocol, salt, iteration and B. Code2 alone still cannot identify the reason.
- Added `srp-error-category.h` and `test-srp-error-category.m`. Exact-label category
  matching reads only three known NSError description fields; raw text, unknown
  userInfo and proof/credential data are never emitted. Host and phone tests pass.
- Built and installed `/var/tmp/srp-error-probe-v2.dylib akd`, replacing ONLY the
  previous SRP probe rule. akd restarted; installed marker verified. Bridge13,
  network-probe5, rendering and all unrelated injection rules preserved.
- Added `icloud-credential-contract.m`: creates an in-memory synthetic .invalid
  account, never saves it or sends requests. On phone, native Authorization equals
  the expected synthetic Basic credential both without and with bridge13. This
  verifies fixture serialization, NOT live AuthKit-result handoff correctness.
- No successful sign-in and no new fix claimed. Need live handoff instrumentation
  or a discriminating user attempt with SRP categories; avoid another blind loop.

## Latest result and the distinction that matters

The user reports the same secondary account works in fresh Safari authentication
and on their Hackintosh. They enter credentials only on the S20. Tools have not
used any password pasted in chat. Do not copy those passwords into commands,
files, logs, helpers or this handoff.

Latest attempt, Settings pid7773 / akd pid7766:

- AuthKit completion failed: `AKAuthenticationError -7006`, underlying
  `com.apple.AppleIDAuthSupport 2`; result unavailable.
- This failed BEFORE AALoginAccountRequest. No detailed iCloud response was
  available for the new passive response parser to summarize.
- Therefore do not describe the latest failure as the previously captured
  iCloud401. Do not assert these codes prove a mistyped password, cooldown,
  account lock or device rejection without establishing their meaning.

Earlier real v13 attempt, Settings pid7735 / akd pid7732:

- AuthKit completion succeeded without reported error; expected username,
  password, DSID and alternate-DSID result keys present (presence only).
- Native iCloud request carried a body-bound validation signature, no native
  signing-error header, and matching selected machine/service metadata.
- `setup.icloud.com` AALoginAccountRequest still returned HTTP401,
  `com.apple.appleaccount 401`. This remains unresolved separately.
- Native Mac and iOS account-free AALoginAccountRequest use the SAME
  `/setup/signin/v2/login` endpoint and root/user field structure. Do not
  assume the endpoint alone is an iOS-versus-Mac mismatch.

Private sanitized evidence (ignored out): latest-authkit-7006-events.json,
v13-real-401-events.json, v12-real-proof-401.json, handoff-runtime.json.
No successful iCloud account, App Store or iMessage login has been demonstrated.
Stop asking for repeated retries without a specific new test or change.

## Workspace, authorization and coordination

- Worktree `~/ios-port-research/work/hackios-apple-account`, branch
  `feat/s20-apple-account`. Source tools and task docs are currently UNTRACKED,
  not committed; preserve them. Do not run git clean or discard this worktree.
- User explicitly transferred this experiment from codex-gps and authorized the
  user-owned Hackintosh helper. No further permission needed for ordinary scoped
  inspection/fixes. No messages sent to other people. Genuine iPhone untouched.
- Read AGENTS.md, workspace NEXT.md/TASKS.md and this handoff. Before boot/root,
  keybag or graphics work read the referenced passcode/SHIPPING documents.
  Do not reboot without asking, bypass native authentication or alter passcode.
- Coordination session is codex-apple-account. This handoff releases its locks
  and claims after recording state. New agent must acquire the shared phone lock
  under its own session name before mutations; check actual ownership first.
- Settings-only Metal-off prevents mangled text. Preserve it and all unrelated
  injection rules. `prefs:root=APPLE_ACCOUNT` independently crashes this image;
  open Settings from its icon. No reboot/account removal/keychain/cache reset done.

## Current live setup (rechecked at handoff)

- Phone Wi-Fi SSH: existing s20 config, Hostname192.0.2.10,
  HostKeyAlias10.77.0.2. USB10.77.0.2 is intermittent. Preserve host checking.
- Injection bridge: `/var/tmp/anisette-lab-bridge-v13.dylib Preferences akd`.
- Passive probe: `/var/tmp/network-error-probe-v5.dylib Preferences akd accountsd appleaccountd`.
- Existing SRP probe: `/var/tmp/srp-error-probe.dylib akd`.
- Existing Settings rendering rule: `/var/jb/usr/lib/libredwood-metaloff.dylib Preferences`.
- Phone private config/status directory:
  `/var/mobile/Library/RedwoodAppleAccountLab/`.
  Logs `status.log`, `network-events.log` contain only scoped metadata.
  **Never print connection.json** (bearer token). Enabled loopback port49742.
- Local native broker127.0.0.1:49741, PID66881, exec session27833.
- Owned reverse Wi-Fi tunnel PID57071, exec session3471:
  phone127.0.0.1:49742 -> local127.0.0.1:49741.
- Hackintosh `user@hackintosh.example`, Intel macOS15.8.1 build24H32.
  Isolated binary `~/.local/share/redwood-account-lab/hackintosh-provider-v5`.
  Dedicated key and pinned known-host file are in ignored out/apple-account/private/:
  `hackintosh_ed25519`, `hackintosh-known-hosts`. Never print/copy private key.
  No added public TCP server, launch agent, sudo/security change or account sign-out.
- Old emulated provider/tunnel may remain IDLE standby on49731/49732. Not the active
  identity source. Do not reset provisioned state, mix sources or kill unrelated SSH.
- Processes/session IDs are observations, not durable identifiers: verify command
  and listening port before restarting/stopping a process. Tool exec sessions may
  be unavailable from a new chat; shell processes can be checked independently.

## What the code does and its limits

See [tools README](../tools/apple-account/README.md) for commands and build steps.

- `hackintosh-provider.m` gets native AOS machine proofs and native device identity.
  For scoped iCloud requests it uses native AADeviceInfo client-info/User-Agent;
  GSA requests use native AuthKit metadata. AAAbsintheSigner signs exact bytes.
  `--check` prints only booleans; **--stdio replies contain proofs and MUST only
  flow into the private broker pipe**, never a terminal/log/tool text output.
- `native-provider-server.py` loopback bearer-token broker uses pinned persistent
  SSH. Nonce, freshness, exact input digest and fixed response schema checked.
  No account-login endpoint or arbitrary signing payload endpoint.
- `icloud-signing-metadata.h` shared phone/helper validator, mirrored in broker:
  <=16KB plist, exactly protocolVersion/userInfo/delegates; protocol1.0;
  userInfo exactly UUID client-id, bounded language/timezone. Up to three
  delegates: empty com.apple.mobileme, empty com.apple.gamecenter, and
  com.apple.private.ids with ONLY protocol-version string1..4. Unknown fields,
  credential/token fields and arbitrary inputs are rejected before forwarding.
- `anisette-lab-bridge.m` modifies native Anisette data delivery, aligns AuthKit
  CPD metadata, overlays selected native iCloud headers and attaches an exact-body
  signature. Also hooks the native supplemental signing callback. Separate GSA
  and iCloud caches prevent service metadata contamination. Original credential
  verification remains Apple code, HTTPS and Apple's actual server responses.
- Known limitations: private daemon hook ignores requested DSID; generic AOS helper
  uses DSID-2. Routing84215040 is an AltServer compatibility HINT, not read native
  routing. AADeviceInfo User-Agent differs from the actual native AARequest UA on
  BOTH platforms; no evidence that another UA tweak resolves this. Native body
  and account credentials remain unchanged. Some helper-unavailable paths fall
  back to original native request; do not call the entire adapter fail-closed.
- `network-error-probe.m` passive v5 preserves native completion/error objects.
  Failed AALoginAccountRequest responses only (400..599, <=64KB) expose allowlisted
  numeric error codes and fixed message categories; no raw text, unknown field
  names, account/token values or successful bodies. Latest attempt stopped at
  AuthKit, so this new detailed iCloud summary has not yet captured a real failure.

## Meaningful verification already completed

- Native sync/async Anisette delivery, callback ABI and AuthKit formatter smoke.
- Disconnected helper/error lifetime paths fixed and tested earlier; no false
  success data generated. Native access check still refuses unentitled client.
- Real three-service schema/signing accepted in latest iCloud-stage trials.
- Unsent native request signing passes through phone/broker/Hackintosh. Private
  loopback NSURLSession receiver verified proof header delivery with allowed
  metadata and no authorization/signing-error header; extra test tunnel removed.
- Distinct native service metadata/caches verified with booleans. These tests do
  NOT establish Apple accepts the machine identity or iCloud credentials.
- Synthetic failed-response summary excludes tokens/account/raw text, ignores
  successful responses, preserves native NSError, HTTP status and network errors.
- New native contract probe needs IntlPreferences loaded on Mac to supply its
  NSLocale category. Its first isolated CLI run threw without that framework;
  corrected Mac and phone account-free probes complete, no requests sent.

## Suggested next work (no blind login loop)

1. Inspect latest -7006/underlying2 separately from older iCloud401; research exact
   meaning from native/primary implementation evidence, not generic dialog text.
2. Audit native AuthKit-result -> ACAccount credential -> iCloud authorization
   handoff and the requested application-token/client context. Start with static
   native methods and synthetic accounts. Any runtime diagnostic should expose
   presence/equality booleans only, keep all credential material on the phone.
   Do not export a working Mac account token/keychain/Safari session.
3. Verify requested DSID/routing semantics and service-specific metadata against
   actual native use. Do not reset/reprovision the working Hackintosh identity.
4. When a justified test reaches iCloud again, inspect passive v5
   account_failed_response_summary for numeric error details. If empty/unclassified,
   report that honestly; do not dump the whole response or assume bad password.
5. Final acceptance requires actual completed native account state/UI; App Store,
   iCloud sync, Find My and iMessage are separate subsequent tests.

## Rollback

Read current kern.exec_inject and remove ONLY the temporary account bridge/probe
rules, preserving unrelated workers' changes and Settings Metal-off. Close Settings
and restart affected native daemons to unload injected libraries. Do NOT restore
an old full rule snapshot over newer unrelated work. Stop only verified owned
broker/tunnel PIDs; remove private phone connection config only after unload.
Keep private provisioned state for diagnosis. No reboot required for these steps.

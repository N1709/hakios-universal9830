## CURRENT trial ready: genuine Store AMD pair added, helper2

- Coordinated capture confirmed actual native store POST /auth/v1/native includes SAP ActionSignature + AuthKit I-MD headers, lacks Store AMD pair, returnsHTTP200 with ZERO data; native verifiedaccount=nil. Previous gsas401 is postdata push-token rejection, not established Media-auth cause.
- Primary recent contributor implementation (merged ipatool issue642/PR643 references below) identifies +[AMSAnisette _headersForRequest:account:type:] accountnil,type2 for distinct X-Apple-AMD/X-Apple-AMD-M proofs. Native IntelMac15.8.1 method ABI @40@0:8@16@24q32 confirmed, actual native pair generation succeeds with no account/login/body input. This is a concrete missing field correction, not yet verified real sign-in fix.
- native-media-provider-v2 on Mac adds strict store-proof RPC: version/nonce/operation/endpoint ONLY, four exact HTTPS auth.itunes native paths. Returns ONLY genuine AMDpair. Metadatahelper8 unchanged. native-media-transport.py strict schema/output validation extended. Existing sign/prime/verify operations retained.
- Broker restarted exec44160, same pinned hotspot SSH/bearer/loopback49741; --helper hackintosh-provider-v8 --media-helper native-media-provider-v2. Private connection.json unchanged. No Mac logout needed. Checked Hackintosh native AppStore present; no mas/ipatool/ApplePackageTool in PATH/usual Brew dirs. Don't divert to setting up unrelated CLI or sign out working account.
- Store adapter /var/tmp/store-amd-bridge-v1.dylib configured accountsd amsaccountsd appstored itunesstored Preferences AppStore. Hooks native AMSURLSession _prepareRequest:properties:error: AFTER original preparation; exact HTTPS host/paths only; canonical endpoint request to helper WITHOUT query/body/account/password, adds AMDpair only. Original URL/query/body/SAP/I-MD/properties/errors retained. Native request/response parsing/TLS/verification unchanged. Missing provider fails closed NSError12+connection error, never fake success.
- Unsent synthetic phone test uses REAL Mac AMDpair: native body/URL/query/SAP/I-MD and properties/error identities preserved; original request unmutated; HTTP/spoofed host/unlisted path/out-of-scope iCloud refused. No login request sent. Unit schema checks extended for hostile endpoints/extra body/password fields. Existing media/capture tests pass.
- Only account/store four services restarted, UI apps closed; akd not restarted. Core shared9b41 and coordinated recorder unchanged. StoreAMDinstalledtrue markers in new services (accountsd12184, appstored12195, itunesstored12201; amsaccountsd see ps). Native auth health passes. Count1 AppleAccount still saved; media account still0 before real test.
- NEXT: rearm coordinated capture, one AppStore login; user reports loading immediately. Check store_amd_headers_attached body/SAP flags and wire request header NAMES for X-Apple-AMD/AMD-M, actual response body/application status, native verifiedaccount and store-account count. Do not claim download working until real download tested. Native auth path preserved, NOT rewritten /fast/. MacBook label remains borrowed coherentMacidentity limitation.

## Coordinated capture obtained: native auth EMPTY response; Store AMD clue

- Real run f1bc11dd0b09c8a122ec682c4d8a4f4e collected privately under ignored out/apple-account/private/capture-<run>, raw responses kept private; capture disarmed automatically. Monitor exec58053 completed.
- AuthKit request11869-2/3 /grandslam/GsService2 eachHTTP200, application Response.Status.ec0. Native continuation and complete credential flags present, updated account present.
- Store verification11874-1 POST https://auth.itunes.apple.com/auth/v1/native, body length1281 (NOT recorded), HTTP200 with ZERO received/captured response bytes, no contenttype. Shortly afterward finish_verified_account accountnil. This directly explains "Request finished without a result"; do not call it bad password.
- gsas401 is actually /grandslam/GsService2/postdata, responseec-800012/em "Push token is invalid." Same endpoint later during login returns200 empty. Previous focus on master-token service API/HTTP401 as Media cause was unsupported. The new capture made this distinction possible.
- Real store request headers include X-Apple-ActionSignature and I-MD/I-MD-M/LU/RINFO, but NO X-Apple-AMD or X-Apple-AMD-M. Recent primary contributor investigation and merged ipatool fix identify Store AMD type2 distinct from AuthKit anisette, native API +[AMSAnisette _headersForRequest:account:type:] accountnil/type2. Sources: https://github.com/majd/ipatool/issues/642 and https://github.com/majd/ipatool/pull/643 . Do not claim it proves empty response cause yet. Do NOT rewrite endpoint to /fast/ blindly; reports conflict, currentpath matches native bag.
- NEXT verify native Mac Store AMD generation without account/login, extend existing private media helper to supply only Store AMD headers for exact allowlisted native-auth endpoints, then native header adapter preserves body/SAP/other native headers. No new UI trial needed until full unsent test passes. MacBook label remains borrowedMacidentity issue, no label spoof.

## CURRENT: coordinated private response capture replaces fragmented tracing

User explicitly criticized repeated expensive UI trials, requested better root-level tools/logging. Do NOT add another isolated hook/ask for another trial for each observation. New coordinated recorder tested and installed; next action is ONE monitored AppStore trial, then analyze captured bytes offline.

Latest prior trial AppStore11764: native SRP success, authkit_delegate result13fields/native_continue=true, process_authkit_update accountpresent/result14fields, finish_verified_account accountnil. iTunesStore0, iCloud account1. Native app-token API observer never fired, session-isolation marker never fired even though gsas200/401 observed. Removed ineffective session-isolation rule and restarted akd. Static literal behind AMS0 reads "Unknown Error / Request finished without a result." No proven server rejection reason. Do not call gsas401 causal until correlated to actual request.

NEW tools:
- media-transaction-recorder.m: Foundation __NSCFLocalSessionTask _onqueue_resume, connection:didReceiveData:completion:, _onqueue_didFinishWithError: typed, originals unchanged. Opt-in capture.json with random32hex run and expiry<=900sec. Request IDs pid-counter, timestamps/duration, process, host/sanitized path (NO query), header NAMES only, request-body LENGTH only, real HTTP status/error/response metadata. No request body/credential values recorded.
- Actual decrypted RESPONSE bytes streamed unchanged to private files, <=2MB per response/8MB per process per run, truncation flagged; in-flight captured tasks can finish after arming deadline/disarm, bounds still enforced. Raw responses can contain account tokens; owner explicitly authorized private debugging. Files stay mode0600 under mobile-owned0700 directory /var/mobile/Library/RedwoodAppleAccountLab/private-capture/<run>. Never cat/raw-print/commit. Local collection in ignored out/apple-account/private/capture-<run>.
- Scope: gsa.apple.com,gsas.apple.com,auth.itunes.apple.com,buy.itunes.apple.com and *-buy.itunes.apple.com. No helper loopback traffic/image/catalog capture. This is in-process plaintext response data, NOT a claim of exact TLS wire headers.
- media-capture.py prepare/collect/disarm/watch --ssh-config privateconfig. Controller correlates events, privately collects response files, outputs fixed nested protocol error/status summaries only. Watch streams relevant request/native-flow events, stops3sec after finish_verified_account, auto-collect/disarm; otherwise expires. State private media-capture-active.json. Parent/run directories tested mobile-readable/writeable, raw artifact ignored bygit.
- media-auth-flow-probe.m now timestamped, compiled into combined recorder. Synthetic preservation test previously passed. Existing standalone oldflow rule REMOVED to prevent duplicate tracing. app-token-error-probe rule also REMOVED (unused). Core shared9b41/Mescal helper unchanged.
- session-data-metadata.m confirms exact Foundation callback ABI; media-static-strings.m reads ONLY two known constant NSString literals, never account memory. Metadata/static probes no auth calls.

LIVE addon: /var/tmp/media-transaction-recorder-v1.dylib akd accountsd appleaccountd amsaccountsd appstored itunesstored storeaccountd Preferences AppStore. Installed immutable tested bytes; six native services restarted once, UI apps closed to load recorder on next launch. installedtrue markers for akd11869/accountsd11874/appleaccountd11879/amsaccountsd11884/appstored11889/itunesstored11894. Native auth health passes. No account reset/reboot/credential substitution. Deployment manifest ignored media-recorder-deployment.json records replaced observers. Do not overwrite signed mapped binaries.

Tests: root AND mobile UID live localhost HTTP200/401 each160016bytes, native completion status/body unchanged, captured bytes match byte-for-byte, no truncation, request-body sentinel absent from metadata. Local-only fixture ports cleaned. test-media-capture.py checks nested Response.Status.ec plist/JSON, HTML/binary distinction, excludes token/account/message values. All pass. Fixture captures contain synthetic data only. Real capture not yet armed at doc write; prepare/watch next before user trial.

Commands from worktree:
python3 tools/apple-account/media-capture.py prepare --seconds 600 --ssh-config out/apple-account/private/account-hotspot-ssh.conf
python3 -u tools/apple-account/media-capture.py watch --ssh-config out/apple-account/private/account-hotspot-ssh.conf
Manual collection if watcher interrupted:
python3 tools/apple-account/media-capture.py collect --ssh-config out/apple-account/private/account-hotspot-ssh.conf
Raw files are private even after collection; inspect with code in memory and print ONLY redacted/numeric findings. Do not assert success from HTTP200; correlate application status and native store-account save.

## CURRENT control: isolate gsas session storage, trial pending

- Latest AppStore11708 flow: native AuthKit delegate result13fields, password/username/DSID/AltDSID keys present, native_continue=true; process_authkit_update result14fields/accountpresent; finish_verified_account accountnil -> media failure loop. Native SRP successful, real helper sign events. iTunesStore count0, iCloud account1 active/authenticatedflagfalse. No successful download.
- app-token-error-probe-v1 installed in akd11688 but DID NOT FIRE during latest gsasHTTP200/401 sequence. Thus previous theory that this exact ObjC app-token API was used is unproven/wrong for this path. Do not claim captured token-native rejection code. Static API inspection continues; no raw password substitution.
- Native AMSAuthKitUpdateResult builds updated account from AuthKit results via _createUpdatedAccount:usingAuthenticationResults:options:, original credential selection remains. media-static-methods-v4 enumerates instance methods; v5 enumerates class methods for UpdateResult. These are static metadata tools, no authentication.
- NEW narrow session-isolation control media-session-isolation.m: hooks __NSURLSessionLocal dataTaskWithRequest:completionHandler: ONLY https://gsas.apple.com in akd. Copies native configuration, clears cookie + URLCredentialStorage and automaticcookie flag, retains same delegate, delegateQueue, protocols/additionalheaders/other config. Original request and original completion/data/response/error retained; native server rejection/TLS/trust delegate untouched. Background configs fall back native. This is a hypothesis/control, NOT proven fix or evidence of cookies present.
- Immutable /var/tmp/media-session-isolation-v1.dylib akd added. Only akd restarted; native health passes. request-events marker media_session_isolation_installed and, when hit, media_verification_session_isolated with delegate_preserved bool. If no hit, this API wasn't used; do not infer effect. Existing shared9b41/Mescal private helper and other observers unchanged.
- Local-only phone transport fixture preserved synthetic additional header and raw HTTP200/401 status/body through real callback, no Authorization/Cookie or Apple/account request. Tests pass. Fixture reverseport49763 removed automatically. No reboot/account reset or credential modifications.
- NEXT one AppStore Get/signin trial, report loading immediately; inspect request-events marker, gsas status, flowflags, mediaaccountcount. If fails/noeffect, undo ONLY this addon rule rather than stack speculative patches. No further repeated trials merely to add diagnostics without decisive new evidence.

## Latest interactive trace: complete AuthKit result -> native verification -> nil account

- AppStore11653 actual trial: media_auth_flow process_authkit_update has account_present1, update_result1, dictionarytrue/fieldcount14, Username/Password/DSID/AltDSID keys allpresent. Thus credential result DOES reach native AMS.
- accountsd11540 helper sign=true, auth.itunes HTTP200 decodednil (do not equate cert response with store acceptance), then native AMS0. AppStore finish_verified_account account_present0/update_result1 -> AMS100; appstored receives IXUserPresentableError1/AMS100. iTunesStore account count stays0. No download verified. Native SRP successful. Correct media-account-presence-v2 read entitlements match account-health-v2.
- New app-token-error-probe.m observes actual akd AppleIDAuthSupportCopyAppTokensOptions ObjC boundary. Static native path _fetchTokensForServiceIDs -> AKSRPAuthenticationContextHelper -> native C token function. Records native NSError code chain, known numeric kAppleIDAuthSupportStatus, selected CPD/extra metadata equality flags only, result presence. No token/mastertoken/username/body logging or value changes.
- Synthetic app-token-error-smoke preserves mastertoken-pointer/url/serviceIDs/params identities, success result and failure NSError identity. Additive /var/tmp/app-token-error-probe-v1.dylib akd configured; only akd restarted to11688, CS_VALID/identity/internalAuthKit entitlement intact, health RPC PASS. Existing UI flow probe/sharedMescal remain.
- No actual app_token_service_result after new observer YET. Next interactive AppStore attempt needed for fresh context. Do NOT count earlier gsas401 as fully decoded cause. Client/userInfo stages still not diagnosed beyond nilverifiedaccount.
- Attempted noninteractive native silent-media-token-probe-v1 using existing account identifier, requested com.apple.gs.itunes.auth, shouldPreventInteractiveAuthYES and persistenttokenupdatesNO. Native context eligibility was FALSE, probe stopped before authenticate. Do not claim token rejection or use this as proof of actual live serviceID. No prompts, credentials read, token extraction or forced eligibility.
- User wants quick AppStore trial, can reply loading immediately. Native UI-auth result must be fresh; no repeat5min wait. No further Mescal alteration without evidence; real signature and invalid-signature rejection remain tested. Missing account remains unresolved. Runtime native broker55437/helper8/mediahelper1 over hotspot config unchanged.

## Latest AppStore loop: Mescal signing succeeds, no media account saved

- User AppStore Get/Install prompts login; enters credentials, spinner then returns with no error; Get repeats login. No successful download or Media sign-in.
- AppStore11586 media_native_helper_result sign=true observed in actual process. This alone may be analytics signing (xp/amp requests); do not claim actual authenticate endpoint passed Mescal yet. Native path no new AMS12hardware failure in recenttrace.
- Native AMS2 origin18ffd114c is optional password-reuse fallback: authType2 OR no ams_password OR nonnil serviceidentifier rejects reuse. It is NOT necessarily final failure. AMS7 origin18ffb28bc is active-media-account lookup absent. Other AMS204 are bag lookup failures. No explicit final server password rejection established.
- Correct count probe media-account-presence-v2 uses SAME read entitlements as account-health-v2: AppleAccount1/idms2/iTunesStore0. V1 incorrectly used AuthKit-only entitlement and read ALL counts0; discard v1 diagnostic. AppStore11586 and Preferences11587 codevalid637534209, native identities and internalAuthKit entitlement present.
- Added passive media-auth-flow-probe.m at three native boundaries: shouldContinue delegate (return preserved), _processAuthKitUpdateResult, _finishWithVerifiedAccount:andAuthKitUpdateResult:. Presence/count flags only; no secrets/account changes. Synthetic smoke preserves YES/NO, original args/object identities, callcount4.
- Fresh immutable /var/tmp/media-auth-flow-probe-v1.dylib rule ADDITIVE for AppStore Preferences accountsd amsaccountsd. ONLY UI apps closed, no daemon restart; pending UI relaunch loads trace. Existing account daemons not yet traced by new addon, don't claim they are. Shared Mescal9b41 remains unchanged. One user Get/login flow needed for trace, user can reply loading immediately; no requirement to wait UIerror. No claimed fix.
- Runtime broker exec55437 uses --media-helper native-media-provider-v1; original metadatahelper8/hotspotIPv6. Synthetic sign/prime/invalid-signature rejection and existing bridge smoke pass. Source changes up through 878506b3 pushed; new flow observer/count probe not yet committed.

## CURRENT: native Mac Mescal signing adapter ready for App Store trial

Media&Purchases/AppStore remain UNVERIFIED. The account was authenticated/active at continuation start. New shared layer installed, services restarted; latest account count1/active=true/warming=false/authenticated=false (previous restarts showed this transiently; do not assume recovery). Native akd11535 and accountsd11540 retain CS_VALID637534209, correct identity/entitlements, read-only auth RPC passes. No account reset or reboot.

Existing trace establishes Settings AND AppStore failure at native AMS12 branch "Mescal Failed / Failed to fetch hardware info"; user-owned Mac AMSMescalSession synthetic sign succeeds. Stock FairPlay binary unchanged; restored demand job repeatedly exited0 and was UNLOADED during prior investigation to stop wait/relaunch; no licensing patch. Do not re-register blindly.

NEW implementation:
- native-media-provider.m compiled immutable native-media-provider-v1 on Hackintosh, private stdio only. Uses real native default/prime MescalSession + IMURLBag, real sign/prime/verify/verify-prime promises. Verification failure never becomes success.
- native-media-transport.py manages persistent pinned SSH RPC, strict version/nonce/op/size schema, reconnects after closed transport, no payload/proof logging.
- native-provider-server.py optional --media-helper native-media-provider-v1 adds bearer-only loopback POST /v1/media. Owner explicitly authorized private signing assistance; signing bytes MAY include credential values, unlike narrow iCloud body schema. Data exists transiently over pinned SSH/memory only, never files/logs. No arbitrary URL/command/session selection. Max131072-byte input,30sec RPC.
- media-signing-bridge.m hooks ONLY four typed native AMSMescalSession methods (sign, prime, verify, verify-prime) in existing media process scope. Uses same private connection.json and fixed loopback endpoint. Real helper verification required; helper/native error fails closed. No forged success or dialog bypass, no raw input/error text logging.
- Shared artifact LIVE /var/jb/usr/lib/libredwood-account-layer-9b41e2a7df42db8b.dylib. All four hooks installed in store/account processes. Native iCloud adapters unchanged; unrelated injection/graphics/passcode preserved.

Tests: Mac synthetic sign RPC PASS; end-to-end PHONE sign+prime returns real proofs, malformed server and priming signatures rejected, empty input fails closed; combined shared library same smoke and existing bridge sync/async metadata smoke PASS. Unauthorized endpoint401, malformed/unknown operation400. test-native-media-transport.py schema PASS. Shared-rule preservation tests PASS. No real Media sign-in yet under new library.

Runtime: native broker tool exec55437, host user@hackintosh.example via private account-hotspot-ssh.conf, metadata helper8 + mediahelper1, same bearer and loopback49741/phone49742. Existing IPv6 tunnel supervisor unchanged. Old broker5867 stopped after exact command verification. To restart add --media-helper native-media-provider-v1 to previous broker command. Keep both Macs awake.

NEXT: user prefers AppStore login, can report "loading" immediately, no minute wait. No further synthetic investigation needed before ONE test. AppStore/Preferences were not running at last check, so next launch loads newlayer. Inspect media-error-events.log for media_native_helper_result operations/success and native error origins while loader runs; do not wait for5min UIerror before inspecting. Also recheck saved account health. Store downloads still unverified.

MacBook device label is expected from borrowed coherent Mac machine identity. No cosmetic iPhone model spoof applied. Correcting registration/device type requires a coherent supported provider, not changing clientinfo alone. User offered Mac Media logout; unnecessary for this synthetic/native helper test, asked them to keep signed in.

# S20 Apple Account: current handoff

## Latest runtime: shared992a945fac1b51d6; account authentication flag now false

User requested continued work, service restore and combined feature push. Branch
feat/s20-apple-account-gps published (initial commit1a3c0aab). FairPlay job remains
registered user501 with stock binary unchanged. Explicit/native demand starts
still exit0; runs5/spawnscheduled observed, so KeepAlive=false does NOT eliminate
relaunch from pending clients. Do not say service is functional or has no retries.
Native expected /usr/sbin path not tested on this read-only system volume.

Added bounded static-code backtraces ONLY to AMS-domain error observation: maximum
8 AppleMediaServices/Core/UI image offsets, no stack arguments/bytes, no arbitrary
image paths or descriptions. Native error factory/result/description preservation
smoke passed on phone. Shared992a installed via upgrade preserving other rules;
six services restarted. New background AMS7/1 origins captured; no new Media user
trial yet, so exact auth0/12 origin still pending. No licensing/auth success stubs.

IMPORTANT post-upgrade read-only health: AuthKit RPC still passes; account count1,
active=true, warming=false BUT authenticated=false on repeated checks. Before
upgrade it readtrue. Do not claim preserved authentication health merely because
account remains saved. Observer source only changes output metadata; cause of flag
change not established. No account deletion, reset, password entry or automated
login was performed. User told this state explicitly. Resume existing account,
not fresh sign-out. One user-controlled retry is now useful to capture exact AMS
error origin; user may report loader immediately, no required minute wait.

## CURRENT: original FairPlay job registered; integration branch

User requested adding the service and publishing combined AppleAccount/GPS progress.
Original byte-identical binary now installed under /var/jb/redwood/fairplay-stock/
<SHA256>/fairplayd.H2; original MachServices registered user501 from
/var/jb/Library/LaunchDaemons/com.apple.fairplayd.H2.plist. KeepAlive=false, native
licensing code unchanged. Job registered on demand with runs0 at health check;
startup ability remains unresolved after earlier runs1/exit0 test. System volume
is read-only; Program differs from /usr/sbin, no exact-path test or boot persistence
claimed. tools/apple-account/fairplay-service.py installs/status/rolls back only
its known job. Account1 authenticated/active/notwarming and AuthKit healthy after
registration. No new user login needed for these checks.

Branch feat/s20-apple-account-gps collects AppleAccount source/docs and eleven
GPS files copied from feat/s20-gps without modifying its worktree. Accepted GPS
commits were already in baseline9e0d7cbe. GPS real fixes verified in assisted lab;
native standalone CoreLocation not shipped. No proprietary binary/private out
included. Source media-static-methods.m reads static method addresses only;
current targets sanitizeError18ffd3d40, processAuthKit18ffd31a0,
performAuth18ffd2c8c, delegateAuth190029858. Native sanitize method only remaps
one -7003 error; generic media failure origin still needs tracing.

## Current: Media failure captured; stop repetitive UI trials

User confirmed current Media&Purchases error after long spinner and explicitly
objects to repeated 5–8 minute trials. Do NOT request another sign-in trial merely
to add a diagnostic. Use existing trace, static analysis, or credential-free native
service probes until a decisive hypothesis is ready.

Shared layer remains d78ff32a576b7c94. Captured accountsd10990 AMSErrorDomain12,
then auth.itunes.apple.com HTTP200 with decoded object nil, then AMSErrorDomain0;
Preferences11017 subsequently AMSErrorDomain100. No text/userInfo values recorded.
Keychain -25300/-25293 and LocalAuthentication -9/-10 occur nearby but may be
optional credential reuse; do not assert they caused failure. AMSError204 may be
optional bag lookup. No server password rejection established by these codes.

Read-only health AFTER latest investigation: account-health-v2 reports account1,
authenticated=true, active=true, warming=false; AuthKit RPC healthy. Native iCloud
account preserved. Media&Purchases remains unresolved.

FairPlay service investigation: live system/user501 job and /usr/sbin/fairplayd.H2
absent. Local extracted rootfs also omitted binary, BUT original rootfs.dmg contains
stock 19,316,144-byte binary. Extracted ONLY it to ignored private fairplay-stock;
SHA256 e3eab59230aea545ed6a46ec058fa73ac30345c3d1e7adda379116bf818b07bb.
New reproducible tools/apple-account/fairplay-startup-probe.py bootstraps original
native user501 job/mach services with KeepAlive=false and relocated immutable
stock binary, observes2seconds, then bootout in finally. Controlled result runs1,
state notrunning, exit0; cleanup verified job absent. No signing patch/re-sign,
no licensing response replacement, no account call or reboot. Task-private binary
and plist remain under /var/tmp/account-fairplay-e3eab59230aea545; no launch job
left registered. Relocated Program path differs from stock, so this is evidence
of startup failure, not proof of its cause. Matches earlier inject_fairplay.c
history of immediate native main return. Restoring daemon alone is NOT a fix.

Next: analyze native AMS error0/12 origin using existing framework metadata/static
code and account-free service behavior. Existing promise probe did not capture
actual failing promise; broad NSError observation only provides codes. Missing
FairPlay can explain old AppStore waits but causal link to current Media auth is
unproven. Do not invent a kernel driver from entitlement names or bypass native
FairPlay/account checks. Preserve current system-wide layer and saved account.

## CURRENT shared d78ff32a576b7c94: native error/renewal capture, user trial pending

Latest user941b trial failedMedia&Purchases. AMS decoded observer DID see both
auth.itunes HTTP200 completions but decoded object dictionary=false/error=nil;
it did not expose store business error. Initial AuthKit/SRP still success, gsas401
nearby. Do NOT label those opaque replies successful authentication or known
account-review failure. New native endpoint role/type fields will distinguish
cert/session/validation/store-auth known paths without logging arbitrary paths.

Added media-error-boundary.m to shared bundle: read-only NSError factory and
owned localizedDescription implementations across NSError subclasses in native
media process scope. Records bounded domain/code chains with 5sec dedup, <=256
keys and timestamps; never text/userInfo values other than underlying NSError.
Actual original factory result, input objects and description strings preserved;
on-phone smoke verifies these invariants. Added Accounts verify-options/renewal
callbacks to existing passive validation helpers (original arguments/results
preserved), q/Q scalar callback form supported/tested. All 2/3-object/BOOL/enum
callbacks and decoded-result identity/privacy smoke pass. No authentication
behavior change in this upgrade.

Installed shared immutable /var/jb/usr/lib/libredwood-account-layer-d78ff32a576b7c94.dylib
by targeted upgrade; native account still1/authenticated/active/notwarming, health
RPC passes. Error observer is working and emitting ordinary startup keychain/
contacts/background codes; these are NOT proof of Media failure. Private baseline
media-response-capture-baseline.json now identifies current hash and log count.
Read entries after current user attempt, including timestamp and process/role.

User ONE Media&Purchases Continue trial pending async request, password only on
phone. Do not restart services or change diagnostics during that pending attempt.
Inspect new media_native_error chain + native_store_renew/verify_options completion
and media_decoded_response endpoint_role/decoded_type. Older error/promise probes
remain passive. iCloud account must be preserved. Shared current-boot scope and
Mac-helper IPv6 dependency unchanged. Installer rollback retains original legacy
mapping, no reboot/account reset performed. Source/runtime documented in shared
layer doc; latest installed hash replaces941b, which replaced01fd.


## Current shared layer941b051de62277ec: decoded AMS response capture armed

User retried Media&Purchases with sharedlayer01fd; UI CouldNotSignIn persisted.
Native iCloud account remains count1/authenticatedtrue/activetrue/warmingfalse.
AuthKit completion succeeds; auth.itunes HTTP200; gsas401 nearby. Promise-only
observer installed but NO promise failure events. Do NOT claim its missing events
mean AMS success or a confirmed terms/password cause.

Added media-response-probe.m hooks native AMSURLSession
_completeTask:decodedObject:error: (v40@0:8@16@24@32); directly records bounded
NSError chain + media-response-summary.h fixed numeric fields/fixed categories.
Original task/decoded object/error are forwarded IDENTICALLY. Raw server text,
credentials, account/token values discarded from summaries. Summary/forwarding
on-phone synthetic tests pass; unsent NSURLSession task not resumed. Native promise
observer still preserves originalBOOL and error identity (test rerun passes).

Shared build now includes this module; artifact upgraded atomically to
/var/jb/usr/lib/libredwood-account-layer-941b051de62277ec.dylib. shared-layer.py now
supports upgrade preserving original legacy rollback mapping and unrelated rules;
prior state archived privately. Only one current shared rule. Six relevant
services restarted; no account reset/reboot. AuthKit health and native account flags
still good. New observer installed in accountsd10820/amsaccountsd10835/etc. Already
sees bag.itunes/sandbox.itunes HTTP200 decoded responses with no errors (startup
traffic, NOT proof user login uses sandbox or succeeds).

NEXT user ONE Media&Purchases Continue attempt pending async input. Read
media-error-events.log media_decoded_response entries after baseline line count in
out/private/media-response-capture-baseline.json. Actual auth.itunes response/error
will narrow remaining failure. Log includes only Apple hostname/other label, no
paths/request bodies. Current CLI helper/tunnelIPv6 unchanged. User was explicitly
told diagnostic not confirmed fix. Keep native iCloud account intact.


## CURRENT SHARED DEPLOYMENT — 2026-11-17

User requested system-wide account compatibility so other services can use the
working account path. Implemented and installed ONE versioned shared library:
/var/jb/usr/lib/libredwood-account-layer-01fd8405b33a759a.dylib across akd/accountsd/
appleaccountd/amsaccountsd/appstored/itunesstored/storeaccountd/Preferences/AppStore.
Replaced FOUR old account adapter rules (bridge14+16/public5/delegate2), preserving
all unrelated rules and older passive diagnostics. Source build-shared-layer.sh,
shared-layer.py install/status/rollback, shared-layer-scope.h. Full architecture/
limits/commands: docs/x1slte-apple-account-shared-layer.md. No boot persistence
added; runtime rule current boot, immutable dylib installed on disk. Mac helper
still required; private IPv6config/supervisor/broker unchanged.

Combined components: native bridge + initial-login public transport + delegate
fresh-metadata transport + passive Media promise error probe. Tested combined
native delivery/signing, real local200/401 callbacks for BOTH proxies, native
validation callback identities, media promise BOOL/error preservation. Installer
readback hash verified. Install/rollback preserves concurrent unrelated rules test
passes. Native code-valid/current identities for akd10587/accountsd10592/
amsaccountsd10603/appstored10609/itunesstored10614; AuthKit health passes. Native
account STILL count1/authenticatedtrue/activetrue/warmingUpfalse after rollout.
No account reset, auth-success fabrication, keybag changes or reboot.

User identifies current blocker specifically Settings→Media&Purchases→Continue,
then Signing into AppleMediaServices loader / Authentication failed. Earlier
AuthKit completions succeeded, auth.itunes.apple.com HTTP200, gsas401 nearby;
those do NOT identify application-level store error. oslog code-only stream saw
zero lines; no raw logs retained. Fresh media observer installed and records
NSError chains to /var/mobile/Library/RedwoodAppleAccountLab/media-error-events.log
in six media/client processes. User asked once via async request to open
Media&Purchases Continue, enter secrets on phone and leave result open. NEXT read
media-error-events/network-events and original account health. Do not repeat
signout/iCloud setup; separate Media account remains unverified.

Active installer private state out/apple-account/private/shared-layer-active.json
contains exact removed rules + installed rule. Rollback uses targeted removal /
restore, not entire old sysctl snapshot. CLI explicit rw_auth_lab_install remains
for fixtures; bundle constructors guard explicit native process set. Source
changes outside shared compile retain original constructor scopes. Delegate fixture
export renamed rw_delegate_session_fixture_proxy to avoid duplicate exported symbol.


## CURRENT VERIFIED SUCCESS: native account authenticated/active, setup completed

After hotspot IPv6 recovery, background delegate root/services0 AND background
AALoginAccountRequest HTTP200/rootstatus0 were captured in accountsd10415. User
reports UpdateAppleAccountSettings banner disappeared. Read-only account-health-v2
with SAME read entitlements as existing account-presence confirms count1,
isAuthenticated=true, isActive=true, isWarmingUp=false. No credential getter or
account identifiers read. Native-account setup now completed; no auth success
stub or forced boolean was used. Full server/native verification remained intact.
Evidence native-account-healthy-state.json in ignored private out. New source
account-health.m reads ONLY counts/boolean getters after runtime method ABI checks.

Important test-permission caveat: first account-health-v1 signed only AuthKit
permission reported count0; that was insufficient account read permission, not
account removal. Existing account-presence still returned1. Re-signed immutable
v2 with out/private/account-presence.entitlements, then read actual native state
above. Never infer account disappeared from an unentitled CLI query.

Remaining: USER VERIFICATION of individual iCloud/device-list/FindMy/AppStore
pages after outage. Core AppleAccount login now demonstrated, but do not claim
all Apple services work or that helper-free standalone mode exists. AppStore
purchase account/iMessage/FindMy may require separate work/validation. Asked user
to open account page and verify iCloud details/device list; no password needed
while banner absent. Current helper still required live, pinned IPv6/tunnels as
next section. Account not removed, reset or recreated after successful preflight.


## Latest healthy state: resume existing Update Apple Account Settings

Hotspot recovery COMPLETE: fresh machine endpoint passes without any recent login
snapshot; native phone sync/async bridge passed; credential-free phone HTTPS to
https://gsa.apple.com returned real404 in2.05sec (expected nonexistent root path),
so general Apple reachability works. DNS returns NAT64+IPv4. Current processes
akd10453/accountsd10415/Preferences10449 all have valid identity/code flags and
internal AuthKit entitlement; read-only native health RPC passes. Latest logs
4099/-7005/-1001 predate recovery; no fresh failed user trial after recovery yet.
No further daemon restart/account changes made. Async user request now pending:
Settings→Update AppleAccountSettings→Continue, password on phone only. EXISTING
account count1 preserved; no signout/remove/recreate needed. Follow callback/error
trace after next attempt; FindMy/AppStore/device list are not proven functional.

Use PRIVATE SSH -F account-hotspot-ssh.conf for all phone inspection now, NOT
bare ssh s20/USB. Supervisor5862 exec44639, broker5867 exec93788. Bothhost keys
remain original pins; actual stable .local helper hostname resolves IPv6. Next
agent should not undo original global SSH config (it was never changed).


## Hotspot recovery 2026-11-17: pinned IPv6 helper connections restored

User confirmed Wi-Fi dropped; all three devices now on hotspot. Host en0 has
IPv6 + NAT64/clat46 and translated192.0.0.2/32. That IPv4 is NOT a peer identifier.
Discovered Bonjour SSH service, verified two IPv6 peers against existing keys:
Hackintosh <link-local IPv6>%en0, stable Bonjour <hackintosh>.local;
S20 <link-local IPv6>%en0. Both match original pinned host keys. A candidate
rejected by Hackintosh key was then separately confirmed against S20's own pin;
no keys changed/host-check disabling. USB10.77.0.2 currently not reachable.

Private account-hotspot-ssh.conf defines s20 scoped IPv6 with HostKeyAlias10.77.0.2,
account-hackintosh BonjourIPv6 with HostKeyAlias192.0.2.10, then Host* Include
original ~/.ssh/config to preserve root/key/known-host options. Original global
SSH config is unchanged. IMPORTANT use -F out/apple-account/private/account-hotspot-ssh.conf
for phone commands now. Raw HostName IPv6scope needs %%en0 in SSH config (expands
once), command-line destination direct user@IPv6 uses normal %en0.

Broker/USB-supervisor now accept optional --ssh-config, defaults unchanged. Fixed
new config ordering before first broker child spawn. NativeTransport via pinned
IPv6 supplies fresh iCloud metadata; phone AuthKit health and sync/async native
bridge smoke pass. Accountcount1 still confirmed. Supervisor PID5862 exec44639;
broker PID5867 exec93788, hostuser@hackintosh.example, helperv8, same privateports.
Stop old2856/95489 only (verified owned). No phone account/daemon/rule change in
this recovery. Real direct phone public-HTTPS test pending; don't request passwords
until general Apple connectivity also confirmed. Current phone DNS returns NAT64
and IPv4 addresses; direct request is credential-free.

Commands:
python3 -u tools/apple-account/phone-helper-tunnel.py --ssh-config out/apple-account/private/account-hotspot-ssh.conf
python3 -u tools/apple-account/native-provider-server.py --host user@hackintosh.example --ssh-config out/apple-account/private/account-hotspot-ssh.conf --helper hackintosh-provider-v8 --key out/apple-account/private/hackintosh_ed25519 --known-hosts out/apple-account/private/hackintosh-known-hosts --token-file out/apple-account/private/broker-token


## Latest 2026-11-17: name shown; update prompt returned amid network/helper outage

User saw name/AppleAccount in Settings, UpdateAppleAccountSettings disappeared,
then returned and requested password. User entered it locally; FindMy/device list/
AppStore mostly spinners. Last successful read-only probe: AppleAccountcount1,
IDMScount2. Do not call this fully functional iCloud/AppStore sign-in.

Actual latest logs: Settings10429/akd10425 native Apple requests timeout-1001,
AuthKit-7005; bridge broker timeout/HTTP503 repeated. Private broker PID2856 still
listens, but pinned Hackintosh114 SSH fails (one connect error AddressAlreadyInUse;
TCP check failed). Host HTTPS to gsa.apple.com reached404, so internet is not
uniformly down. Host en0 currently192.0.0.2/32, route toHackintosh114 via default
192.0.0.1 rather than old137 local subnet. Phone last net_test en0 also192.0.0.2/32.
Do not infer exact hotspot/VPN cause from these observations.

USB initially worked, then SSH10.77.0.2 timed out while host en5 link active/IP
10.77.0.1 still present. Supervisor remains running. Therefore current blocker is
loss of usable helper/network reachability, NOT evidence of new password rejection.
No additional account mutations attempted after failures; no reboot, account
removal or credential reset. User should reconnectUSB and restore same reachable
LAN for host/Hackintosh before more password attempts. Experimental auth still
requires live Mac helper. Recheck pinnedSSH, fresh metadata endpoint, phone bridge,
accountstate then resume EXISTING account only. No new code edits since fresh
metadata/helper8, delegate2, public5, bridge16 deployment.


## Current 2026-11-17: account stored/CDP success; final refresh correction loaded

Actual Settings10328 with delegatev1: background delegate root/services0,
preflight save SUCCESS, native AppleAccount count1 (previously0); later CDP
completion SUCCESS. User saw stock merge contacts/stocks prompt, then "error
while trying to save iCloud account". Final save completion had three-argument
ABI unsupported by earlier diagnostic, so no original final error captured.
During long CDP/UI stage >90sec, delegate adapter's recent-snapshot dependency
expired, causing identity_unavailable/native fallback/status1. Background
AALoginAccountRequest separately HTTP400 without proof metadata. Do not call
setup complete just because account now stored. No account removed/reset.

Fixed fresh metadata path: helper-v8 accepts EXACT version/nonce/service=icloud
RPC with no credential or body; existing simple/scoped schemas unchanged. Broker
GET /v1/fresh-icloud-metadata bearer-only returns fresh issued/expires + eight
machine metadata fields, no body or NAS/User-Agent. Existing sign/recent comparison
endpoints unchanged. Independent native service-context/same identity tests pass;
unknown service refused. HTTP unauthorized GET401, authenticated fresh metadata
shape/freshness test passes. New broker live PID2856 exec1681, helper explicitv8;
USB supervisor unchanged. Old broker95609 stopped by verified ownership.

Delegate adapter v2 now uses fresh endpoint, <=25sec proof age+future expiry,
no dependence on earlier login snapshot. Credential-bearing body never leaves
phone except native HTTPS request to Apple; no wrong-body NAS copied.

Preferences public session now v5, expanded to accountsd for native
AALoginAccountRequest, original scope/parser/callback/checks retained. Native flow
completion observers support checked BOOL/object/error and three-object forms;
synthetic identity/exact-once callback tests pass. Accounts-only initial login
path requires bridge too: added bridge16 Preferences/accountsd, whose initialization
explicitly loads AppleAccount framework BEFORE class lookup. Startuporder bug
previously installed generic bridge without account headers. Verified actual new
accountsd10415 logs icloud_request_adapter_installed/native_signing_adapter_installed,
bridge_installed/public_session_transport_installed. Initial unsent native fixture
still passes exact-body signature/header checks. Original akd bridge14 untouched.

LIVE relevant rules: bridge16 Preferences accountsd; public-session-v5 Preferences
accountsd; icloud-delegate-session-v2 accountsd; daemon-validation-probe-v1 accountsd;
previous passive9 Preferences/passive7 daemons/SRP3/PETobserver2 remain, comparison
config disarmed. Only Accounts daemon restarted; Settings closed to reload. Account
count remains1. No native validation/CDP checks bypassed, no account deletion.
Next: user resumes EXISTING account's Continue/Update/finish prompt from Settings;
do not instruct signout/recreation. Async UI-state question pending. User secrets
phone-only. Read full final save callback/error and background refreshed statuses;
new long-duration path has NOT been proven by user yet.


## Current: delegate background status1 identified; scoped delegate correction ready

Actual Settings10300 v4 initial root/services0, first native validation success;
preflightsave com.apple.appleaccount1. Background accountsd10296 probe identifies
AALoginDelegatesRequest HTTP200/application rootstatus1/no delegates. Thus original
save error is a real server rejection in the separate background delegate request.
No local permission or cryptographic cause established. Evidence
background-delegate-status1.jsonl in ignored private out.

Added icloud-delegate-session.m v1 accountsd-only. Hooks original AARequest
performRequestWithSession for AALoginDelegatesRequest only, passes sessionproxy;
original request builder/parser/callback preserved. Setup HTTPS requests use fresh
public ephemeral session + selected recent iCloud machine metadata from bearer-
authorized loopback recent-signing endpoint, <=90sec old. Native credential-bearing
request body/authorization are copied unchanged, never sent to helper or logged.
Prior body's NAS signature explicitly removed from fetched header dictionary;
it must NOT be applied to credential body. Both legacy MD/I-MD pairs aligned.
If metadata unavailable/nonmatching scope, original native session fallback.

Real loopback200/401 through native AARequest and this proxy passed callback/
status identity checks, no account/Apple request. Selected metadata alignment,
request/body/auth preservation unit contract passes. New immutable artifact
icloud-delegate-session-v1.dylib accountsd appended; passive daemonprobev1 and
Preferences publictransportv4 remain. Settings closed; only accountsd restarted.
No native verification/save success overridden. Next user trial read background
application status, preflightsave completion, subsequent CDP/account-save states.
Privatehelper source/provisioned state unchanged. Rollback only delegate rule and
restartaccountsd, preserve all older auth/rendering rules.


## Current: original preflight error com.apple.appleaccount1; daemon capture ready

Real Settings10273 v4: root/three service statuses0 and first native validation
success. native_preflight_save_completed successfalse/error com.apple.appleaccount1;
outer completion wraps AASignInErrors-8004 with no original underlying NSError.
Background accountsd7769 made setup.icloud.com requests HTTP200 during failed save,
with selected proof headers absent in generic network metadata. Their application
response/class was not captured (original passive probe loaded before framework).
Do not assert background validation uses wrong credential/machine yet.

Added passive daemon-validation-probe.m accountsd-only, fresh v1 artifact. Explicit
AppleAccount framework load then hooks AARequest native data/response/error
completion solely to emit request class, HTTP status and fixed application summary.
No request bodies/credentials/response tokens/raw text saved. Original objects
forwarded unchanged. Synthetic native-method-slot smoke preserves identity on
success/failure; no account or network request by test.

LIVE appended daemon-validation-probe-v1.dylib accountsd, preserved existing
rules/publictransportv4. Settings closed; only accountsd restarted (newpid10296),
background_validation_probe_installed=true; AuthKit health RPC passes. Next user
attempt will capture daemon-validation-events.log while native preflight error
remains in status.log. Existing private helper and signing daemons unchanged.
Evidence preflight-original-appleaccount-error1.txt in ignored private out.


## Current v4: -8004 maps preliminary save failure; original error pending

Real Settings10239 v3: server root/three services status0, first native validation
success, entered _validateAndEnrollCDPState..., completion BOOLfalse/error-8004.
Domain hidden by prior allowlist (other); cannot call it CDPErrorDomain yet.
STATIC proof: native CDP wrapper first invokes ACAccountStore
saveAccount:withCompletionHandler: (not saveVerifiedAccount). Its callback at
19edbce98 emits "iCDP preflighting failed because we could not save the iCloud
account" and constructs generic _aaErrorForErrorCode:-8004 (19edbd0ec), discarding
original save error. Thus this is a failed preliminary account save; it does NOT
establish cryptographic CDP enrollment failed. Original underlying error needed.
Settings10239 and accountsd7769 code validity/identity/entitlement probes pass.

V4 fresh immutable dylib now configured Preferences only, same public transport,
adds original saveAccount:withCompletionHandler: completion observer and permits
AA/CDP error-domain prefixes in fixed error-code output. Native method ABI and
callback wrappers remain checked; smoke passes. No account saves/check bypasses
introduced by tools; observer forwards native call and result unchanged. Settings
closed for reload, native daemons left running. Next manual trial capture
native_preflight_save_completed original NSError domain/code before -8004 wrapper.
Sanitized v3 evidence local-preflight-save-wrapper-error.txt, static analyses
native-cdp-validation-analysis.txt and native-cdp-preflight-callback.txt in private out.


## Latest confirmed: server service statuses0; later local failure not yet isolated

Actual Settings10214 v2: HTTP200, parsed application root status0 and ALL three
service statuses0; native_account_validation success=true/errors[]. User still
reports unable to validate account. Therefore early server401 and first native
validation failure are no longer current blockers. No full saved account yet.
Root account_info_present=false and direct tokens_field_present=false are only
schema-presence observations; do not infer missing tokens (may be nested).
Saved sanitized status lines settings-server-success-local-validation.txt.

Static AASignInFlowController verification at19edbc674 confirms successful branch
setsAuthenticated:NO if password available, then validates/enrolls CDP when queued
context exists, otherwise saves account. These are further REAL checks, not a
reason to bypass them. v3 now configured (fresh immutable dylib) Preferences only,
same public transport, adding passive completion wrappers for CDP validation,
flow save, Accounts verifyCredentials and saveVerifiedAccount. Runtime callback
ABI inspected before wrapping BOOL/error, object/error or one-object callbacks;
unsupported signatures forwarded unchanged. Callback values/identities preserved.
validation-observer-smoke-v2 passed original verify/alert forwarding plus all
three supported completion forms with synthetic objects/errors; no real account
operation. Need next user attempt to identify which later completion fails.

Read native_cdp_validation_entered/completed, native_account_save_entered/completed,
native_store_verify_entered/completed, native_store_save_entered/completed in
status.log. Logs contain only presence/success and NSError domain/code chains.
No altered auth/check decisions, no raw credentials/proofs/tokens logged. Settings
closed for reload; akd/accountsd unchanged; broker and original adapters preserved.


## Latest: native Settings now HTTP200, account validation still fails

User reports "Could not sign in / Unable to validate account" with public-session
v1. Settings pid10177: AuthKit completes without error, proxy installed and task
returns HTTP200; native AALoginAccountRequest completion HTTP200. Selected proof/
identity comparisons pass. Therefore native Settings401 no longer occurs for this
trial. No completed account state/sync demonstrated. Application-level response
status and native validation NSError were not captured by older probes, so do not
infer full server success or precise later failure yet. Sanitized real trace:
settings-public-session-http200-validation-failure.json in ignored private out.

public-session v2 now configured Preferences only, replacing v1 at a NEW immutable
path; Settings closed, no akd/accountsd restart or account/keychain change. Same
transport as v1. Added fixed status-only icloud-response-summary for response;
passive AASignInFlowController validation/alert observers record success flag and
NSError domain/code chain only. No token/account values or raw server text.
validation-observer-smoke-v1 verified native method ABI and identity-preserving
forwarding for synthetic success/failure and both alert/verify methods, no account
operation. Current pending user trial gathers these details; no validation bypass.

Read status.log public_session_application_summary, native_account_validation,
native_validation_alert after next attempt. Note structured summary lines mixed
with older event/code lines. Runtime owner codex-apple-account-resume. All earlier
adapters and private helper unchanged, comparison forwarding remains disarmed.


## Current trial: public transport inside native Settings flow

Latest user attempt: Settings10139 AuthKit succeeds; native iCloud request401.
The one-shot PHONE public-session comparison completed HTTP200 (old observer field
mac_http_status; target was phone worker11). Comparison server exited. Its old
exec output handle is unavailable in this chat, so service-level status0 for this
PHONE response was NOT recovered. Earlier Mac application statuses0 remain valid.
Do not claim this new HTTP200 alone establishes successful account creation.

Added icloud-public-session.m, immutable phone icloud-public-session-v1.dylib,
Preferences only, gated by private public-session-enabled marker. It substitutes
a public ephemeral NSURLSession proxy ONLY for AALoginAccountRequest. Original
AARequest performRequestWithSession:withHandler: still constructs the request,
stores its native callback, resumes task and calls the original native response
parser/completion. Non-login/nonmatching requests fall back to original session;
missing body/signature also falls back. Standard NSURLSession TLS checks remain.
No account success, error or parsed response is fabricated. Existing bridge15
still supplies selected machine headers/body-bound proof.

Static block19ed673a8 verifies original AARequest invokes
session.dataTaskWithRequest:completion: then resume. Block19ed7cae0 maps
(data,response,error) unchanged into native _handleDataTaskCompletionWithData.
public-session-smoke.m on phone passed REAL local HTTP200/401 through native
AARequest parser and callback(request,response,error), with identity/status checks.
No account supplied/Apple request sent by fixture; its temporary tunnel removed.

LIVE: appended /var/tmp/icloud-public-session-v1.dylib Preferences to existing
rules; Settings closed for reload. Did not restart akd or alter other injections.
Completed native-comparison.json removed to disarm credential forwarding. User
must open Settings manually and try once. Read public_session_transport_installed,
public_session_login_task_created/completed and native account state/UI. This is
an unverified transport trial, NOT a demonstrated native sign-in fix.
Rollback only new rule+marker, close/reopen Settings; preserve all older rules.
Session codex-apple-account-resume owns phone. Latest trace evidence is ignored
out/apple-account/private/phone-public-comparison-native-401.json.


## Resume check — 2026-11-16 23:37 IST

Resumed as codex-apple-account-resume; stale previous phone lock transferred via
normal acquire. Phone and pinned Hackintosh SSH reachable. AuthKit health RPC
passes, comparison loopback endpoint rejects unauthorized request as expected.
Machine broker PID95609, phone tunnel supervisor95489, PHONE comparison96866
are still live. Latest network logs remain the prior pid10105 iCloud401; no new
phone public-session result yet. One user-entered diagnostic attempt requested.
The prior exec74734 handle is unavailable in this chat; retain phone result logs
and arrange durable sanitized application-summary capture on the NEXT comparison
restart if needed. Do not restart a comparison while a user attempt is pending.
This note supersedes the old statement that the phone lock is released below.


Native S20 iCloud sign-in is still NOT working. User requested handoff to a new session. Worktree: `~/ios-port-research/work/hackios-apple-account`, branch `feat/s20-apple-account`. The user's default cwd is the unrelated `hackios-cellular` worktree; do not edit its GPS/cellular work. Most account tools/docs are untracked; never clean them. No commits made during this continuation.

## Read first: what is established

1. Native AuthKit authentication repeatedly succeeds and returns its normal result. Binary analysis identifies PasswordKey as the native `com.apple.gs.idms.pet` password-equivalent token (PET), not the raw password. Mac native flow also uses PET. Do not substitute raw password.
2. Phone then gets HTTP401 from `AALoginAccountRequest`, `/setup/signin/v2/login`, with native `com.apple.appleaccount:401`. Ephemeral keyed comparisons confirm outgoing Basic auth matches AuthKit username/PET and alternate identifier matches. Selected machine headers, legacy MD pair and validation signature are present/aligned; no signing-error header.
3. A private one-shot comparison AFTER phone401 uses that fresh phone-issued credential on the user-owned Hackintosh. It obtains HTTP200. Subsequent controls also got200 with all three requested services, the exact phone body, and the latest helper proof snapshot (not a wire capture).
4. Latest completed worker10 comparison confirmed application root `status=0` AND `status=0` for `com.apple.mobileme`, `com.apple.gamecenter`, `com.apple.private.ids`. This goes beyond HTTP-only success. No account was saved. Bodies/tokens are not retained.
5. Remaining difference is phone/session/request handling or fields not covered by comparisons. Do not call requests identical or root cause proven: exact outgoing wire headers are not captured; successful comparison reconstructs native Basic auth/account request and uses latest broker snapshot.
6. Separate stalled/no-popup attempts coincide with missing USB helper forwards, broker-1004 and AuthKit client `NSCocoaErrorDomain4097`; akd sometimes restarted. These are NOT new password rejections. Reconnection restored native health. No crash file established. Do not conflate with earlier code-signature bug.

## Immediate next action: pending PHONE public-session comparison

A NEW one-shot is ARMED but has not yet been attempted. Last completed Settings PID10105, akd10074; final completed comparison remains Macworker10 applicationstatus0.

Next worker is `/var/tmp/native-pet-comparison-v11` ON THE PHONE, as mobile, using fresh public ephemeral NSURLSession instead of Apple's AAURLSession. Native request builder, exact schema-validated phone body, supplied helper proof snapshot and real server response handling remain. It does not replace Settings callbacks, forge success, save an account or change injected phone code. This is a standalone comparison AFTER native phone401, not a fix.

- Phone --build-test passed synthetic request construction. machine_metadata_available=false is expected because iOS has no AOSKit; supplied snapshot overrides it before transport.
- Phone --snapshot-test passed synthetic account tuple + account-free phone body/proof: snapshot present, signature preserved, no request sent.
- Local comparison service `--phone-worker` targets phone worker11. IMPORTANT: old daemon observer logs its result under **mac_http_status even for this PHONE test**. Local service `worker_target=phone` is authoritative.
- If public phone session/applicationstatus0 succeeds, focus on native AAURLSession/AAURLProtocol augmentation, cookie/credential storage/session authentication behavior; do not yet replace Settings transport without response/callback ABI verification.
- If public phone session401, investigate remaining rebuilt header/transport/network differences vs successful Mac. If status0/noresponse, investigate local worker failure; do not call that server rejection.
- If early auth failure/no tuple or connection loss, this comparison did not run. Recover connectivity/health before asking another retry.

Ask user for ONE attempt only after checking connection and service readiness, then inspect both logs and local application summary. They are willing to retry; avoid repeating without new evidence. No Mac logout needed. Existing PETs/responses were intentionally discarded; cannot reuse an old token from disk.

## Live components and access

Run commands from the account worktree. Default `ssh s20` is pinned USB10.77.0.2. Old Wi-Fi100 and106 are unreliable; don't hardcode them. Hackintosh `user@hackintosh.example` has working native account. SSH flags:

```sh
ssh -i out/apple-account/private/hackintosh_ed25519 -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=out/apple-account/private/hackintosh-known-hosts -o ConnectTimeout=4 user@hackintosh.example 'echo reachable'
```

No passwords/code in commands. User authorized transient owner PET forwarding and deep debugging, but no need to log raw secrets. Never print private bearer configs, --stdio helper output, proofs, passwords, keychains or token values. Mac account preference identity matched user account earlier. Do not repeat credential typing tests or extract stored credentials.

| Component | Current |
| --- | --- |
| Mac native helper | `~/.local/share/redwood-account-lab/hackintosh-provider-v7` |
| Machine broker | loopback49741, `native-provider-server.py`, exec22104, last PID95609 |
| USB supervisor | `phone-helper-tunnel.py`, exec1829; reverse phone49742→local49741 and phone49752→local49751 |
| Comparison | loopback49751, exec74734, last PID96866, worker11 **phone** target |
| Settings adapters | bridge15, passive9 |
| Native daemon adapters | bridge14 akd, passive7 akd/accountsd/appleaccountd, SRP3 akd, PET observer2 akd |

Tool exec IDs are historical session handles, not durable process identities. Verify listeners/commands before stopping anything. `out/apple-account/private/pet-comparison-runtime.json` has current IDs. Check config presence without printing contents.

Native broker helper7 pairs AOSUtilities OTP with base64(UTF8(machineUDID)) LU (AltStore AOSKit pattern), checks deviceID coherence, separates AuthKit/iCloud metadata, signs exact allowlisted body. Broker now respawns dead helper SSH on next fetch; failed warmup does not kill HTTP service. A local synthetic subprocess test verified fetch→closed child→fresh fetch recovery. USB supervisor and comparison log watcher reconnect automatically.

Network periodically loses reachability despite machines awake. USB replug restores phone. Hackintosh114 sometimes fails ARP/TCP and later recovers; verify from this machine. Do not diagnose passwords during transport failure. No reboot performed or authorized.

## Safe checks

```sh
ssh -o ConnectTimeout=4 s20 '/var/tmp/auth-service-health; /var/tmp/comparison-transport-health'
ssh s20 '/var/jb/usr/bin/su mobile -c "/var/tmp/bridge-smoke-v4 /var/tmp/anisette-lab-bridge-v15.dylib"'
ssh s20 'tail -12 /var/mobile/Library/RedwoodAppleAccountLab/network-events.log; tail -6 /var/mobile/Library/RedwoodAppleAccountLab/pet-comparison-events.log; tail -10 /var/mobile/Library/RedwoodAppleAccountLab/status.log'
lsof -nP -iTCP:49741 -iTCP:49751 -sTCP:LISTEN
```

`/var/tmp/code-signature-health-v3 PID` reads flags/identity/selected entitlement only. Correct csops identity operation is11, not6. Expected akd CS_VALID bit, identitycom.apple.akd, internal AuthKit entitlement; native health RPC passes. Don't dump entitlement blobs.

## Restart commands if needed

First inspect exact owner/command and stop only task-owned stale instance. Do not start duplicate listeners. Keep both Macs awake.

```sh
python3 -u tools/apple-account/phone-helper-tunnel.py
python3 -u tools/apple-account/native-provider-server.py --host user@hackintosh.example --helper hackintosh-provider-v7 --key out/apple-account/private/hackintosh_ed25519 --known-hosts out/apple-account/private/hackintosh-known-hosts --token-file out/apple-account/private/broker-token
python3 -u tools/apple-account/pet-comparison-server.py --host user@hackintosh.example --key out/apple-account/private/hackintosh_ed25519 --known-hosts out/apple-account/private/hackintosh-known-hosts --token-file out/apple-account/private/pet-comparison-token --worker native-pet-comparison-v11 --phone-worker --phone-body-token-file out/apple-account/private/broker-token --wait-for-phone-failure
```

Long-running services should have independent tool sessions. Comparison expires after30min or one valid attempt. It waits native401 (up to45sec) before worker, tuple age<=60sec; callback references expire120sec. To rearm, privately transmit JSON over SSH stdin to `/var/mobile/Library/RedwoodAppleAccountLab/native-comparison.json`, mode0600 owner501:501: fields enabled=true, port49752, token=contents of private pet-comparison-token. NEVER print the JSON/token. Existing config currently present/armed. Do not auto-repeat account requests.

Recent-signing endpoint in machine broker is bearer-only loopback GET `/v1/recent-signing-metadata`: last validated nonsecret body + helper proof snapshot in memory, age90sec maximum. Comparison additionally checks body timestamp>=credential issuance-2sec. Both Python and Objective-C strict schemas reject arbitrary/credential body fields. No on-disk request/token recording.

## Sources and validation

- `native-pet-comparison.m`: current crossplatform source, v10 Mac/v11 phone. --build-test and --snapshot-test send nothing; --loopback-test sends unauthenticated fixture only; --compare-once sends one actual setup request from private stdin, never saves account.
- `icloud-response-summary.h`, `test-icloud-response-summary.m`: fixed numeric status codes and known delegate flags only. Python independently sanitizes before logging. Successful responses may contain tokens; values discarded.
- `pet-comparison-server.py`, `pet-comparison-trigger.h`, `daemon-pet-comparison.m`: one-shot private comparison. Observer getter always returns original object. No auth/access-check bypass.
- `icloud-signing-metadata.h`, `test-icloud-signing-metadata.m`: narrow body schema; no credentials/tokens.
- `anisette-lab-bridge.m`, `icloud-request-identity.h`: existing selected metadata/signature adaptation. Do not blindly broaden.
- `docs/APPLE-ACCOUNT-BINARY-NOTES.md`: native credential provenance and static analysis.
- [Detailed continuation history](APPLE-ACCOUNT-CONTINUATION-HISTORY-20261116.md) preserves experiments, false starts and addresses. Older research history remains separate.

Compile phone standalone CLI with `clang -target arm64-apple-ios16.5 -isysroot ~/theos/sdks/iPhoneOS16.5.sdk -fobjc-arc -fblocks -Wno-deprecated-declarations -framework Foundation -framework Accounts -Wl,-no_fixup_chains ...`, then `ldid -Stools/apple-account/anisette-fetch-ios.entitlements ...`. Mac uses x86_64-apple-macos13. Use a NEW immutable artifact filename for every changed binary.

## Critical traps and preservation

- NEVER overwrite an already-tested/mapped signed phone binary. Earlier same-path overwrite coincided with lost CS_VALID/identity/entitlements and early auth failures. Fresh immutable filename recovered validity. Do not force validity or bypass checks. Source is tracked/untracked tools, not only out/.
- Preserve complete passcode/keybag chain, default boot profile, native account checks and all unrelated injection rules. Ask before reboot. No kernel changes made here. Preserve Settings-only metaloff and existing PosterBoard workaround.
- Existing injection additions: bridge14 akd, bridge15 Preferences; SRP3 akd; passive7 akd/accountsd/appleaccountd; passive9 Preferences; daemon PET observer2 akd. Don't reinstall failed passive8/10 or old worker1 callback ABI.
- Previous worker1 HTTPstatus0 readout was WRONG ABI/inconclusive. Native AARequest completion is request,response,error. Public NSURLSession callback used by current workers avoids that mistake.
- Synthetic PID8291 servercode-20101/resync=true was a test fixture, not real account rejection.
- No native success stubs, no credential substitution, no reset/provisioning wipe, no account saves through tools.
- Rollback observer/comparison: remove only private native-comparison.json to disarm; stop verified owned comparison listener. Full adapter unload requires read/modify/write only the known account rules, preserving every unrelated rule, and native daemon restart; do not reset whole injection sysctl. No need to unload for handoff.

Phone lock is being released for the next session. Consult workspace NEXT.md/TASKS.md and acquire under your own name before device changes. Services are intentionally left running; user-facing next action remains a single pending trial, not a claimed fix.

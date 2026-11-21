# Validation record — 2026-11-14

Worktree: `work/hackios-release-devops`, branch `release/exynos990-devops`, based on
`e3589a1cade9bfe06d0438d805147e33b18de8d7`. The active `x1slte` checkout and its
untracked files were preserved. No phone connection, flash, reboot, push, remote
setting change or GitHub workflow dispatch was performed. Newer committed main-line
snapshot `a64ecd4` was subsequently merged into this isolated branch as `563e270`;
the release tests and workflow lint passed again after the clean merge.

## Passed locally

- Fresh pinned XNU materialization: `xnu/apply.sh` applied 84 changed files and
  288 overlay files. First audit build used the local Apple-source clone only as a
  read-only Git source; the release adapter separately materialized its own source
  tree from the pinned remote. Neither borrowed compiled kernel output.
- Native dependency setup and **source kernel compilation**, including the new
  `./build.sh kernel --device x1slte --jobs 4` adapter. Stripped output was
  **15,144,376 bytes**, below the **16,777,216-byte** loader limit. Compiler: Apple
  clang 21.0.0 on the supplied Apple Silicon macOS host. Build metadata/checksum are
  written to ignored `out/kernel/`; no firmware or iOS system image was built.
  The later integrated build at `3fe9bd2` also passed from clean source, producing
  **15,161,256 bytes** with `source_dirty: false`; SHA-256:
  `b9cdbbcf48cb8dcc87604a216835b501a22f7a7080472f0929489c166721432c`.
- **27 Python tests** (`bash ci/check.sh`): nested content changes with unchanged
  mtimes; copy/location and mtime stability; mode/options/add/remove invalidation;
  checksum corruption; failed atomic publication; concurrent cache writers;
  missing inputs; output symlinks; source-only edits preserving the system key;
  firmware/provider changes invalidating it; symlink firmware rejection; generated
  tool files not invalidating source inputs; Android v2/Samsung table validation;
  truncated APFS/boot images; HFS CRC/capacity; exact model/bootloader/geometry/mount
  evidence; unsupported targets; protected destination rejection; full synthetic
  candidate verification/tampering; boot/recovery mismatch; AVB overlap; private
  store deduplication across candidate metadata changes.
- Shell syntax for new entry points, CI helper and edited legacy build scripts.
- Python syntax for every release helper.
- `actionlint` **1.7.12** passed all four new workflows; custom runner label is
  declared in `.github/actionlint.yaml`.
- `git diff --check` and current release-document local link checks.
- Negative CLI checks: y2s build refuses an unmaintained board; image doctor refuses
  missing private inputs. These failures are expected and protective.
- Gitleaks **8.30.1**, with redaction and `--all --full-history`, scanned local Git
  refs. Four generic-api-key findings were inspected: two SIMD local-variable
  expressions (`data_key_lo` / `data_key_hi`) repeated in historical zstd source
  commits. They are false positives, not credential literals. Redacted reports
  remain ignored in `out/`. The new release helper directory also passed a scan.
  This does not certify absence of personal identifiers, firmware or license issues.

## Not established

- New workflows have not run on GitHub; hosted macOS toolchain differences remain
  to test. Private worker and storage volumes have not been provisioned.
- A fully clean, all-source userland/UEFI/complete image build has not been run.
  The dedicated worktree intentionally lacks the owner's private extraction,
  firmware, DT table and UEFI input snapshot; no files were borrowed from the other
  agent's active build tree to hide those missing prerequisites.
- A synthetic APFS/header fixture validates safety logic, not filesystem integrity
  or bootability. Complete APFS checking and actual AVB signature validation are
  still required for a real candidate.
- No first-install/update/rollback procedure has been tested on hardware. The host
  helper remains read-only; there is no qualified TWRP ZIP or Odin release package.
- Publication inventory/provenance review remains open: the initial tracked-tree
  audit found 164 review items, including 154 binaries/artifacts and 42
  research/prebuilt items (categories overlap). A secret-pattern scan cannot clear
  their provenance or redistribution status. The integrated tree has **174** review
  items (156 binary/artifact and 51 research/prebuilt, overlapping).

Use the [release checklist](checklist.md) to close these gaps. Do not turn a passing
kernel build or these host tests into a claim that the release is ready to flash.

## Continuation: merged release validation

Merged the latest committed `x1slte` snapshot `8d584cc` into this branch as
`ef711056`. The other checkout remains on `x1slte`; no device action occurred.

- Fresh Mesa 25.0.7 softpipe/Panfrost and AIR/Metal compiler build passed, with
  four compile workers. Source archive SHA verification now works with the macOS
  host's SHA tooling. Mesa's Python build dependencies still need a version lock.
- OpenH264 source at `1bca62311e447fd9c39fc93de2c4b96cd5ce971c` compiled for iOS
  arm64 in this worktree. The image CLI now builds its own pinned dependency.
- Boot-time HFS formatting was removed. Persistent mode halts on unreadable,
  missing/damaged or unmountable data instead of formatting or silently booting
  with volatile data. The C policy test covers all outcomes; modified init also
  cross-compiled successfully. Hardware validation is outstanding.
- Image staging now uses independent inodes (APFS clone copies on macOS), with
  a mutation test proving staging cannot change the locked input file. Directory
  symlinks and nested destinations are rejected.
- **28 release Python tests**, the C data policy test, and passcode build,
  file-restore and native-adapter tests passed after the merge. The passcode tests
  are now included in the normal CI entry point. Actionlint passed all workflows.
- Source userland compilation and APFS root generation passed before and after
  the merge. The merged root includes the native keybagd/guarded restore chain.
  Complete system generation remains blocked by the extraction recipe freshness
  check: the independently copied extraction predates the checked-out selection
  recipe. Its timestamp was not touched to bypass this protection. A new reviewed
  extraction is required before claiming a complete candidate. Missing firmware
  categories and locally installed bootstrap components also need qualification.
- No recovery candidate was assembled, published or flashed from these partial
  results. Offline first-install initialization, AVB verification, filesystem
  validation and actual install/update/rescue tests remain release blockers.

The earlier missing-input description records the initial audit state. Sanitized,
independent copies of the extraction, firmware, UEFI and DT inputs were subsequently
staged locally; owner credentials, personal apps and compiled userland were excluded.
The supplied UEFI artifact was hash-checked, not rebuilt from source in this step.

Merged kernel build completed successfully at `8d2d12b6` with
`source_dirty: false`: **15,161,848 bytes**, below the 16 MiB limit. SHA-256:
`4aee6fc2bd598506f14fe2138947c1d0185ef67346c9c7890dcd38d417f3b7d1`. The isolated pinned OpenH264 fetch/build helper also
completed successfully. Neither output establishes phone boot qualification.

## One-command local build — completed

`./build.sh local --output dist/local-validation` completed successfully on
2026-11-14. Source candidate commit: `b563e2497152ecd2a615abd820509d3845c18cbd`;
`source_dirty: false`. The pipeline prepared a new reviewed
recipe extraction, generated Apple device-tree identity and the XNU menu font,
built source kernel/media/userland/loader, assembled Samsung DT/AVB recovery,
packaged the three images and passed `run.sh verify`.

The pinned OS cryptex and device-tree members were fetched from Apple's CDN with
byte-range and ZIP CRC checks. The root DMG and App cryptex were independent APFS
copies of existing local inputs and were freshly extracted; active source trees
and compiled userland outputs were not borrowed.

Real reuse was demonstrated during retries: extraction recipe/content verification
reported a hit, and the complete system image was restored from cache key
`f316850227fe25c9acf53b96fca5ee24779d59ab8550ef98538985fe0f50f9e2`. Its packaged SHA-256 exactly matches the stored object:
`800030a9aae82106ccf292ab4aebd2af549e76b62db917dfbb678b6650bc76a4`. Loader-only preparation fixes did not rebuild that system image.
Failed attempts stopped before packaging. They exposed and resolved hidden input
mutation for callservicesd and missing generated identity/menu-font headers.

Ignored local outputs: `dist/local-validation/`, `dist/local-validation.tar` and
`dist/local-validation.tar.sha256`. The archive is approximately 2.5 GiB and is a
private development candidate, not an Odin/TWRP installer. Kernel, root image,
system image and recovery were assembled, but UEFI remains the supplied hash-locked
artifact. Filesystem/AVB/hardware qualification gaps stated above remain open;
structural/checksum verification is not a successful boot/install test.

All **34 release Python tests**, the C data policy test and passcode integration,
restore-file and native-adapter checks passed during this implementation. Final
workflow lint and shell syntax checks passed. No phone operations were performed.

## Experimental TWRP package — implemented and built

Installer source commit: `c2d65727081d54651e142980446ff3cdfed7ef28`; source tree was clean.
`./build.sh twrp --bundle dist/local-validation --output dist/jesvi-ios-x1slte-experimental.zip --experimental` completed successfully.
The archive contains the real candidate images, HFS kit, bound installer metadata
and a static Linux/ARM64 `META-INF/com/google/android/update-binary`. ZIP inventory,
CRC checks and the native host build of the same image/HFS validators all passed.
ELF architecture, executable permissions and absence of a dynamic interpreter were
checked. Archive SHA-256: `f7843bd11214c77db322a45f1e3b745b6f76b5e30e6ff5900195558c3de8c88a`.

**35 Python tests**, C boot-data policy tests, passcode integration/restore/native
checks, **13 Go top-level tests** (plus failure subcases), Go vet, static ARM64
cross-compilation, actionlint and whitespace checks passed. File-backed tests cover
fresh HFS headers/zero extents, data/keybag preservation on updates, existing-image
hash reuse, tails retained, failures before BOOT, corrupt/truncated/unexpected ZIP
inputs, wrong identity/unlock/mount evidence, staged boot/recovery independent of
transport, and UI command-injection rejection.

No PIT/GPT rewrite or resize backend was added. The native installer exclusively
claims only its four measured target partitions, streams the large system image,
requires physical holds before writing, fsyncs/invalidates block caches/readbacks,
and never requests reboot. It supports preflight-only packages by default;
`--experimental` explicitly enables the hardware-unqualified writer.

These results supersede earlier statements that no writer/initializer exists.
They do **not** establish actual TWRP properties/holder cleanup, physical key input,
block ioctl behavior, ZIP64/sideload handling, first HFS mount, sboot/AVB behavior,
bootability, update compatibility, interruption recovery or Download Mode rescue.
No phone was accessed, flashed or rebooted. See [TWRP guide](twrp.md) before any trial.

## ExtremeROM re-review, rollback preparation and final ZIP output

Reviewed ExtremeROM source at `2056320784b7d1d88f9e7025f86f7bbd39945d6b`;
see [comparison](extremerom-review.md). Its S20 physical super size matches our
profile. Samsung hardware-model/bootloader property precedence was corrected
and tested. No physical PIT/GPT resize was introduced. Legacy BOOT-only Android
return wording was corrected; [rollback](rollback.md) separates previous-compatible
iOS restoration from full stock Android restoration/reset.

The read-only stock-set inspector has synthetic TAR/MD5 tests for complete sets,
wrong/missing CSC, model/revision mismatch, boot-only AP, corruption and unsafe
archive inputs. It has not validated a real stock firmware download or performed
a stock restoration. No claims of Samsung signature/PIT validation are made.

Current retained ZIP: `dist/jesvi-ios-x1slte-experimental-r2.zip`.
SHA-256: `c70e8c01c71d0d2d6ad13f642d07445b56f0c993a1d18fc882339bfb3b78b5be`. Its images match the retained original candidate;
the obsolete first ZIP/transport tar were removed to avoid duplicate storage.
The R2 installer source key was checked against the current recovery/builder sources.
Native Apple APFS checks passed the root extracted from recovery and system image;
Apple HFS checking passed the actual format-kit reconstruction on a private
read-only, unmounted virtual device. It was detached and removed. ZIP assembly now
requires those checks and AOSP AVB public-test-key verification before publication.

**43 Python tests**, C/passcode tests, **14 Go top-level tests** plus failure subcases,
Go vet, static Linux/ARM64 cross-build, actionlint and documentation-link checks
passed. Apple-framework tests were moved to required macOS checks, avoiding an
invalid attempt to link Darwin frameworks on Ubuntu. Private full builds run
checks before producing the final ZIP. Local CLI preflight accepted the new default
ZIP output with `--experimental` and no redundant `--twrp` flag.

Local full builds now emit ZIP by default; `--no-twrp` explicitly requests a tar.
The private Actions workflow produces the same ZIP/checksum/build receipt and
uploads a seven-day private downloadable artifact. Its experimental-writer checkbox
defaults false. Source/public CI still publishes only source-kernel/check artifacts.
No GitHub repository/worker was configured or workflow remotely run in this session.

The user explicitly restricted this work to the package/DevOps branch. Fingerprint
work, other agents, their worktrees, physical phones and boot state were untouched.
Actual TWRP install/update/rollback, mapper cleanup and Download Mode rescue remain
hardware qualification items; host filesystem checks do not close those gates.

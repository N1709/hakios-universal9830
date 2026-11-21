# Release engineering review — 2026-11-14

## Recommendation

Use **Jesvi iOS** as the display name and **ios-exynos990** as the repository name.
Ship the first release as an **experimental SM-G980F developer preview**, with an
exact feature/known-issues table. Offer the other four device families as maintainer
opportunities. Do not call the port release-ready based on an estimated percentage:
installation, clean-build reproducibility and recovery are independent acceptance gates.

This review starts from local commit `e3589a1` on `x1slte`. Work is isolated in branch
`release/exynos990-devops`; the other agent's checkout, untracked files, phone and
build outputs were not changed. The branch must be reconciled with later main-line
changes before release, especially `userland/build.sh` and `x1slte.sh`. During the
final review, committed main-line snapshot `a64ecd4` was merged into this isolated
branch (`563e270`); its new calls/display/power changes are preserved. Any later
active work still needs the same integration review.

## What the audit found

| Area | Finding | Consequence |
|---|---|---|
| Repository root | Workspace is a research collection; actual project is `work/hackios-xnu-kernel` | Never initialize Git at the collection root or upload `work/`, `device/` or `build/` |
| Git remote | Active project has only `upstream`, no publication `origin` | Hosting settings, CI runs and actual releases cannot be configured by a local branch alone |
| README | Described POCO/Qualcomm, RAM-only boots and no flashing | Replaced with Samsung-specific entry points; original retained with attribution |
| Git ignore | Root uses an allowlist; `.github`, new entry points and SHIPPING were not all admitted | Added explicit entries; existing tracked artifacts still need a separate review |
| Workflow location | `docs/upstream/github-check.yml` (formerly under `ci/`) is a historical template, not an active GitHub workflow | Real workflows now live under `.github/workflows/` |
| Clean kernel | Exported overlay applies to pinned Apple XNU; native kernel build succeeds | Source-only CI is practical now |
| Clean images | Legacy `ios165` paths, `host-macos` tool paths, global UEFI/DTBH/AVB paths | Added repo-relative/explicit inputs and corrected wrapper paths; full image qualification remains open |
| Source/binary parity | macOS userland still uses inherited `prebuilt/userland` for part of its build | A source build of the kernel is not proof that every user-space binary can be regenerated |
| Publication inventory | Initial tracked-tree audit: 164 review items, including 154 binaries/artifacts and 42 research/prebuilt items (overlap) | Review origin/licensing and private data before public history or automatic source archives are exposed |
| Missing tracked prerequisites | Active checkout has untracked `libredwood.dylib`, `m2m_mark`, `prefset` and other work | Do not silently borrow them; determine which are regenerated versus required inputs |
| System cache | Old image skip uses selected mtimes, missing nested firmware/app edits | Added content-hash cache around the actual system stage, after required executables compile |
| Flashing | Kernel/root live inside recovery; BOOT needs a Samsung DT table, not raw FDT | Generic ROM ZIP/Odin recipes cannot be copied without a board-specific installer |
| Storage | `super` is raw APFS at offset 0; userdata has a fixed 116,253,523,968-byte expectation | This destroys Android's logical partitions and cannot be generalized by model name alone |
| Recovery | Runtime replaces TWRP; AVB footer covers the boot head, not all runtime payload bytes | Reinstall/rollback and full-payload integrity need explicit validation |
| Runtime formatting | `init.c` may initialize HFS when it finds no valid data filesystem | Dirty update and failed filesystem detection must be reviewed before public flashing |

After integration with `a64ecd4`, the tracked-tree inventory contains **174** review
items (156 binaries/artifacts; 51 research/prebuilt items, overlapping). The
inventory is triage, not a claim that all flagged files are forbidden. Some are
project-built binaries, some donor vendor modules, and some research files. The
scanner does not certify history or inspect encoded firmware arrays. For example,
`redwood_focaltech_fw.h` / `hakios.focaltech_fw.h`, camera tables and inherited UEFI
modules require human provenance review even if represented as source text.

## What to borrow from UN1CA

I inspected upstream UN1CA at commit
[`0c2ea1d61ddc78ae8e6900787f9f9e7269b7b439`](https://github.com/salvogiangri/UN1CA/tree/0c2ea1d61ddc78ae8e6900787f9f9e7269b7b439),
branch `sixteen`. Its useful pattern is a discoverable target selector, common
platform configuration, target overrides, separated firmware downloads/extraction,
patching and packaging. The selected device is explicit in its
[build environment](https://github.com/salvogiangri/UN1CA/blob/0c2ea1d61ddc78ae8e6900787f9f9e7269b7b439/buildenv.sh)
and [configuration generator](https://github.com/salvogiangri/UN1CA/blob/0c2ea1d61ddc78ae8e6900787f9f9e7269b7b439/scripts/internal/gen_config_file.sh).

For this project the equivalent is:

1. **Shared Exynos platform:** XNU, GICv2, loader protocol and common drivers.
2. **Board profile:** exact models, bootloaders, measured memory/partitions, UEFI,
   panel/touch/firmware choices, owner and evidence.
3. **Immutable input lock:** OS build, SDK, vendor source identities, UEFI commit,
   tool versions and hashes; no "latest successful artifact" as an input.
4. **Build stages:** source kernel → board UEFI/loader → root → system → package.
5. **Qualification:** checksums and format checks → recovery rehearsal → actual
   installation/rollback → signed, immutable release manifest.

Do not import UN1CA's Android partition installer wholesale. Its own
[installation guide](https://github.com/salvogiangri/UN1CA/discussions/130) uses
custom recovery and describes its ROM's filesystem flow. Here APFS/HFS and the
recovery-resident runtime change the design. Its scheduled
[blob workflow](https://github.com/salvogiangri/UN1CA/blob/0c2ea1d61ddc78ae8e6900787f9f9e7269b7b439/.github/workflows/blobs.yml)
is useful as an example of automation boundaries, not a template for publishing
Apple or per-phone inputs. No UN1CA code was copied into this branch.

## The five-device plan

The first families are S20, S20+, S20 Ultra, Note20 and Note20 Ultra, confirmed by
the owner. FE is later. Samsung also lists the Exynos S20 FE in its
[processor device showcase](https://semiconductor.samsung.com/processor/showcase/smartphone/),
so it should not be described as absent from the Exynos 990 family.

The shared SoC reduces repeated driver work. It does **not** make a port a rename:
RAM reservations, panel/DSC settings, camera modules, touch firmware, board
revisions and storage sizes differ. Five marketing families also cover more than
five exact model variants. Only SM-G980F has an allowlist here; LTE/5G/region
variants must enter via separate measured evidence. See [porting](porting.md).

## CI and cache decisions

Public PRs can run host tests and compile source kernels on disposable hosted
runners. Private input/image builds belong on a separate private build repository
and disposable macOS workers with a restricted persistent cache volume. Do not
attach the active development Mac or phone to public CI.

The system image cache hashes the selected extraction, actual stage0/provider
executables, recipe/APFS tools, firmware, optional apps/bootstrap and options. A
kernel-only change therefore does not force a multi-GB system rebuild. A nested
firmware edit invalidates it even if timestamps are unchanged. Misses force a build;
corrupt entries fail; publication is atomic. Full-file content hashing costs I/O
but avoids the correctness failures of mtimes or a single Git commit cache key.

Kernel CI has its own exact source/compiler key. No fallback restore key is used
for final outputs. Private retained candidates reference image objects by SHA-256,
so repeated system images occupy one object across candidate manifests. Local
`.cache` is disposable; retained release candidates are a separate store.

GitHub warns that fork PRs may read base-branch caches; cache entries are immutable,
and standard cache eviction is not durable hosting.
[GitHub cache reference](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching).
Do not cache iOS, firmware, SSH material or personal extractions in public Actions.
GitHub-hosted jobs also have bounded runtime/storage; source CI and private image
assembly have separate budgets.
[Actions limits](https://docs.github.com/en/actions/reference/limits).

## Flashing and delivery decision

**First install:** qualify a dedicated TWRP environment and a streamed installer
(or host-assisted TWRP writer). Validate exact model/bootloader/geometry, backup and
read back every write. Commit BOOT last. This is a destructive iOS-only installation.

**Rescue:** retain a separately downloaded, verified TWRP Odin tar and matching
Samsung stock firmware **off the phone**, with a demonstrated Download Mode path.
Odin is useful for this bootstrap/rescue role, not an automatic catch-all package
for raw APFS and userdata initialization.

**Updates:** initially require an explicit recovery/host-assisted process. The
custom loader's fastboot-like commands are not Samsung stock fastboot. Its existing
research scripts do not implement an authenticated updater, safe resume, rollback
or a verified installed-version check. Do not promise OTA or dirty updates yet.

**Distribution:** source, notices, build tooling and reviewed project-built
artifacts may go to public releases after audit. Complete OS/device images remain
locally assembled/private unless redistribution has been separately cleared. A
public download link does not establish redistribution permission. Package the
public bootstrap separately from private multi-GB images. Per-asset GitHub release
uploads must be under 2 GiB; avoid an unsplittable multi-GB ZIP. If authorized image
hosting is later chosen, use content-addressed objects/chunks and a signed manifest
with complete size/hash records, range/resume and retention.
[GitHub release limits](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases).

The new `./build.sh verify|plan` is deliberately an **offline verifier/planner**, not a ready
flasher. This task did not authorize interrupting the other agent's phone tests,
and no new installer has been tested on hardware. See the full
[flashing design and acceptance tests](flashing.md).

## What this branch does and does not establish

Implemented: device registry and validation; developer entry points; isolated
kernel builder; pinned SDK source selection; private image input ledger and
assembly adapter; exact system image cache; private content-addressed retention;
image header/capacity/CRC checks; offline plan; CI/upstream/draft/private workflow
scaffolding; issue/PR templates; documentation and repository setup plan.

Host tests and local clean kernel compilation are evidence for those components.
They do not establish full image reproducibility, live GitHub workflow success,
remote repository settings, public distribution clearance or hardware-safe
installation. Those remaining gates are explicit in [the checklist](checklist.md).

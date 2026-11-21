# Release checklist and one-week plan

The date is a target, not an override for installation safety. Release the exact
qualified scope; if first-install recovery is not proven, publish a source/developer
preview with no end-user flashing promise.

## Work completed in this branch

- [x] Separate branch/worktree; no changes to the active checkout or phone.
- [x] Samsung-specific README, docs index, contributor/device guidance.
- [x] First-five-family registry plus later FE candidate; untested variants blocked.
- [x] Source kernel build entry point and isolated exported-source tree.
- [x] Source CI, upstream reporting, private candidate and gated draft workflows.
- [x] SDK Git revision pin and explicit private input/UEFI/DTBH/AVB paths.
- [x] Content-based system cache with checksum verification and atomic publication.
- [x] Private input lock, candidate manifest and content-addressed image retention.
- [x] Structural image validation and offline flashing plan; experimental writer tracked separately below.
- [x] Publication inventory and GitHub settings plan.

See [validation](validation.md) for tests actually run. These checks do not imply
that the remote workflows or complete private image path have been executed.

## Must finish before a public source release

- [ ] Final owner/repository and branding confirmed; origin created/configured.
- [ ] Current active work merged/reconciled; release candidate source commit frozen.
- [ ] Full-history secret/private-data audit; tracked artifact/provenance review.
- [ ] Review embedded firmware arrays, donor UEFI modules, screenshots and calibration
      material. Remove or replace unresolved artifacts in a separate audited branch.
- [ ] Correct old blanket claims in LEGAL/COMPLIANCE/NOTICE against actual inventory;
      preserve licenses and upstream attribution. No assumption of redistribution rights.
- [ ] Remote rulesets, required checks, environment restrictions and security settings applied.
- [ ] CI succeeds from the final remote on actual hosted runners, including a fork PR test.
- [ ] Public release notes list only tested exact models/features and known issues.

## Must finish before an installable image release

- [ ] UEFI wrapper/overlay source made reproducible and accessible; pin its full commit,
      Mu submodules, compiler and image hash. The referenced wrapper URL currently
      returns unauthenticated 404; it may be private, not necessarily missing.
- [ ] Every required userland/prebuilt component reproducibly rebuilt from matching
      source, or explicitly versioned with provenance and permitted distribution.
- [ ] Exact private firmware inventory finalized. No optional-missing dependency
      silently produces a reduced-function image advertised as complete.
- [ ] Clean private image build succeeds; second unchanged build proves reuse; edited
      nested firmware input proves invalidation. Run APFS integrity checks.
- [ ] Input authenticity and tool/archive checksum pins completed, beyond local hashes.
- [ ] Release debug service exposure, SSH provisioning and identity generation reviewed;
      no developer keys/passwords/calibration identities embedded.
- [x] Boot-time formatting removed; failed reads/headers/mounts halt persistent-data boot.
- [x] Experimental TWRP writer and explicit offline HFSX initializer implemented and host-tested.
- [ ] TWRP writer, first-install initializer, update preservation and rescue qualified on hardware.
- [ ] Exact TWRP build/model properties, bootloader/unlock prerequisites and AVB behavior
      recorded; full image/manifest authentication implemented.
- [ ] First-install writer implemented with preflight, streaming, readback, no protected
      partitions and fail-stop behavior. Current `./build.sh verify` is only offline verification.
- [ ] BOOT head, runtime, clean install, cold boot, interrupted writes and Download Mode
      rescue demonstrated on the owner's SM-G980F.
- [ ] Data preservation/update/downgrade policy tested; no OTA/dirty-update claim by default.
- [ ] Redistribution scope approved before any complete image/firmware hosting; local
      assembly remains the default delivery model.
- [ ] Release checksum/manifest signatures, retained previous candidate and support docs ready.

## Suggested sequence for the coming week

| Day | Work | Exit evidence |
|---|---|---|
| 1 | Integrate reviewed release branch; inventory/history cleanup; final repository | Audited source commit, no private artifacts in intended publication |
| 2 | Fresh source + complete private build; pin inputs/toolchain | Build log, provenance, image checks, no borrowed active-workspace files |
| 3 | Repeat build/cache test; complete installer preflight and explicit format gating | Same input reuses system; changed input rebuilds; negative safety tests |
| 4 | Controlled first install and independent Download Mode rescue | Full readback, boot/feature results, proven stock/TWRP recovery |
| 5 | Failure/interruption/update tests; fix defects | No unexpected writes, data handling verified, known limitations recorded |
| 6 | Freeze candidate; test cold boot, power/charging, storage and declared features | Exact commit/hashes and device acceptance record |
| 7 | Publish audited scope, notes and contributor roadmap | Source preview or qualified installer, accurately labeled |

Any failed storage/recovery gate keeps end-user installation out of the release.
Other model maintainers can begin source work immediately; they need not wait for
a claim that all five models are supported.

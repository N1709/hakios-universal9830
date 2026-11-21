# Local storage cleanup review — 2026-11-14

Initial read-only inventory of the project workspace. The owner subsequently
authorized cleanup of stale files; see **Cleanup performed** below for actual
removals and measured reclaimed space. Other agents' files/processes were preserved.

At inspection, `df` reported approximately **45 GiB free** on the 228 GiB data
volume. The project workspace occupies approximately **59 GiB** by `du`, primarily
`work/` (46 GiB), reference clones (9.8 GiB) and device data (1.9 GiB). Sizes below
are allocated-file estimates, not guarantees of physical space reclaimed: APFS
clones/snapshots, hard links and concurrent builds can change the result.

All paths below are relative to `ios-port-research/`.

## Recommended order

| Priority | Candidate | Approximate size | Review/handling |
|---|---|---:|---|
| 1 | `work/hackios-release-devops/xnu-src/` plus three superseded `.cache/release-kernel/` entries | **2.37 GiB** | Created by this release audit, rebuildable. Keep the latest validated cache and `out/kernel/` |
| 2 | `work/hackios-xnu-kernel/userland/system.apfs.bak-nov7` | **2.44 GiB** | Old November 7 backup; confirm it is no longer the required rollback baseline. Keep current `system.apfs` |
| 3 | `work/hackios-files/out/` | **1.51 GiB** | Historical Files/Finder/identity test candidates; retain any specifically needed baseline. Do not delete the branch or worktree |
| 4 | Old `work/hackios-cellular/out/cellular/` test candidate directories, excluding `private-rom/` and `private-ril/` | **3.27 GiB** | Includes `m0-20261111`, Nov 11/12 baselines and live test batches. Check with the active work owner; retain current/recovery baselines |
| 5 | `work/hackios-xnu-kernel/logs/` and `work/hackios-cellular/logs/` | **up to 5.12 GiB** | Archive/compress old closed logs; preserve recent logs and rescue images. This is an upper bound, not a recommendation to delete both directories |
| Optional | `git/steelbrain__metal2vulkan/` | **1.90 GiB** | Clean reference clone, mostly a tracked validation corpus. Re-clonable, but confirm no current research depends on it |

The first four groups total about **9.6 GiB** before retaining selected baselines.
Only group 1 is entirely this task's disposable output. The other groups need
owner review; names such as `out` or `logs` do not prove that their contents are unused.

## Exact disposable builds from this task

Preserve the latest integrated source/build cache:

```text
work/hackios-release-devops/.cache/release-kernel/
  5b6790f9e9b7b0843bb33c1e6dba725ee203e872d25ca078a3b4a91f76eda064/
```

Keep `out/kernel/mach.x1slte`, `out/kernel/build.json`, `out/kernel/SHA256SUMS`,
the release branch/commits and the source worktree. Obsolete candidates:

```text
work/hackios-release-devops/xnu-src/                                  0.564 GiB
work/hackios-release-devops/.cache/release-kernel/
  726c8c707e23af339c6ae37aea8d96fa38066d28f0e5b1abdd181299e3a5dac9/   0.603 GiB
  74ed544a81896de3c43a84ecad75253d3655c32dac637a8322333f3866dd3f12/   0.603 GiB
  dcbddb1a1b04b5241e09202e576456020d7896442aa7f26f4cff3566ae2f16bc/   0.603 GiB
```

These are generated kernel trees/builds, not separate user branches. An exact
candidate inventory is also saved locally in ignored `out/cleanup-own-builds.json`.
Recheck the retained key against `out/kernel/build.json` before any later cleanup.

## Large files to retain while development continues

| Path | Size | Why not a first-pass cleanup target |
|---|---:|---|
| `work/hackios-cellular/out/cellular/private-rom/stock-system.img` | 5.74 GiB | Stock reference/extraction input for active calls/modem research |
| `work/hackios-cellular/out/cellular/private-rom/stock-vendor.img` | 1.34 GiB | Device/vendor extraction source |
| `work/hackios-cellular/out/cellular/private-rom/stock-product.img` | 1.16 GiB | Stock reference input |
| `work/hackios-cellular/out/cellular/private-ril/vendor.img` | 1.36 GiB | Different size from the other vendor image; do not assume a duplicate |
| `work/hackios-xnu-kernel/sysimage/rootfs.dmg` | 4.40 GiB | Current build recipe can extract missing components such as callservicesd from it |
| `work/hackios-xnu-kernel/userland/system.apfs` | 2.48 GiB | Current system image, not the old backup |
| `device/` | 1.94 GiB total | Includes private recovery/firmware/unique device backups; protect and copy off-host |
| `git/Project-Silicium__Mu-Silicium/.git/worktrees/` | about 2.0 GiB | Live worktree/submodule Git metadata, not a generic cache |

`work/cellular-mu` has local modifications. Do not manually remove its parent
`.git/worktrees` entries or use `git clean -xfd`/blanket worktree pruning. The
camera, GPU, Files and wallpaper worktrees may also be needed despite a clean Git
status; ignored firmware/build inputs are not represented in that status.

Do not remove SDKs, the current extraction/dyld cache, firmware, SSH material,
`backup-main-before-cellular-merge-*`, EFS or active release artifacts just to make
`out/` or the workspace look smaller. Review stale experiments individually.

## Compression measured without changing files

A streaming gzip-level-1 trial (output counted and discarded) on three closed text
logs found **594.76 MiB → 29.44 MiB**, about **565 MiB recoverable** while retaining
all log contents in compressed form:

- `work/hackios-xnu-kernel/logs/serial/serial-20261107.txt`: 100.03 → 14.90 MiB.
- `work/hackios-xnu-kernel/logs/runs/resume-20261107-1344/als-crashloop-serial.txt`: 82.32 → 12.34 MiB.
- `work/hackios-cellular/logs/cellular/a-sbd-054306/transcript.txt`: 412.41 → 2.20 MiB.

No archive was saved and no original was removed. These files are already included
in the logs row above; do not add their savings to that row's full size.

## Follow-up cleanup procedure

Use explicit, reviewed paths. Confirm no active build or log writer uses them,
preserve a known-good recovery candidate and the manifest/hash for any retained
image, perform only the approved removals, then measure `df` again. Keep the
report updated with actual results; do not report estimated savings as recovered
space. Compressing old text logs is preferable when their debugging evidence may
still be useful.

## Cleanup performed — 2026-11-14

The user subsequently authorized local cleanup. Removed only this release task's
obsolete `xnu-src/`, four superseded `.cache/release-kernel/` entries, and the
redundant `dist/local-validation.tar` with its checksum. Before removing the tar,
its candidate manifest was checked against both the retained candidate directory
and the retained TWRP ZIP. No active build had those trees open.

Measured free space increased from **18,261,491,712** to **24,150,503,424 bytes**:
**5,889,011,712 bytes (5.48 GiB) recovered**. Later/concurrent activity can change
free-space readings. The latest kernel key `4c19e43e05d9a776ec9e00a8cd044632db76a152c6bbd08e2a5f4d6bbfeabbec`,
current source/output, TWRP ZIP, unpacked candidate, private image cache, extraction,
SDK/toolchains, firmware and all device backups were preserved. Other worktrees
and running application/agent/fingerprint processes were left untouched.

This recovers disk storage, not application RAM. The inspection showed about
3.7 GiB of swap in use; no processes were killed and no swap files were removed.
An exact record is in ignored `out/cleanup-performed.json`.

After validating the strengthened R2 TWRP archive, its original installer ZIP and
sidecars were removed as superseded output. Candidate manifest, image CRC/size
and per-image SHA identities match; all runtime image bytes remain available in
the retained candidate and R2 archive. The small old build metadata is retained in
`out/research/superseded-twrp-build.json`. This avoids keeping another 2.5 GiB copy.

Identical generated system-image copies were also compacted using independent
APFS clone copies of the validated immutable cache object, without changing any
path, byte content, permission or timestamp. Targets: `userland/system.apfs` and
`dist/local-validation/system.apfs`. The source and both targets were SHA-256
verified before replacement; clones were verified afterward and have independent
inodes. No target was open according to the local process check. This recovered
another **5,167,788,032 bytes (4.81 GiB)** at measurement.
All images remain present; `out/compact-images.json` records the operation.

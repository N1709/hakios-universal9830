# Contributing

Start with [the build guide](../docs/release/building.md) and
[device porting guide](../docs/release/porting.md). Use a branch/worktree per task. Keep
changes small enough to review and explain the observable behavior, tests and limitations.

## Branches

- `x1slte` is the working branch. All day-to-day work lands here.
- Feature branches (`feat/...`) start from `x1slte` and merge back into `x1slte`, never straight
  into `main`. Delete a feature branch (local and GitHub) once it is merged.
- `main` is the release branch. It only gets `x1slte` merged into it when a release is ready, so it
  can be behind `x1slte`.

Kernel work is represented by `xnu/redwood.patch` and `xnu/files/`. If editing a
private `xnu-src` tree, export from the exact current base before committing. Do
not export an older kernel tree over another developer's changes. The release
kernel adapter uses its own content-addressed source tree and never exports back.

Run `bash ci/check.sh` for release tooling changes and a source kernel build for
kernel changes. Report host compilation, recovered boot logs, visible screen
behavior and runtime tests as separate evidence. Never label an untested board supported.

A new board needs an actual maintainer, exact model/bootloader allowlists, measured
partition sizes, memory map, firmware provenance, rollback and hardware tests.
Do not copy SM-G980F offsets into a different storage/RAM variant.

Never commit personal device backups, EFS, unique IDs, passwords, SSH keys,
proprietary OS images or vendor firmware. Sanitize logs. Check staged file names
and the publication audit before committing. Existing donor binaries still need
review; their presence is not permission to add more.

Preserve copyright/license notices. Different files have different origins;
consult `NOTICE` and `THIRD_PARTY_LICENSES.md`, and record uncertain provenance as
unresolved. No source or firmware should be relabeled merely to simplify packaging.

PRs should name the device and source base, describe validation, document any
storage or bootloader effect, and update the appropriate release/subsystem guide.
New hardware support stays unmaintained/development until the qualification
checklist has real evidence. No one needs to provide a physical phone to contribute
host tests, build fixes or documentation.

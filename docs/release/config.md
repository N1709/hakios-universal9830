# release.conf: one file for every release build

`release.conf` at the repository root says what to build, like UN1CA's target
selection. `./build.sh release` reads it on a Mac; GitHub Actions reads the same file.

```sh
./build.sh release --dry-run                    # show the plan and file names
./build.sh release                              # build everything it asks for
./build.sh release --set WRITES=true --set OUTPUTS="twrp restore"
JESVI_TARGETS="x1slte y2s" ./build.sh release   # environment override
```

| Key | Meaning |
|---|---|
| `NAME`, `VERSION` | File names: `NAME_VERSION_YYYYMMDD_target.zip`, `NAME-Restore_...zip`; `_preflight` is added when writes are off |
| `TARGETS` | Device ids from `devices/exynos990/`. Targets without a measured profile are listed as SKIP |
| `OUTPUTS` | `twrp` (iOS installer), `preflight` (check-only), `restore` (back to Android), `tar` (images for developers) |
| `MODE` | `choose` (keep iOS data if present, else fresh), `update`, `fresh` |
| `WRITES` | `true`: ZIPs write after the Volume Up holds. `false`: every ZIP only checks the phone |
| `BUNDLE` | Existing verified candidate to package. Empty: full local build first (macOS, private Apple inputs, ~25 GiB free) |
| `OUT` | Output directory inside `dist/` |

Each ZIP gets `.sha256` and `.build.json` (source commit, Go version, writes flag).

## GitHub Actions

| Workflow | Runner | Output |
|---|---|---|
| `ci.yml` | public | tests, Go recovery checks, kernel, and **restore ZIPs** for every measured target (artifact `restore-zips-<sha>`) |
| `private-images.yml` | self-hosted macOS with private Apple inputs | everything `release.conf` asks for (`writes`/`outputs` inputs override it); artifact `jesvi-ios-<sha>-<run>` |
| `draft-release.yml` | public, on `v*` tags | draft release with the restore ZIPs attached |

The restore ZIP carries no Apple or Samsung files, so public runners can build it.
iOS ZIPs contain Apple-derived images and stay in private runs.

## Targets and mods

Per-phone settings, boot-args and mods: see [targets](targets.md).

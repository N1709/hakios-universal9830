# Build automation audit — 2026-11-14

Current conclusion: the repository has implemented source CI and private candidate
building components, but a one-click downloadable complete installation package is
not operational or qualified. No GitHub workflow has been dispatched or validated
on a remote repository. Only `upstream` is configured locally; no publication origin.

| Component | Current behavior | Remaining work |
|---|---|---|
| CI manual/push/PR | Tests, exact kernel cache, source-built kernel artifact | First actual hosted run and toolchain qualification |
| Private images manual | Private/default-branch opt-in; disposable self-hosted macOS; stages snapshot, builds, verifies, retains | Provision worker, variables, volumes and reviewed fresh inputs; actual successful complete run |
| Apple input fetch | Existing `sysimage/fetch.sh` fetches selected members from Apple | Integrate provisioning into CI; content-based extraction recipe receipt and integrity verification |
| Image reuse | Private system content cache and deduplicated retained image blobs | Real complete-image miss/hit/change tests |
| Candidate retrieval | Images retained on private volume | Authenticated downloadable candidate or private artifact; retrieval/materialization tooling |
| Draft release | Opt-in tag creates source-only draft after strict audit | Publication inventory unresolved; no installation package attached |
| Flashing | Offline verification/planning | Explicit offline data initializer and qualified first-install/update/rescue implementation |

## Live Apple check

A small HTTP Range request to the exact URL in `sysimage/fetch-from-ipsw.py`
returned HTTP 206 with `Content-Range: bytes 0-31/6165698198` and ZIP magic.
Remote ZIP directory inspection found:

- `096-04088-077.dmg`: 4,728,546,953 bytes (root filesystem).
- `096-04666-078.dmg`: 2,726,297,600 bytes (OS cryptex).
- `096-03536-083.dmg`: 10,485,760 bytes (App cryptex).

Only small range reads were performed; no multi-GB download was started.
The pinned input is iOS 16.5 / 20F66 for iPhone X, not arbitrary latest iOS.
Fetching from Apple supplies iOS components, not Samsung firmware, board-specific
UEFI/DT inputs or bootstrap additions, and does not qualify a rebuilt image.

## Additional implementation gaps found

The private snapshot stager copies selected extracted inputs but not `.done-rootfs`.
The legacy builder's freshness guard depends on that marker and mtimes. Therefore
a successful input content-lock check alone does not establish that the snapshot
was extracted using the current selection recipe. Replace this with a validated
content receipt before treating private CI as release-ready; do not simply copy or
touch the timestamp marker. The downloader currently checks expected member length,
not a pinned cryptographic member hash, and must validate HTTP ranges and archive
integrity before unattended provisioning can be claimed robust.

Earlier successful host builds and workflow lint demonstrate local components;
they do not demonstrate an end-to-end Actions installation package.

Subsequent local implementation completed successfully: see [local build](local-build.md)
and [validation](validation.md). This supersedes the local extraction-receipt gap
above. Remote Actions provisioning, delivery and installer qualification remain open.

A native experimental TWRP ZIP writer and offline HFSX initializer were subsequently
implemented and host-tested, with preflight mode as the default. See [TWRP](twrp.md)
and the latest validation record. Hardware/remote Actions qualification remains
open; this does not enable a public automatic release.

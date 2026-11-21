# Restore preflight build and source publication status

The v0.1.1 hosted CI run passed checks, source-kernel compilation and Restore
packaging: https://github.com/jesvi-eulah/Jesvi-iOS/actions/runs/44708864495.
Its Restore ZIP is a **non-writing preflight tool**, not an iOS installer or a
qualified Android restore. It contains the source-built ARM64 recovery executable,
measured device configuration, updater marker and README only. No iOS system,
firmware, wallpaper, inherited prebuilt or donor device tree enters this package.

`python3 tools/release/restore_audit.py dist/ci` verifies exact ZIP membership,
checksums, clean-build provenance flags, ARM64 executable format, measured profile,
no write opt-in and no credential patterns. CI explicitly sets `WRITES=false`.
The manifest hashes every included entry. This limited check does not establish
full source redistribution clearance or hardware qualification.

The separate draft-source workflow remains blocked by `audit.py --strict`:
99 tracked files require provenance/publication review, including inherited
binaries, Samsung device trees and wallpaper data. No records were bulk-approved,
no gate was disabled and no tag was moved. GitHub's automatic source archives
include the repository tree; they must not be presented as cleared source releases.

The full iOS TWRP installation package requires a locally assembled, verified
private image candidate and its firmware/input ledger. It cannot be assembled
from the firmware-free hosted checkout. Use the private-image workflow or local
private build for that package, then qualify it on the exact phone before claiming
installation support. Never upload the private image inputs as source artifacts.

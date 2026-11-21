# Apple Account and GPS integration progress

Feature branch: `feat/s20-apple-account-gps`.

Native iCloud account setup has succeeded on the S20 with a private Mac-assisted
account layer. The stored account was verified authenticated and active after setup. After the
latest service restart it remains saved and active, but its authenticated flag
reads false; current authentication health is unresolved. The shared layer
covers the account and media processes, preserves native credential verification,
and is reversible. It still depends on the private Mac helper and current-boot
injection configuration; it is not a standalone or fully integrated release.

Media & Purchases and App Store authentication remain unresolved. The original
FairPlay daemon can now be registered on demand from writable storage, with
byte-for-byte verification and no signing or licensing modifications. A bounded
startup test exited0 immediately. Registration is not evidence of a working
FairPlay service. Its expected system path is on the read-only image.

Real GNSS fixes were verified in the host-assisted lab. The native Core Location
prototype rejects stale, invalid and estimated fixes, but native app delivery and
standalone operation remain unverified. GPS scripts are experiments, not boot
services. Private positions and firmware are excluded from this branch.

Validation: shared layer cross-build; injection rule/rollback preservation test;
private comparison schema self-test; three NMEA status tests; ASan/UBSan NMEA
freshness and validity gate test; on-phone AuthKit health and authenticated active
account check after FairPlay registration. Subsequent code-origin observer upgrade passed its
native error-identity smoke, but post-restart account authentication reads false;
that regression/state change remains open. No reboot, keybag reset or account
removal was performed. See Apple Account handoff and GPS status for continuation.

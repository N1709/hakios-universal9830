# Installer qualification: x1slte (SM-G980F)

**2026-11-18, update mode, passed.** Jesvi-iOS 0.1.0 (`release/v0.1`, tag `v0.1.0`), installed from TWRP on the
measured S20 (SM-G980F, `G980FXXSNHYB7`), over an existing Jesvi iOS install. The phone booted to iOS with the
Apple logo from power-on and its data intact (confirmed by the owner).

| | |
|---|---|
| TWRP | 3.7.1_12 Artisan (ExtremeKernel 4.19.325), unified Exynos 990 build: `ro.product.device=z3s` |
| Unlock evidence | `ro.boot.verifiedbootstate=orange` (flash.locked/device_state not reported by this TWRP) |
| Method | `tools/install/twrp-install.sh` steps: ZIP copied to `/tmp`, update-binary run detached (API 3, fd 3) |
| Battery | 29 % and charging; the 50 % guard was overridden for this one run at the owner's request |
| Writes | super, recovery, boot: each written and read back |

What the trial found, fixed before this record:
- TWRP 3.7.0_11 (official, 2022) is too old for the HYB7 bootloader: USB drops, a reboot mid-check.
- TWRP's Install button refuses on an iOS phone ("Unable to find partition for path '/system_root'"), before the ZIP runs.
- Unified Exynos 990 TWRP reports `z3s`; the installer now accepts the family's TWRP products after the exact model/bootloader match.
- A USB hub between the computer and the phone made TWRP's adb vanish; plug straight in.

Installer log (on the phone, `/tmp/ub.out`):

```
Verifying complete ZIP payload before any device writes...
Jesvi iOS (x1slte): EXPERIMENTAL installer. Hardware qualification pending.
Plan: reuse existing physical partitions; no partition-table changes.
Exact device, bootloader, partition sizes and battery checks passed.
UPDATE: existing HFSX data/keybags will be preserved.
Use verified OFF-DEVICE backups and tested Download Mode rescue. TWRP will be overwritten.
Confirm backups/rescue and installation of this experimental candidate.
Hold VOLUME UP for 3 seconds to confirm; VOLUME DOWN/POWER cancels. Timeout: 45 seconds.
No Android partition map at the start of super (already iOS); keeping any saved copy
Writing and verifying super
Writing and verifying recovery
Writing and verifying boot
All written ranges passed readback. No reboot requested.
Installation readback completed. Keep this log off-device; boot testing is still required.
EXIT=0
```

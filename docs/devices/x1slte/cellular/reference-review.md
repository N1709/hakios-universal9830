# Community advice review — 2026-11-11

The suggested research path is useful. A modem image is firmware, not an AP
radio driver. A device tree describes wiring/resources, not a working RIL.
The Linux kernel transport, userspace radio protocol and iOS telephony bridge
are separate implementation layers. Same CPU alone does not establish modem,
firmware-protocol or board compatibility.

## References checked

| Reference | Evidence and reuse boundary |
| --- | --- |
| [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) | Canonical README dated Oct18 explicitly lists cellular unavailable. A newer fork may differ; do not infer all Redmi ports have cellular. |
| [Yofuness/ios-sunfish](https://github.com/Yofuness/ios-sunfish) | Pixel4a README status Nov3 lists cellular not planned. Useful platform integration lineage, not evidence of a working radio stack. |
| [DenialOfGravity/ios-redfin](https://github.com/DenialOfGravity/ios-redfin/tree/ws-n-ipa-gsi1) | Already audited: LTE internet/SMS-send evidence, Qualcomm QMI/IPA packet code; not an S5123 driver or verified VoLTE solution. See cellular-s20.md pinned commits. |
| [ben443/samsung-s20-pmos](https://github.com/ben443/samsung-s20-pmos) | README reports early bring-up, no working cellular evidence; also labels SM-G980F as z3s whereas this phone is x1slte. Do not substitute its board files. |
| [Replicant/libsamsung-ipc](https://replicant.us/supported-devices.php) | Open Samsung protocol work is relevant, but published supported devices are older; S20 support is not established by that list. |
| [omarchy-mobile raven modem plan](https://github.com/derekross/omarchy-mobile/blob/c3cfd86233eb6781260b315d741e0f05c9d1073e/device/raven/MODEM-PLAN.md) | Closer hardware reference: Pixel6Pro/GS101 with S5123 and SPI boot. Project reports working data/SMS/CS voice, unresolved VoLTE. Different AP, GPIO/SPI/PCIe resources and firmware protocol; not directly deployable. |

## New useful same-modem reference

Read-only checkout: `git/derekross__omarchy-mobile`, pinned
`c3cfd86233eb6781260b315d741e0f05c9d1073e`. Sparse paths: device/raven and
packages/omamobile-modem-sit. No reference scripts executed on the phone.

Raven reports wake/PS_HOLD changes about460ms after SPI BOOT, then requires
AP2CP_WAKE before PCIe link-up. Its userland uses Google's SIT protocol; this
is not proof that Samsung S20 firmware accepts the same messages. Its CS-call
success does not establish Jio VoLTE. No direct import of its command IDs,
firmware, NV files, MMIO addresses or security-controller configuration.

Cross-check against our pinned Samsung source:
`modem_ctrl_s5100.c:649` polls CP2AP_WAKE200 times at20ms; line1083 raises
AP2CP_WAKE with5ms delay before `exynos_pcie_host_v1_poweron`. Our DT uses
PCIe channel1 and SPI16; raven uses PCIe0/SPI9.

Action taken: powered BOOT preflight now observes three input signals for a
bounded4 seconds after successful TX (previously20ms). It records initial/final,
changed and rising bitmasks; a pre-existing pulled-up high is not a transition.
A failed delay returns through the shutdown path. No automatic claim of CP
acceptance or PCIe access was added. Probe ARM64 build passes; hardware pending.

## Already available locally

- Own `device/backup/20261105-203534/radio.img` and dtbo backup; no new recovery
  dump needed for BOOT work. Firmware retained privately outside Git.
- Own extracted DT and pinned ExtremeXT Exynos990 vendor kernel source.
- Stock `init.baseband.rc` launches `cbd -t s5100`, BOOT device `umts_boot0`,
  RADIO partition source, and EFS path. This confirms separate boot-daemon and
  RIL roles; it does not authorize EFS writes or make Android binaries run on XNU.

Next: current phone backup after lock release, powered BOOT probe and shutdown,
then board-specific PCIe1 enumeration/remaining firmware, Samsung IPC/SIM,
LTE packets and iOS interface; IMS/VoLTE/audio remain separate acceptance work.

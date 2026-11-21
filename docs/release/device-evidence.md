# SM-G980F measured evidence

This sanitized record is the basis for the `x1slte` development profile. It is not
an installer certification. Measurements were recorded on the owner's phone on
2026-11-05; private source dumps/backups remain outside this repository.

| Field | Observation |
|---|---|
| Model | SM-G980F, S20 LTE, Exynos 990, 8 GB / 128 GB |
| Bootloader | G980FXXSNHYB7 |
| Boot image | Android header v2, page 2048; Samsung DT table magic `d7b7ab1e` |
| BOOT | 61,865,984 bytes |
| RECOVERY | 69,009,408 bytes |
| SUPER | 9,437,184,000 bytes |
| USERDATA | 116,253,523,968 bytes |
| System window | super + 0, maximum 4,294,967,296 bytes |
| Super log tail start | `0x1E8B04000` |
| Recovery log | 16 MiB, length 8 MiB |
| Recovery kernel | 32 MiB, maximum load 16 MiB |
| Recovery root APFS | 52 MiB, must end before recovery footer |

Local evidence sources (not public artifacts): `device/DEVICE.md`,
`device/dump/20261105-203631/partition-sizes.txt` and the owner's stock boot/DT data.
Repository implementation: `common/devices/x1slte.conf`, `x1slte.sh` pack/payload/
standalone functions, `xnu/files/bsd/dev/redwood_ufs.c` data-size check,
`userland/src/init.c` mount-only HFS policy, `tools/hfs-kit-any.py` format.

TWRP identifies the S20 Exynos family as `x1s`; `x1slte` is this project's board id.
The recovery product aliases in the profile are provisional until the exact chosen
TWRP build's `getprop` has been recorded. Never accept an alias alone instead of the
exact model and measured geometry.
[Official TWRP device page](https://twrp.me/samsung/samsunggalaxys20.html).

The early comments in `common/devices/x1slte.conf` include superseded pinned-file
and modem-reservation history. The current release design is `ios-only-v1`; do not
use those historical offsets to infer a dual-boot layout. The 20 MiB packing slot
comment also differs from the loader's 16 MiB read limit: release checks use the
smaller 16 MiB limit.

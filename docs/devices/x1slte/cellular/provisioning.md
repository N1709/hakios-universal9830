# Modem files on a clean install, and other regions and carriers (2026-11-20)

## Why the SIM disappeared after a clean install

The modem service (`redwood-cellulard`, launchd job `com.redwood.cellular`) boots the S20's Samsung modem with six
files in `/var/root`. Until 2026-11-20 they were copied there by hand, so a clean data volume had none of them, the
service ended at once ("no modem", no signal) and the SIM looked dead although every piece is on the phone.

None of them may be shipped or committed (Samsung firmware). Since 2026-11-20 the phone makes them itself:
`redwood-cellulard` runs `/redwood/bin/cell-provision` whenever one is missing (source
`tools/cellular/cell-provision.c`). It opens the partitions **read-only**; EFS and the modem partition are never
written.

| File | Made from |
|---|---|
| `cell-radio.img` | the phone's modem partition `/dev/rufs_radio` (up to the end of its last section) |
| `cell-toc.bin` | the partition's table of contents: its first 800 bytes |
| `cell-boot.bin` | the TOC's BOOT section |
| `cell-boot.info` | "S512" + the BOOT entry: 1, size, CRC, load address, id |
| `cell-plan.txt` | the TOC's MAIN, APM, VSS entries (from `cell-radio.img`) and NV (from `cell-nv.bin`) |
| `cell-nv.bin` | `nv_data.bin`, the modem's own settings, read out of the EFS ext4 file system `/dev/rufs_efs` |

Only missing files are made; an existing `cell-nv.bin` (the modem updates its private copy) is kept. Verified on the
phone (2026-11-20): the five radio-derived files came out byte-identical to the hand-made copies of
2026-11-15, and a clean-install test (all six moved aside, reboot) made them all, `cell-nv.bin` from EFS included,
and registered on Jio with 4 bars and data in about 20 s.

Run it by hand: `ssh s20 /redwood/bin/cell-provision --check` (what is missing) or without `--check` (make it).
A test beside the real files: `CELL_PROVISION_DIR=/var/root/cell-test/ /redwood/bin/cell-provision`.

## Other regions and carriers

The modem firmware is whatever Samsung firmware the phone had before iOS: its own RADIO partition (region code in
the firmware name, e.g. G980F**XXU**... for Europe or **INS**/**INU** for India). The port does not replace it, so:

1. **Modem firmware for your region:** flash Samsung's stock firmware for your region with Odin *before* installing
   iOS (the CP / modem part is what matters). `cell-provision` then copies that region's modem on the clean install.
2. **NV (calibration, IMEI-related settings):** comes from your phone's own EFS. Never copy someone else's
   `nv_data.bin`, and never write EFS.
3. **Data APN:** Jio's `jionet` is the default. For another carrier write its APN into `/var/root/cell-apn` (one
   line) and restart the modem (airplane mode on, then off):
   `ssh s20 'echo airtelgprs.com > /var/root/cell-apn'`. Without that file: `data <apn>` on the command pipe
   `/var/tmp/redwood-cell/cmd` sets it until the next restart. Look up your carrier's APN (Android's
   `apns-conf.xml`, or the carrier's own page).
4. **IMS / VoLTE:** the IMS PDN uses the standard APN `ims` (almost every VoLTE carrier). The P-CSCF comes from the
   network (PCO), so nothing is set per carrier. A carrier that needs another IMS APN, or a VoLTE profile the
   modem firmware does not have, is not supported yet. Kill switch: `touch /var/root/cell-noims`.
5. **Mobile data off:** `touch /var/root/cell-nodata` (kept across restarts).

## When it still says "no modem"

- `ssh s20 'tail -40 /var/tmp/redwood-cellular.log'`: "modem files missing", "cell-provision exit N", or a BOOT error.
- `error -2 ... phase 0` right after a restart: the previous run was killed before it powered the modem down, and
  the service refuses to start a modem that is still on. Reboot (do not `launchctl kickstart -k` it mid-run).
- Airplane mode on, then off, starts a fresh bring-up (up to 3 tries 15 s apart before it stays off).

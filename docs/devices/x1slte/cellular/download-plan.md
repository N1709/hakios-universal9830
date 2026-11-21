# S20 download backend: next stage after live PCIe endpoint

Current proof (Nov12 15:08): BOOT64496B + PCIe L0 + distinct Type0 S5123 endpoint;
verified CPoff cleanup, no restart. Full firmware/ONLINE/network is unproven.
No repeated link runs are needed to start this source work. Keep SIM out.

## Board facts and queue layout

Source: own device/dts/dtbo.1.dtb, /fragment@modemif/__overlay__/cpif, not the
unrelated live-fdt dump. Only nonidentity properties decoded; private inventory
out/cellular/live/download-queue-properties.json. Samsung implementation pinned
ExtremeXT/android_kernel_samsung_exynos990@69515fbb7a4395898c05a8624f76a12afbac11c5,
drivers/soc/samsung/cpif/ and drivers/pci/controller/dwc/.

Protocol0=SIPC, link_type1=PCIe, ipc_version0x32, link_attrs0xfc9. DPRAM magic,
legacy memory BOOT/DUMP, BOOT/DUMP alignment, bootloader-SPI and SBD IPC are set;
SBD_BOOT is absent. umts_boot0 channel215/format4, attrs2 (SIPC5); no
NO_LINK_HEADER bit, so boot writes receive SIPC5 framing.

All offsets/sizes below are hexadecimal, relative to IPC AP reservationF0000000+8MiB:

| Object | Offset/size | Source meaning |
| --- | --- | --- |
| magic/access | 0/4 | 32-bit fields; BOOT magic424f4f54, access1 |
| FMT indices | 8,c,10,14 | TXhead/TXtail/RXhead/RXtail |
| RAW indices | 18,1c,20,24 | TXhead/TXtail/RXhead/RXtail |
| FMT TX | 1000+1000 | 4KiB circular queue |
| FMT RX | 2000+1000 | 4KiB circular queue |
| RAW TX | 3000+1fd000 | Ends200000, boot/download stream |
| RAW RX | 200000+200000 | Ends400000, fits IPC reservation |

link_device_memory_legacy.c:create_legacy_link_device supplies exactly this
geometry from DT properties. init_legacy_link initializes magic/access/indices
and then writes IPC magic/access; link_start_normal_boot overrides BOOT magic
for DPRAM_MAGIC. Do not initialize queues while CP is using them.

## Actual kernel call path and limits

bootdump_io_device.c:bootdump_write adds link headers and calls ld->send.
link_device.c:shmem_send -> xmit_to_cp classifies BOOT/DUMP channels and routes
them to xmit_to_legacy_link(...IPC_MAP_NORM_RAW). This path explicitly assumes
PCI link ready and does NOT send IPC IRQ for each boot write. The producer
validates indices/free space, copies a bounded circular frame, then publishes
head with a memory barrier. CP consumes/polls it. The class/function name shmem
is shared with PCIe and does not imply the S20 MAIN payload uses AP_SHMEM image
copy ioctl. IOCTL_LOAD_CP_IMAGE routes this board's BOOT to SPI; the SHMEM-only
bulk copy branch must not be used for MAIN on this board.

SIPC5 framing parser exists (shannon.py); the inner download command/ack protocol
and allowed firmware ordering are still required. A Linux queue transport is
not itself a replacement for cbd. Never substitute Google SIT commands merely
because Pixel6 uses S5123.

Doorbell pci_db_addr14d60000 is a PCI address. s51xx_pcie_probe programs/reads
BAR0 and derives an offset before mapping root memory; BAR granularity must be
measured with BMEoff. No guessed CPU doorbell address or broad inboundATU/S2MPU
change. Existing diagnostic only configures outbound CFG0 and clears EP BME;
its result does not authorize/establish DMA mappings.

## Implementation/proof checklist

- [x] BOOT power/PIO lifecycle, live L0 and actual endpoint proof.
- [x] Root BMEoff, endpoint BMEclear-only, verified shutdown and fault models.
- [x] Derive ring geometry/header route from own DT and pinned kernel.
- [ ] Dedicated XNU backend maps retained IPC RAM with proven NormalNC attributes;
  avoid cached aliases and Device mappings for bulk memcpy. Bounds/ownership
  checks, barriers and one producer/consumer owner required.
- [ ] Trace exact PCIe DMA aperture/address translation/S2 permission grants for
  these reserved ranges. Preserve UFS and unrelated HSI2 clients. Keep BMEoff
  until ready; verify root/EP state, never broad-disable protections.
- [ ] Recover/verify S20 cbd download command/ack formats and BOOT/MAIN/APM/VSS
  ordering (source or private binary analysis). Validate signed firmware/CRC
  metadata. No guessed firmware addresses, NV mutation or partition writes.
- [ ] Implement small command/ack transaction with timeout and crash/shutdown path;
  prove CP consumption/response before a full MAIN transfer.
- [ ] Load remaining required firmware, bounded handshake and true modem ONLINE.
- [ ] Samsung IPC control/SIM queries, then request Jio SIM insertion when useful.
- [ ] Data-session state, packets/interface/routes/DNS and cellular-only HTTPS.
- [ ] IMS/VoLTE/call/audio is a separate remaining stage.

Batch any required kernel additions into one notified restart only after the
backend and host checks are ready. Continue source/debug preparation live.

## Related references evaluated

The [S7 boot/RIL prototype](https://gist.github.com/tonyg/4ea14f4dfe414422c0648c6e0a8bcb5d)
shows an older shared-memory loader and finishing handshake. Its SHMEM uploads
and security ioctls do not establish S20 PCIe download compatibility. No code
copied or commands executed from it. The [S5123 code skeleton](https://github.com/grant-h/shannon_S5123)
is a possible navigation aid, not a working loader. A Samsung S5123 module
manual surfaced at FCC report5137238 but fetch returned403; its full contents
have not been reviewed, so no implementation depends on the search excerpt.

## Priority clarification and queue core — Nov12 continuation

User priority: 4G data, SMS and ordinary calls. Deliver data first and test SMS
as soon as modem control is available. Ordinary Jio LTE calls still require
IMS/VoLTE internally; hiding the VoLTE label does not remove that dependency.
SMS routing also needs carrier verification; do not assume an IMS-free route.
[Jio's LTE explanation](https://www.jio.com/help/faq/mobile/services/high-speed-internet/what-is-4g-lte-service-provided-by-jio/)
identifies VoLTE as its voice transport. Progress25–35% is a rough engineering
estimate for the full requested outcome, not measured completion or a deadline.

New s5123-ring.h provides bounded single-owner producer/consumer operations,
board RAW queue geometry, index/range validation, split wrapping copies and
explicit acquire-load/release-publish callbacks. A failed publish returns
COMMIT_UNKNOWN and must be resynchronized, never blindly retransmitted. Partial
copy failure leaves the published index unchanged. No hardware mapping, protocol
payload, DMA enable or modem command is included. Backend still pending.

Host ASan/UBSan test passed7680 independently enumerated position/length/direction
cases, wrap/full/empty behavior, partial-copy failures, uncertain publication,
malformed peer indices and overflow/metadata-overlap guards. Integrated into
check.sh. This is host proof only; no new phone operation/restart.

Source investigation: stock-g980f contains an encrypted972MiB archive left from
an earlier download; it has not been validated as complete or usable. No cbd/RIL
binary found in the extracted vendor firmware inventory. Do not treat that
archive as a recovered working loader or execute guessed legacy S7 commands.

## XNU cache audit and loader handoff — Nov12

Built, host-tested, not installed. New root debug command `cp.memory` requires
three exact 64-bit `/chosen` values from the loader: memory-version1, IPC base
F0000000 and size800000. The loader emits these only after rechecking actual
CP reservation/cache state. The audit checks the kernel physical aperture,
then translates each of2048 existing direct-map pages via AT S1E1R/PAR_EL1.
It reports NormalNC44, other cache types, faults, wrong PA and prior generic
debug aliases. It never reads/writes IPC contents, creates mappings, grants
DMA access or powers the modem. Missing certificate refuses access.

`ml_io_map_wcomb` is unsuitable for the whole8MiB IPC: it allocates from the
shared8MiB io_submap with KMA_NOFAIL. Use the existing Wi-Fi driver's approach,
`ml_io_map_unmappable(..., VM_WIMG_WCOMB)`, which uses kernel_map instead.
The XNU MAIR entry is44 (Normal outer/inner noncacheable). Before doing so,
resolve any incompatible existing alias; the audit is evidence, not permission
to ignore cached aliases. No hardware IPC backend is claimed yet.

Samsung s5100_poweron_pcie grants per-region access with
exynos_set_dev_stage2_ap("hsi2",0,base,size,ATTR_RW), excluding MSI. Its helper
uses HVC0xc6000100 and packs `(vid<<16)|instance_index`; own boot DT and Samsung
DTS identify HSI2 as25. Existing Wi-Fi uses HVC0xc6000101/HSI1 index24/VID1,
so those arguments must not be copied. No cellular HVC or DMA grant issued.

Validation: isolated kernel relink and loader build pass. All42 Python tests
and C sanitizer suites pass, including all2048 audit-address cases and cache/
fault/certificate rejection. Build logs in research-root build/
cellular-memory-build-final.log/.exit (0), loader log in private out/cellular/
live/memory-loader-build.txt. Current phone remains on the previous proven
SPI8/reservation base; this audit awaits a batched notified restart.

## External SIPC loader recovered from source — Nov12 continuation

Pinned upstream: [Samsung cpboot_v3 via LineageOS,27ec634140f0e8798fc626fa2f6f4a081dbd23cf](https://github.com/LineageOS/android_hardware_samsung_slsi-linaro_exynos/tree/27ec634140f0e8798fc626fa2f6f4a081dbd23cf/cpboot_v3).
ProtocolSipc.cpp/.h, Protocol.cpp and BootDumpExt.cpp carry Samsung2020 Apache2
notices. The [S20 common board configuration](https://github.com/LineageOS/android_device_samsung_universal9830-common/blob/0d6675d52807f129d624bc0d671dcdf6d7af71ce/device-common.mk)
builds cbd with SIPC. More importantly, our own extracted init.baseband.rc invokes
`cbd -t s5100 -bs -me -B umts_boot0` (SPI BOOT, PCIe MAIN), matching BootDumpExt
and the device kernel/DT. This is a source-derived compatibility case, not proof
that our CP has accepted these commands. No full vendor download is required
for this protocol investigation; the partial encrypted archive is untouched.

Actual RADIO table validates BOOT/MAIN/APM/VSS/INFO CRCs. TOC's final word is
count7; the per-image final words are not used to assign stages. BootDumpExt
moves BOOT to stage0 and starts the queue loop at TOC1. It stops building at
OFFSET (index6), places FIN there, and does not send trailing INFO. Our profile:

| Stage | Image | Bytes | 2048-byte fragments | Host sequence |
| --- | --- | ---: | ---: | --- |
| 0 | BOOT | 64496 | SPI, already proven | Not repeated in UDL loop |
| 1 | TOC | 800 | 1 | START, SEND image, DONE |
| 2 | MAIN | 59502592 | 29054 | START, SEND image, CRC, DONE |
| 3 | APM | 29728 | 15 | START, SEND image, DONE |
| 4 | VSS | 4827808 | 2358 | START, SEND image, DONE |
| 5 | NV | 1048576 | 512 | External source unresolved; blocked |
| F | FIN | 0 | 0 | START only, after all earlier stages |

UDL command is little-endian u32: request9000 or responseA000, OR stage<<8,
OR operation START0 / SEND1 / CRCc / DONEd (FAILf). Data contains four u32 words:
command,total-frame-count,one-based-current-frame,payload-length, then up to2048
bytes. One SEND acknowledgement follows the complete image, not each fragment.
CRC message is command+CRC, MAIN only in this external loop. Each write is
wrapped by our kernel's ordinary4-byte SIPC5 header, channel215, length excluding
padding to8 bytes. First intended live transaction is TOC START9100 -> A100;
its full TX bytes are `fc d7 08 00 00 91 00 00`. Do not send before IPC init,
cache mapping and DMA prerequisites. No UDL bytes have been sent to the phone.

New original MIT s5123-udl.h encodes these wire facts (no upstream implementation
copied), checks exact chunk sizes/stages/output bounds, and rejects malformed,
failed or unexpected ACKs. No automatic retransmission or device I/O. C sanitizer
tests pass golden vectors, all2048 final lengths and padding/capacity guards,
UINT32_MAX image arithmetic and malformed replies. udl_plan.py reads RADIO only,
CRC-verifies, requires the reviewed exact profile, emits stage metadata, and
explicitly blocks NV/FIN;3 plan tests pass. It never opens EFS/NV. Existing
kernel SHMEM transport facts remain applicable; MODAP/SIT paths are not used.

Additional mapping evidence: latest known-good loader log puts XNU RAM at
880000000+17e000000, wholly above the IPCF0000000+800000 range. Thus the new
cp.memory command is expected to report outside physical aperture without
calling phystokv. Future mapping should require certificate, disjoint kernel
aperture, no generic-debug alias, then validate every new NormalNC translation.
This narrows the alias issue; it is not yet a mapped IPC/backend proof.

## Live firmware download, first attempts — Nov12 23:15-23:35 (session A, codex-cellular out of usage)

Installed (two notified restarts, root/camera/GPU untouched): the reserving loader again (the 21:21 loader
had been built without CELLULAR_RESERVE_CP: "reserved 3/3, normal-nc 3/3" back) and a kernel with
`kern.redwood.poke "cp.s2mpu"`: HVC 0xc6000100, (VID0<<16)|HSI2 index 25, RW, for f0000000+800000 and
d0000000+4000000 (not the MSI page f6200000), as s5100_poweron_pcie. Only with the loader certificate.

`live-ipc --map-check` PASS (2048 Normal-NC pages, unmapped). `live-ipc --dump/--unmap` added.
`live-modem --reserved-base-verified BOOT INFO --download TOC` (tools/cellular/live-modem.c): after SPI
BOOT, init_legacy_link + BOOT magic (424f4f54, access 1, indices 0); after L0: cp.s2mpu, root and EP
command MSE|BME, EP BAR0 = pci_db_addr 0x14d60000 (reads 14d00004: 64-bit BAR), then TOC START
(fc d7 08 00 00 91 00 00) into RAW TX, head 0->8, poll RAW RX; bus master off again at the end.

Results, twice, CP shut down after (second time by hand, see below):
- SPI BOOT 64496 B OK, GPIO first 2 last 6 (WAKE high from the start, PS_HOLD rises; PHONE_ACTIVE never).
- PCIe L0, endpoint a5a5144d; **hypervisor grants both regions: ret 0**; BME on both ends verified.
- **CP never consumed**: RAW TX tail stays 0, RX head 0, nothing written anywhere in the header (dump).
- The second run "hung": the RX wait counted usleep(2000) iterations, which sleep far longer on this
  kernel -- fixed to a clock deadline. The process was killed; reset+power were set low by hand
  (m 0x15c30124/0x15c30284 bit0 = 0), IPC unmapped with --unmap, then a plain restart reset PCIe/SPI.

Not yet done that Samsung does before the UDL loop (next hypotheses, cheapest first, all live):
1. EP MSI (cap 0x50/0x54/0x58) pointed at the RC's MSI target; CP may wait for it before polling.
2. RC setup from dw_pcie_setup_rc: bus numbers (0x18), RC outbound MEM ATU for the BAR window, so the
   AP->CP doorbell (s51xx_pcie_send_doorbell_int, int_pcie_link_ack) can be written; Samsung sends one
   on every power-on after the first registration.
3. Wait for CP2AP_WAKEUP edge after the magic (check_cp_status) before link; WAKE is high from the
   first sample, so it may be a pull-up, not the CP.
4. Read the S5123 BOOT image's strings/handshake (private analysis) for what it waits for.

### MILESTONE Nov12 23:40: the CP answers -- TOC stage acknowledged

The missing step was MSI, as Samsung's exynos_pcie_rc_msi_init does for EP_SAMSUNG_MODEM: RC DBI
0x13800820/824 = f6200000/0 (the shm MSI page), 0x828 (INTR0 enable) = 0xf1, and the endpoint's MSI
capability (0x50: control 000a7005, 32-bit) address 0x54 = f6200000, data 0, enable bit16. With that,
right after it the CP consumed TOC START and replied in RAW RX:
  START  fc d7 08 00 00 91 00 00 -> fc d7 08 00 00 a1 00 00 ACK
  SEND   800 B TOC, one frame    -> ...01 a1 ACK  (TX head/tail 340)
  DONE                           -> ...0d a1 ACK  (TX 348, RX 18)
CP shut down after, cleanup_verified 1, no restart. Transcript: logs/cellular/a-download-*/ (private).
Next: MAIN (59 MB, 29054 frames, + CRC), APM, VSS, NV (nv_data.bin from the Mac's EFS backup image),
FIN START; then the CP should boot its main firmware (complete_normal_boot waits for its init on IPC).

### MILESTONE Nov12 23:45: full firmware download, main firmware running (PHONE_ACTIVE high)

`live-modem --download-all TOC PLAN` (PLAN lines "STAGE FILE OFFSET SIZE CRC", "15" = FIN), files staged
in /var/root (cell-radio.img = the RADIO backup, cell-nv.bin = nv_data.bin from the Mac's EFS backup):
  TOC 800 B ACK; MAIN 59502592 B / 29054 frames queued in 1.5 s, SEND ACK, CRC 7c455a85 ACK, DONE ACK;
  APM 29728 B ACK; VSS 4827808 B ACK; NV 1048576 B ACK; FIN START ACK.
Then GPIO 6 -> 7 within 500 ms: **PHONE_ACTIVE high and held for 10 s** = the CP's main firmware is up.
IPC header unchanged (magic still BOOT; RAW TX/RX 1794a8/88, all consumed). CP shut down after
(cleanup_verified 1), no AP restart.

Next (Samsung's complete_normal_boot and after): wait for the CP's init message (cp2ap msg / MSI at
f6200000; we have no MSI handler yet -- poll the MSI page/INTR0 status and the FMT RX queue), switch
the link to IPC (magic MEM_IPC_MAGIC, FMT/RAW queues), then Samsung IPC (SIPC FMT, RIL protocol) for
SIM status (SIM is out: expect "no SIM"), then radio power / registration, data (RAW channels),
SMS, calls. Keep the CP off between runs; nothing here needs an AP restart.

### MILESTONE Nov13 00:05: Samsung IPC handshake complete (INIT_START / PHONE_START / INIT_END)

After FIN (polled, no MSI handler needed): control words at IPC+0x400 ap2cp_msg, +0x404 cp2ap_msg,
+0x408/+0x40c united status (DT DRAM_V1; int = 0x80|0x40|cmd). Before boot ap2cp_united = ds_det 2<<14.
Doorbell AP->CP: iATU outbound region1 (DBI+0x300200: MEM 14d00000..14dfffff -> 14d00000 = EP BAR0),
write 0x10000+n to 0x14d60000 (pci_db_addr; DOORBELL_INT_ADD 0x10000, int_ap2cp_msg 0).
Seen live: cp2ap 0x83 (MSI status 1), then 0xC1 INIT_START -> AP 0xCD PIF_INIT_DONE + doorbell ->
CP 0xC8 PHONE_START -> AP: queues zero, magic 0xAA, access 1, 0xC2 INIT_END + doorbell. CP stops
repeating PHONE_START = accepted. MSI from CP arrives (RC 0x13800830 status bit0).

NEXT (blocking IPC): link_attrs 0xfc9 has LINK_ATTR_SBD_IPC; umts_ipc0 (ch 0xEB, FMT, ul 16x4K,
dl 32x4K), umts_rfs0, csd, router, dm0, cass ... carry attrs 0x82 = SIPC5|SBD_IPC. So FMT/RIL traffic
goes over SBD rings: create_sbd_link_device + init_sbd_link (link_device_memory_sbd.c, 616 lines)
must write the descriptor region into shmem at INIT_START, before PIF_INIT_DONE (we skipped it).
rmnet* (attrs 0x180) = packet processor (pktproc) for data. Port SBD layout to the live helper next.

### SBD rings, first live tries — Nov13 ~06:00 (session A)

live-modem now writes Samsung's SBD layout at INIT_START (before PIF_INIT_DONE), from this board's DT
iodevs (create_sbd_mem_map/init_sbd_link): 10 link channels in DT order -- cass 35, ipc0 235, ipc1 236,
rfs0 245, csd 1, router 25, dm0 28, loopback 244, multipdp_hiprio -> QoS ch 10, multipdp -> 11 (rmnet*
PS channels and the kor_skt/usa_spr option-region smd4/ciq0 skipped). g_desc at IPC+0x800 (version 1,
10 channels, rpwp_array_offset 0xf0c), buff_pos/len arrays to 0x85dc, buffers 0x10000..0x7a8000.
ap2cp_united = ds_det 2<<14 | ap_status 1<<1 | pda_active 1<<5. One request sent on umts_ipc0 UL:
SIPC5 f8 eb 0c 00 + IPC fmt (len 8, mseq 1, MISC 0a ME_VERSION 01 GET 02, ff), ap2cp 0x81 + doorbell.

Seen: CP keeps raising MSI after INIT_END (cp2ap_msg stays c8 -- data MSIs or PHONE_START repeats;
INIT_END resent up to 5x does not change it). ipc0 UL rp stays 0 (request NOT consumed). The 4th rpwp
block (DL wp) gets values that are not ring indices (256, ~10264 rising, 13184, 19918, 4096, 65535)
while the DL buffers stay zero: the CP writes some structure of its own there, i.e. our SBD layout does
not match the CP's (version, table sizes, or a region offset). dm0 DL wp=4 looked valid but its buffer
is zero too.

Next checks (live, no restart): read back the region after FIN and before INIT_START to see what the CP
itself writes (it may publish its own layout/version); compare with Samsung's g_desc for SIT/other
versions (sbd.h variants), try placing the SBD area elsewhere (mld->base may not be the IPC base for
SBD: check cp_shmem_get_nc_region / SZ_2M split in link_device.c ~3926) -- the 2 MB "nc region" hint
suggests the SBD base could be IPC+0 but sized 2 MB, or a different SHMEM index. Loop bug fixed: the
DL dump skips out-of-range indices (one run spun; tool killed, CP held off by hand, restarted).
Raw dump after INIT_END (IPC+0x400): ap2cp c2 | **cp2ap 0x83** (INT_VALID|SEND_FMT|SEND_RAW: "data for
you", so the MSIs are data notices, not PHONE_START repeats) | ap2cp_united 0x8022. At IPC+0xf48 the CP
wrote 20 bytes of its own: 00 01 17 28 80 33 ce 4d e0 33 ce 4d 04 00 00 10 ff ff ff ff -- inside our DL
wp array (0xf0c + 3*20). Legacy queue indices stay 0, SBD DL buffers zero. Our buff_pos arrays (from
0xf5c) are intact. => find what the CP places at 0xf48 (a CP-side descriptor at a fixed offset? then our
rpwp array must not sit there: compare the S20's real g_desc size -- if the stock cpif has
MAX_SBD_LINK_IDS 32 but an extra field, the rpwp array moves). Next: decode those 20 bytes, read the
stock S20 kernel's cpif (Samsung opensource G980F kernel, not ExtremeXT) sbd.h.

### MILESTONE Nov13 ~06:40: the modem talks Samsung IPC on umts_ipc0

The missing piece was the packet processor v2 info block (stock S20 config: CONFIG_CP_PKTPROC_V2=y,
JUMBO off, MODEM_IF_QOS=y): cp_rmem_2 (rmem_index 2) = d0000000+64MB = SHMEM_PKTPROC, CP view 0x28000000.
At INIT_START, before the SBD rings and PIF_INIT_DONE: info[0] = num_queues 1 | mode 1 (SKTBUF) <<4 |
max_packet 2048 <<8 = 00080011, q_info[0] = desc_base 28000100, num_desc 0x1933 (6451), data_base
28100000, fore 0, rear 0. With that, all SBD channels come up; DL umts_ipc0 (ch 235) frames:
  f8 eb 1d 00 | 19 00 00 00 13 05 03 0f 00 "\r\nAST_POWERON\r\n"
  f8 eb 0c 00 | 08 00 01 00 01 01 03 ad          PWR_PHONE_PWR_UP noti
  f8 eb 0d 00 | 09 00 02 00 0d 0f 03 06
  f8 eb 0f 00 | 0b 00 03 ff 26 01 01 00
  f8 eb 15 01 | 11 01 04 01 0a 01 02 ...         MISC_ME_VERSION response to our GET: "G980FXXSNHYB1 ... REV0"
(SBD buff_len_array values are not lengths here; use the SIPC5 length.) The legacy-FMT duplicate request
is not used by this CP (TX tail stays 0). Next: SIM status (SEC group), then radio power / registration.

### MILESTONE Nov13 ~06:50: SIM read and network seen (40 s live run, SIM inserted)

Decoded umts_ipc0 traffic (Samsung IPC: len u16, mseq, aseq, group, index, type; type 3 = NOTI, 2 = RESP):
- 13/05 "AST_POWERON"; 01/01 PWR_PHONE_PWR_UP; 26/01, 0d/0f early notices; 0a/01 ME_VERSION response.
- SIM: 05/13 carries the card ATR (3b 9f 96 80 1f c6 ...), 05/11, 05/10, 05/07, then 05/01 SIM status
  notices (00 00 / 82 00 alternating). Our SEC_SIM_STATUS GET (05 01 02) got GEN_PHONE_RES 80/01
  "05 01 02 03 80" -- error/unsupported form; the CP reports the SIM by itself anyway.
- NET (08): current PLMN notices with the Jio PLMN (405-8xx), NET_REGIST (08/05) updates, 08/3c, 08/19;
  DISP (07/06, 07/01) signal/icon info; 27/01, 0d/19. The CP powers RF and camps on LTE by itself after
  PWR_UP (stock NV behaviour); it is shut down at the end of every run (cleanup_verified 1).
Logs with SIM/cell identifiers stay private (logs/ and out/ are gitignored); do not copy them into docs.

Next: a long-running daemon instead of the 40 s test (keep CP up, rings serviced, MSI driven), decode
SIM status / NET_REGIST properly (libsamsung-ipc tables as a guide), then data: PDP context (GPRS group
0x0d) + pktproc RX/TX path -> a utun interface in iOS; SMS (group 0x04); calls/IMS last.

### Nov13 ~08:10: RFS server, AT channel, first data-call attempts (session A)

- Service (`redwood-cellulard`, launchd `com.redwood.cellular`) has a control FIFO /var/tmp/redwood-cell/cmd:
  `at <cmd>` (umts_router ch 25) and `fmt <grp> <idx> <type> <hex>` (Samsung IPC on umts_ipc0).
  Send ONE line per write after the FIFO exists (a write to a stale FIFO with no reader blocks).
- RFS server (after postmarketOS Note 8 mifmon.c / libsec-ril): byte stream over ch 245 in 2044-byte
  SIPC5 frames (not multi-frame headers). Seen: NV_WRITE 154330 B @0x80000 and 30394 B @0 (applied to the
  private /var/root/cell-nv.bin, never the EFS partition), OPEN "FLL.bin" (answered ENOENT; the EFS backup
  has root/FLL.bin -- serve the real file next).
- AT: "AT" -> OK. Any further command only echoes (AT+CPIN?, AT+CGDCONT=..., AT+CGATT=1): the AT engine
  stalls after the first command, with or without RFS answered first. Unknown yet why (Note 8's CP
  answered CPIN/COPS/CGATT).
- IPC data call (Note 8 layouts): 0d01 DEFINE (127 B: 01 cid 06 apn[100]...), 0d04 ACTIVATE (105 B: 01 cid)
  -> GEN_RES 0x8000 OK both, then GPRS_CALL_STATUS 0d10 "01 03 00 00" (cid 1, state 3 = not active, cause 0);
  no 0d09 IP config. Same with APN "jionet" or "", and after IPC PS attach 0d03 (0x8000). Note 8 saw the
  same: the CP only attaches for data after AT+CGDCONT + AT+CGATT=1. PWR 01 07 SET 0202 -> 0x8001 refused.
- NET_REGIST 08 05 data "21 03 07 ...": status field unclear (libsamsung-ipc numbering does not fit).
Next: make AT work past the first command (check the router UL/DL ring indices after each command, try
"\r\n" endings, check whether results arrive on another channel, e.g. umts_csd/ipc1), serve real EFS files
(FLL.bin), then AT attach -> IPC activate -> 0d09 IP config -> packet path (pktproc DL, multipdp UL) -> utun.

### Nov13 ~08:50: exact S20 message layouts from the stock radio library (session A)

Source: the user's local stock firmware G980FXXSNHYB7 (CP G980FXXSNHYB1 = our modem): AP tar -> super.img.lz4
streamed (LZ4 + sparse) -> vendor (ext4, super bytes 6268387328..7727595520) -> lib64/libsec-ril.so, bin/cbd,
kept private in out/cellular/private-ril (gitignored; never commit). Disassembled with llvm-objdump:
- Radio power: group 01 idx 07 **type 01 (EXEC)**, data 02 02 (on) / 01 (off). (SET -> 0x8001; EXEC -> 0x8005 =
  already on.)
- LTE attach profile (IpcTxSetLteAttachProfile): 0d 14 SET, 337 B frame / 330 data: d0 mode 1, d1 profile id,
  d2 pdp type (IPv4v6 = 6), d3..102 APN, user @frame 0x86, pass @0xea, d[0x147] auth, d[0x148] roaming type.
- PS attach (IpcTxPsAttach): 0d 03 SET, 10 B: 01 00 00.
- Define (IpcTxDefinePdpContext): 0d 01 SET, **149 B** frame / 142 data: 01 cid pdp apn[100] ... (Note 8: 134).
- Activate (IpcTxSetPdpContext): 0d 04 SET, **248 B** / 241 data: d0 = 1 (activate; jump table by request),
  d1 cid, d2..5 u32 user/pass flags, user @frame 0xd, pass @0x71 (Note 8: 112).
- If GetModemType()==0x40 the RIL routes data calls through another protocol object (IIL variant, same
  0d01/0d04 headers and sizes).
Results: all accepted (0x8000). Sent before the network attach (fresh CP), the attach profile changes the
registration notices (08 05 ... "21 03 07" -> "21 03 01" -> "21 01 02"), but define+activate still give no
0d09 IP configuration and no new 0d10 within ~60 s. With the old sizes the CP answered 0d10 "01 03" (cid 1,
not active) at once. Next: decode IpcRx for 08 05 (NET_REGIST) and 0d 10 from the library to know the PS
state; find what the RIL sends between attach and setup-data-call (IpcTxSetDataCallEstablish 03 02? data
profile 0d 1b?), deactivate before re-activate; also serve real EFS files (FLL.bin).

### Nov13 ~09:30: data-call results decoded (session A)
- GPRS_CALL_STATUS 0d10 data: cid, state (1 active, 3 disabled), cause byte. Cause 2 -> RIL DataFailCause 36
  (REGULAR_DEACTIVATION) via the converter at libsec-ril 0x371f44 (table 0x153592). With the correct sizes
  (define 149 B, activate 248 B) the CP answers 0x8000 and then stays silent until the next define/
  activate, which ends the previous context with cause 2 -- no 0d09 IP configuration or "active" status.
- 03 02 SET (IpcTxSetDataCallEstablish, data 01 allow / 02 disallow) -> 0x8003 unsupported (same as Note 8).
- PDP type bytes: 02 IPv4, 05 IPv6, 06 IPv4v6 (define builder table 0x040605).
- The CP is truly registered: NITZ (0a 05: yy mm dd hh mm ss + PLMN), JSON serving-cell info (0b 0d
  {"JVER":"SSB2","PLMN":"405854","ACT_":"4",...}), 0a 52 periodic counters, 0f 36 toggling 00 03 / 01 03.
- Likely gaps: the success report may come through the JSON data protocol (IpcProtocol41JsonData::
  IpcRxIpConfiguration / IpcRxGprsCallStatus -- which group/index?) or needs a PDU-session/CID allocation
  step (DataCallManager::DoAllocatePduSessionId) or QoS/data profile (0d 1b). Next: find JsonData's rx
  group/index and the RIL's exact SETUP_DATA_CALL message order (DataCallManager handlers).
- JSON variant tried (IpcProtocol41JsonData uses JsonUtil::MakeJsonPacket(buf, grp, idx, type, json); data =
  09, u16 len, JSON, padded to 512): define {"cid":1,"pdp_type":6,"apn":"jionet"} -> 0x8001 refused; activate
  {"cid":1,"state":1,"auth_type":0,"reason":0} -> no answer. => this CP takes the binary define/activate.
- Open question: what makes the CP complete the binary activation (it accepts, then stays silent until the
  next request closes it with cause 2 = REGULAR_DEACTIVATION). Candidates: PS attach state (IpcRxGprsPsState
  -- which index? not yet seen), APN/IMS dummy PDN (DataCallManager::SetupDummyPdn), PDU session id
  allocation (DoAllocatePduSessionId), or a data profile (IpcTxSetDataProfile 0d 1b) before setup.
- GET probes: 0d04 GET -> "01 14 00"; 0d10 GET -> cid 1 state 3 cause 2 (ignores a cid argument);
  0d09 GET -> 0x8001 (not a GET). IpcRxPdpContext / IpcRxGprs* check ModemType()==0x40 and
  IsSupportJsonCmd(): per command, JSON is used only if the CP announced it (JsonUtil::SaveSupportJsonCmdList)
  -> find that announcement in the CP's startup messages; it decides the format of each data/SMS command.
- SMS/calls on Jio: LTE-only network -> SMS over IMS and VoLTE; on Samsung the IMS client runs on the AP
  (not in the CP). So Jio SMS needs data first, then an AP-side IMS (SIP) client. Plain CP SMS (IPC 04 xx)
  only helps on networks with SMS over SGs/CS fallback.

### MILESTONE Nov13 ~10:00: LTE data call ACTIVE (Jio, IPv6)
What made it work: (1) the packet processor got free RX buffers (SKTBUF mode: descriptors at d0000000+0x100
filled with data_addr = 0x28100000 + i*2560 + 64, HEAD on desc 0, fore_ptr = n-1 in q_info; 256 descriptors),
(2) the S20 message sizes (attach profile 0d14 330 B, define 0d01 142 B, activate 0d04 241 B), and
(3) **cid 2** -- cid 1 answers state 3 at once (it is the attach PDN). PS attach 0d03 -> 0x8005 (already attached).
Result: 0d09 IP configuration (313 B) and 0d10 "02 01" (cid 2, active). Layout of 0d09: d0 cid, d1 flags 0x42,
d2 0x21, IPv4 none (Jio is IPv6-only), IPv6 address @0x23 (16 B), IPv6 DNS @0x63, MTU u16 @0x98 (1500).
Service FIFO: `data [apn]` sends the whole sequence (currently cid 1 -- use cid 2; fix next).
Next: packets. DL: CP fills pktproc descriptors (rear_ptr @ d0000000+0x14), AP reads and refills (fore_ptr).
UL: raw IP into the multipdp SBD ring (id 9, ch 11 = rmnet1 = cid 2), buff_len = len | ch<<16, then
MASK_SEND_DATA. Then a utun interface in iOS with the IPv6 address, default route and DNS.
- Packet path, first tries (Nov13 ~10:40): service FIFO `data` (attach profile + PS attach + define/activate
  cid 2) brings the PDN up reliably (0d09 parsed: IPv6, DNS, MTU). `ping6 <addr>`, `rs` (router solicitation)
  and `pch <ch> [ring]` added. UL: the CP consumes our raw IPv6 packets from the multipdp ring 9 (rp/wp
  advance) with ch 10, 11 or 12 in buff_len bits 16+. DL: nothing -- pktproc rear_ptr stays 0, SBD DL rings
  0, even after RS (an RA must come back). DT cp_shmem confirms PKTPROC = rmem 2 (d0000000) offset 0,
  64 MB, cached=1 (and pktproc_use_hw_iocc=1). Next suspects: (1) the UL never leaves the CP (check with
  the CP's own data counters / 0d message after UL), (2) DL needs IO coherency (HSI2 IOCC sysreg as the
  Samsung PCIe driver sets for use_hw_iocc) or the RINGEND/HEAD layout, (3) pktproc needs an MSI/doorbell
  "AP ready" kick (s51xx doorbell for pktproc), (4) DL delivered through a different queue index.

### MILESTONE Nov13 ~11:00: LTE packets both ways (ping6 to Jio's DNS answered)
UL on the multipdp QoS high-priority SBD ring (id 8, QoS ch 10) with buff_len = len | (11 << 16) (rmnet1 =
cid 2); ring 9 is consumed by the CP but nothing goes out. DL arrives through the packet processor (rear_ptr
advanced, descriptor len 64, ch 11, status e1, data in our buffer). Also: pktproc info is now written before
the CP boots (as pktproc_create at probe) and again at INIT_START. Service FIFO: data, ping6, rs, pch, dm, stat.
Next: utun in iOS (IPv6 address, default route, DNS) carrying packets both ways; then make `data` automatic.

## 2026-11-13 — LTE data usable by iOS

- `redwood-cellulard` opens a **utun** (`com.apple.net.utun_control`), gives it the PDN IPv6 /64 and the modem MTU, adds `::/0` through it, and moves packets: utun → SBD ring 8 (QoS ch 11) uplink; pktproc descriptors → utun downlink.
- Bug fixed: the kernel peek (`kern.redwood.poke r PA N`) returns at most **64 words**; packets over 256 bytes were silently dropped (TCP connected but no data). `poke_read` now reads in 64-word chunks.
- **configd**: publishes `State:/Network/Service/redwood-cell/{IPv6,DNS}` (Router = own address, as VPN services do; `"::"` was rejected). IPMonitor then makes it `PrimaryService`, installs the Jio resolver (2405:200:800::1) and the default route — apps need nothing special.
- **Automatic data**: once a PLMN is known the service sends `data` (APN jionet, cid 2), retried every 20 s up to 5 times. Kill switch: `touch /var/root/cell-nodata`.
- Verified: `cell-get ipv6.google.com` → 200 OK, 94 KB; `cell-get github.com` → DNS64 `64:ff9b::87eb:9590` → 301 (IPv4-only sites work through Jio NAT64).
- Test tools (phone /var/root): `cell-http IF ADDR HOST [PATH]` (pinned to one interface), `cell-get HOST [PATH]` (app-style), `sc-dump [regex]` (tiny scutil).
- Throughput is limited by the sysctl peek path (one sysctl per 256 bytes); a kernel-side copy or direct mapping is the next speed step.

## 2026-11-13 — SMS receive, mobile-data switch (service side)

- SMS (group 04, decoded from libsec-ril IpcProtocol41Sms): CP NOTI `04 07` (device ready) → answer `04 07` SET (empty) **once** (answering every NOTI loops: 621 in a minute). Incoming `04 02` NOTI: `{msg_type, type, u16 sim_index, tpid, len}` + PDU (SMSC first) → decoded (SMS-DELIVER, GSM-7/8-bit/UCS-2, UDH skipped), logged, appended to `/var/root/cell-sms.txt` (`time<TAB>from<TAB>scts<TAB>text`), delivery report `04 06` EXEC `{msg_type, u16 0, tpid, 0}`. Not yet verified with a real incoming text (Jio may deliver only over IMS).
- Mobile data switch: FIFO `nodata` (utun closed, configd service removed, `/var/root/cell-nodata` kept across restarts) / `data`. State file has `data_on 0|1`. FIFO is now mode 0622 so SpringBoard/Settings (user mobile) can write it.
- Settings/Control Center: both use CoreTelephony C calls `_CTServerConnectionGet/SetCellularDataIsEnabled(Ex)` (→ CommCenter, absent). Plan: entry-trampoline hooks (as inject_files.c) in libredwood-sb + Preferences, answering from `data_on` and writing `data`/`nodata`; each behind its own boot arg.

## 2026-11-13 — SMS on Jio needs IMS (first probe)

- A text sent to the phone never produced `04 02` (incoming SMS) from the CP: Jio has no SMS over SGs/NAS here; SMS goes over IMS (SIP MESSAGE), which on Samsung Exynos is an AP-side stack.
- New FIFO command `pdn CID APN` (define 0d 01 + activate 0d 04, no attach-profile change). `pdn 3 ims` (v4v6) and a hand-sent IPv6-only define were both answered `0d 10: 03 03 ... 05` (cid 3 disabled, cause 05). Non-cid-2 ipconfig (0d 09) is now dumped in full for P-CSCF hunting.
- Next: find what cause 05 means and how the stock RIL brings up the IMS PDN (libsec-ril ImsPreference / IMS PDN path; maybe the attach profile must be `ims` with jionet as the second PDN, as Jio VoLTE phones do), then P-CSCF from PCO, SIM AKA via IPC, SIP REGISTER (+IPsec), SIP MESSAGE for SMS.

## 2026-11-13 — IMS: PDN, P-CSCF, SIP reachable; REGISTER -> 403

- IMS PDN comes up by itself on cid 3 (`ims` command / auto after data): define `0d 01` with **d[0x7e] = P-CSCF-via-PCO** (1 v4, 2 v6, 3 v4v6; found in libsec-ril IpcTxDefinePdpContext). ipconfig then has the P-CSCFs at 0x33/0x43 (Jio: two IPv6 P-CSCFs), the UE IMS address at 0x23. Own utun (cid 3 → PS ch 12) + /128 routes to the P-CSCFs.
- **Never deactivate** (`0d 04` with d[0]=0): the CP went down and the service restarted. **Never activate a second cid on the same APN**: cid 4 + `ims` got the same bearer and IMS downlink stopped reaching ch 12.
- SIM: IMSI `0a 02` GET; IMEI `0a 03` GET {01}; raw APDU `05 06` GET {u16 BE len, APDU}; UICC auth `05 09` GET {type, len, RAND, len, AUTN}. Answers go to root-only files, never the log. `tools/cellular/isim-read.py` opens a logical channel, selects the ISIM and reads IMPI/DOMAIN/IMPU (printed masked): they match the IMSI-derived identity, domain ims.mnc869.mcc405.3gppnetwork.org.
- Samsung's Jio profile (stock imsservice.apk res/raw/imsprofile.json, "RJIL VoLTE"): pdn ims, transport tcp, ipsec, auth_algo hmac-md5-96, UA `Samsung_[MODEL]_[BUILD]_IMS_6.0`, PANI used, `reg_retry_pcscf_policy_on_403: next_pcscf`.
- Jio P-CSCFs answer OPTIONS (200, Supported: sec-agree). REGISTER (UDP and TCP, with/without Samsung UA, IMEI +sip.instance, icsi tags, md5) → **403 Forbidden** without a challenge. Not yet tried: correct PANI (cell id from 08 05 bytes 6-9 LE, TAC bytes 10-11), fresh service (transport flaky after the cid 4 experiment), rate limits after repeated 403s.
- Transport is intermittent (TCP stalls, some UDP lost); the sysctl-per-256-bytes DL path is the suspect (kernel window for the pktproc region is the fix, needs a reboot).

## 2026-11-13 13:00 — packet window: data fast and steady

- Kernel: `kern.redwood.cp_ipc` reads/writes with offset bit 31 (`CP_IPC_PP_FLAG`) go to a 2 MiB Normal-NC mapping of the pktproc area (0xd0000000), made on first use, certificate-gated, refused while the debug peek holds a page there. Before: every packet was 8-13 debug-peek sysctls (64 words each, 16 KiB page mappings kept in a 64-entry table) -> stalls under load.
- Service: descriptors read in batches, one write back per batch, `fore` kept locally and written once per poll; falls back to the peek on an old kernel.
- Measured over cellular only (IPV6_BOUND_IF utun1): 3 MB in ~1 s, connects in ~63 ms, 3/3 OK (was 1 MB in 3.4 s with stalls).
- Boot: modem start at top priority 25 s after boot, 5 attempts 8 s apart (first attempt fails under boot load, second works); data ~40 s after the service starts.

## 2026-11-13 13:11 — IMS: the 403 was the Security-Client offer; REGISTER now draws the 401 AKA challenge (session B)

- Cause of the bare 403: our initial REGISTER offered only `ealg=null` (one tuple, with `prot=esp;mod=trans`). Jio's P-CSCF
  refuses that with 403 and no challenge. Found by comparing with selvakn/gsm-sip-bridge (a client that registers on Jio and
  Vodafone India; its notes name this exact behaviour for an ealg=null-only offer).
- Now sent: four tuples, `ipsec-3gpp;alg={hmac-md5-96,hmac-sha-1-96};ealg={aes-cbc,null};spi-c;spi-s;port-c;port-s`
  (port-c = the socket's source port, port-s = port-c + 2 = the Contact port), TCP, home-domain request line, PANI
  `3GPP-E-UTRAN-FDD;utran-cell-id-3gpp=<serving MCC MNC><TAC 4 hex><ECI 7 hex>` (08 05: ECI bytes 6-9 LE, TAC 11-12 LE).
- Answer: `401 Unauthorized`, `WWW-Authenticate: Digest realm=<home>, nonce=<b64 RAND|AUTN>, algorithm=AKAv1-MD5, qop="auth"`,
  `Security-Server: ipsec-3gpp; q=0.1; alg=hmac-md5-96; ealg=aes-cbc; spi-c=..; spi-s=..; port-c=..; port-s=6000`.
- Next: nonce -> RAND/AUTN -> `auth` (05 09) on the ISIM -> RES/CK/IK; four ESP transport SAs (PF_KEY: hmac-md5-96 with IK,
  aes-cbc with CK); second REGISTER from port-c to the P-CSCF's port-s with the digest response (password = RES, qop=auth)
  and `Security-Verify` = the Security-Server line as received; then 200, SUBSCRIBE reg, SIP MESSAGE for SMS, INVITE for calls.
- `--nullonly` keeps the old offer for comparison. One attempt per several minutes, as before.

## MILESTONE 2026-11-13 13:15 — IMS REGISTERED on Jio (200 OK) (session B)

- `redwood-ims` does the whole registration on the phone: REGISTER 1 (TCP, clear, four-tuple Security-Client) -> 401;
  nonce -> RAND|AUTN -> ISIM AUTHENTICATE over a logical channel through the daemon's `apdu` pipe (DB | RES 8 | CK | IK);
  four ESP transport SAs + four port-selective policies through PF_KEY (this kernel takes hmac-md5-96 + aes-cbc; SA
  src->dst: UE->P spi = P's spi-s (unique:1), P->UE spi-c, P->UE spi-s, UE->P spi = P's spi-c (unique:2)); REGISTER 2
  from port-c to the P-CSCF's port-s 6000 with the AKAv1-MD5 digest (password = RES, qop=auth) and Security-Verify = the
  whole Security-Server value -> **200 OK**, expires 7200, Path, two Service-Routes, P-Associated-URI (sip: and tel: MSISDN).
- XNU PF_KEY facts: SADB_EXT_SA must be the 24-byte sadb_sa_2; MD5HMAC is 1, SHA1HMAC 2, AESCBC 12, EALG_NULL 3;
  sha1 + null was refused (EINVAL) in the self-test, md5 + aes-cbc accepted. `redwood-ims --selftest` checks the kernel side
  and the ISIM without touching the network; `--flush` removes SAs and policies.
- Without `--once` the client stays, answers requests with 200 and prints them (IMSI/IMEI/nonce masked).
- Next: SIP MESSAGE (application/vnd.3gpp.sms: RP-DATA/RP-ACK) both ways, re-REGISTER before expiry, SUBSCRIBE reg;
  then calls (INVITE, preconditions, dedicated bearer, AMR-WB RTP) -- a much larger piece.

## 2026-11-13 15:56 — SMS RECEIVE over IMS works (A)

- Jio delivers texts to the registered client as SIP MESSAGE, Content-Type application/vnd.3gpp.sms, From tel:<SMSC>,
  body = RP-DATA (TS 24.011): 01 ref | originator (SMSC) addr | empty destination | user-data len | TPDU SMS-DELIVER.
- redwood-ims: 200 to the MESSAGE, TPDU decoded with sms-pdu.h, appended to /var/root/cell-sms.txt (0600, same format as
  the modem path), then RP-ACK in our own MESSAGE to the From URI over the protected connection: body 02 ref 41 02 00 00,
  Route = the Service-Route(s) of the 200 to REGISTER, From = first P-Associated-URI, In-Reply-To = their Call-ID,
  Request-Disposition no-fork, Accept-Contact *;+g.3gpp.smsip. Jio answers 202 Accepted (4/4).
- First run delivered 4 queued texts (incl. a two-part one: concatenation not joined yet).
- The 15:24 "data stopped" was most likely the user's data pack expiring (JioPay text "Data pack Expired ... 15:24").
  A downlink watchdog was added anyway (fd66d71).
- Not done: sending (RP-DATA MO with SMS-SUBMIT to the SMSC), concatenated parts, re-REGISTER before expiry (7200 s),
  launchd job, Messages app.

## 2026-11-13 16:00 — SMS SEND over IMS works (A)

- `echo "sms +91NUMBER text" > /var/tmp/redwood-cell/ims-cmd` (root-only pipe made by redwood-ims).
- SMS-SUBMIT (GSM 7-bit if every character is plain ASCII-safe, else UCS-2; one part: 160 / 70 chars), TP-MR counter,
  wrapped in RP-DATA MS->net {00 ref 00 | SMSC addr (the RP-originator of an incoming RP-DATA) | len | TPDU}, sent as
  MESSAGE to the SMSC URI (the From of incoming texts, a tel: URI) with the Service-Route(s). Jio: 202, then a MESSAGE
  with RP-ACK (03 ref) = delivered to the SMSC; RP-ERROR (05) is reported with its cause.
- First test went to the user's other phone (+91…7988); the SIM's own number (P-Associated-URI) ends …3945.
- The SMSC is only known after the first incoming text (the client learns it); TODO: read it from the SIM (EF_SMSP) or
  the modem (IpcTxGetSmscAddress) at start.
- 16:05: redwood-cellulard starts /var/root/redwood-ims once the IMS utun is up (log /var/tmp/redwood-ims.log, 0600,
  appended) and again whenever it leaves (>= 30 s apart). The client leaves cleanly (SAs flushed) after 6600 s for a fresh
  registration, when the P-CSCF connection closes, or on SIGTERM. Kill switch: /var/root/cell-noimsclient.

## 2026-11-13 16:15 — iOS integration: what is missing and the plan (A)

User-visible gaps after SMS worked underneath: no LTE glyph, no Control Center cellular button, no hotspot, texts not
in Messages, no calls.
- LTE glyph: STTelephonySubscriptionInfo needs `preferredForDataConnections` and `providingDataConnection` besides
  `dataConnectionType`; cell_sb.h sets them from the state file's new `data_up` (reboot 16:12, to verify).
- Messages app (next): received texts are in /var/root/cell-sms.txt and redwood-ims; iOS's SMS path is
  IMDaemon/IMAgent + the SMS service plugin talking to CommCenter (CTMessageCenter / CTXPCServiceSMSInterface).
  Options: (a) the stand-in CommCenter answers the SMS interface and pushes incoming messages (needs its client
  callbacks worked out first -- it blanked the status bar once, so only with real answers); (b) write into the
  Messages database (sms.db) + notify IMAgent (fragile); (c) our own notification + a small app. (a) is the native
  route; start by logging what IMAgent asks when the stand-in runs with real subscription answers.
- Control Center cellular button: CCUIConnectivityCellularDataViewController, shown only when MobileGestalt says the
  device has cellular data and CoreTelephony answers; same stand-in-CommCenter dependency.
- Hotspot: NAT64-free design needed (Jio is IPv6-only): share over Wi-Fi with IPv6 (RA from the phone) + DNS64, or
  CLAT/NAT; iOS Personal Hotspot (MISManager) needs CommCenter's tethering answers too.
- Calls: INVITE with SDP (AMR-WB/EVS), preconditions (QoS via the dedicated bearer the network opens: new cid on
  IMS APN signalled by 0d 10/0d 09), RTP to/from the modem's audio path (Samsung CP audio over a separate channel).

## 2026-11-13 16:40 — texts in the Messages app (A)

- imagent gets the port's tweak loader: `libredwood-tweaks.dylib Preferences imagent` (live: sysctl kern.exec_inject;
  persistent: userland/build.sh INJECT -- TODO next image). Tweak: /var/jb/usr/lib/TweakInject/RedwoodSMS.dylib (+ .plist,
  filter imagent), source userland/src/sms_bridge.m, log /var/mobile/Library/Logs/redwood-sms.log (no texts in it).
- SEND (works, user-verified): -[SMSServiceSession sendCTMessageFromIMMessageItem:forChat:chat:style:] replaced: the
  item's body + chat identifier -> /var/tmp/redwood-cell/sms-out/<guid>.msg -> redwood-ims sends -> <guid>.ok/.fail ->
  didSendMessage:forChat:style: / didReceiveError:forMessageID:forceError:. Numbers without +91 are normalised.
- RECEIVE (works): redwood-ims writes /var/tmp/redwood-cell/sms-in/<t>-<ref>.txt (sender, time, text; mobile 0600). The
  bridge (only while /var/tmp/redwood-cell/sms-bridge-in exists) moves it to sms-in/taken/ first (no crash loop on one
  text), then builds the object the SMS plugin expects from BlastDoor -- real run-time subclasses of BlastDoorSMSMessage,
  BlastDoorSMSCTPart, BlastDoorSMSCTPartContentWrapper (type 1 = plain), BlastDoorSMSCTPartPlain (plainTextContentBody),
  allocated WITHOUT init (Swift wrappers: "Use of unimplemented initializer init()" traps) -- and calls
  -[SMSServiceSession _processReceivedSMSMessage:storageContext:withCompletionBlock:].
- Lessons: CTMessage is not what iOS 16 takes (it wants BlastDoorSMSMessage: -version); the dictionary path goes to
  BlastDoor out of process; imagent crash loops end in launchd "spawn failed" with exponential throttling (wait ~1 min);
  never open sms: URLs to wake Messages (each one makes a new draft). Crash text is in the console log (:8080/log).
- /var/root/cell-sms.txt keeps every received text; re-queue from it if the bridge missed some.

## 2026-11-13 17:00 — long texts, reactions, texts from boot (A)

- Send: more than 160 GSM / 70 UCS-2 characters -> parts with UDH 05 00 03 ref total seq (GSM 153 per part, padded to a
  septet boundary: 49 bits of header; UCS-2 67, surrogate pairs never split); each part its own RP-DATA; the Messages
  job answers Delivered only when every part's RP-ACK came (any RP-ERROR -> Not Delivered).
- Receive: parts (UDH IEI 00 or 08) held per sender+reference, delivered joined when complete or after 5 minutes
  ("…" for a missing part). /var/root/cell-sms.txt still records each part.
- Reactions: an item without plain text but with associatedMessageType -> +[IMTapback tapbackWithAssociatedMessageType:
  messageSummaryInfo:] -backwardCompatibilityStringWithMessageSummaryInfo: (the text iPhones send over SMS).
- Incoming delivery is on by default; off: /var/mobile/Library/Preferences/com.redwood.sms-bridge-off.
- From boot (no hands): image inject.conf has `libredwood-tweaks.dylib Preferences imagent`; RedwoodSMS in /var/jb;
  redwood-ims (/var/root) started by redwood-cellulard; SMSC in /var/root/cell-smsc.

## 2026-11-13 17:50 — Control Center cellular button: not solved yet (A)

- iOS hides it because MobileGestalt says HasBaseband = false, cellular-data = false (no Apple baseband).
- Control Center's ConnectivityModule (loaded into SpringBoard at the first opening):
  CCUIConnectivityModuleViewController (_setupPortraitButtons, _setupLandscapeButtons, _makeButtonWithClass:,
  cellularDataButton, portrait/landscape/buttonViewControllers) and CCUIConnectivityCellularDataViewController
  (buttonTapped:, _toggleState, _updateState, _isCellularDataButtonDemoMode, _isCellularDataRestricted).
- libredwood-sb (cell_sb.h) hooks them at module load (dyld add-image) and on viewWillAppear: the cellular button is
  made by the module's factory and inserted after airplane mode; the tap sends data/nodata; the state follows data_on.
  All 7 hooks install, but the button is not shown and "button added" is never logged -> the button object most likely
  already exists in the lists and the compressed 2x2 layout picks its four buttons elsewhere (AirDrop takes the slot).
  Next: sb-dump the arrays' contents at viewWillAppear, find the layout's choice (viewWillLayoutSubviews /
  _compressedButtonSize), or set the cellular button where AirDrop is.
- Tools for this without new images: /var/tmp/redwood-cell/sb-dump ("CLASSPART [METHODPART]", class + instance methods
  and matching images to the console) and sb-hook ("CLASS SELECTOR YES|NO|DATA" for BOOL getters).
- Lesson: the deploy tool can fail to reach fastboot ("did not reach Log access") and the phone then boots the OLD image;
  always compare the md5 of /redwood/lib/<lib> with the build after a deploy.
- ControlCenterCellularDataButtonDemoMode (com.apple.springboard / com.apple.controlcenter) did not show the button;
  set back to false.
- 17:58: the whole Control Center attempt was REMOVED at the user's request (main 5146306): no hooks in
  ConnectivityModule, no module-load callback, no sb-dump/sb-hook. The notes above stay for a later attempt;
  the code is in main's history (5588958 and before).
## 2026-11-13 18:35 — stability fixes; calls: groundwork (session B)

Stability (all on main, prebuilt redwood-cellulard + redwood-ims updated, live on the phone):
- **VoLTE off after a reboot (fixed, 9f4c792):** /var/tmp survives a restart; the stale `ims.pid` named another
  process (installd), cellulard took it for a running client and never started redwood-ims (34 min without VoLTE).
  The pid is now checked with proc_pidpath and a wrong/dead one removed.
- **"downlink silent" modem restarts (suspected fix, 6a8ea5c):** pp_poll returned on a failed read before adding the
  handed count to `fore`; the CP's free-buffer count could only shrink until the downlink stopped. `fore` is now
  derived from `pp_done` on every exit (pp_give_back). Not yet proven: watch `cellular: health HH:MM:SS rx tx ring rear
  done fore dbm bars ims` (one line a minute in /var/tmp/redwood-cellular.log). A stall with `fore` != `done-1` = the
  old bug elsewhere; with fore right and rear frozen = the CP side.
- Weak signal seen at -116 dBm (0 bars) once; usually -89 (4 bars) here.
- Pipe command `restart` (echo restart > /var/tmp/redwood-cell/cmd) = the watchdog's clean modem restart.
  `launchctl kickstart -k system/com.redwood.cellular` also works (stop + start, picks /var/root's binary).
- **Incoming calls answered busy (e005b29):** before, redwood-ims answered every request with a bare 200 -- an INVITE
  would have connected the caller to silence (and billed them). Now 100 Trying + 486 Busy Here; the INVITE is saved
  root-only as /var/root/ims-invite-<time>.txt for the call work.

Calls: how Samsung does VoLTE voice on Exynos (stock libsec-ims.so / libmediarelayengine.so / libSamsungAPVoiceEngine.so,
extracted to B's scratchpad from out/cellular/private-rom/stock-system.img):
- **CPVE = "CP Voice Engine": the modem runs RTP and the AMR-WB/EVS codec itself.** libsec-ims (the AP IMS stack)
  does SIP only and drives the CP through `CPVEInterface`: InitEngine, SetPdnInfo, CreateChannel(local/remote IP+port),
  SetCodecInfo(codec, payload types, mode-set, ptime ...), StartChannel, SetAudioPath, StopChannel, EnableSRTP,
  HandleDtmf; events back: DTMF, RTP/RTCP timeout, RTP stats. ("don't initialize CPVE for softphone": the AP engine
  libSamsungAPVoiceEngine is for VoWiFi/softphone only.)
- So for a call: (1) SIP INVITE/183/PRACK/UPDATE/200/ACK/BYE with an AMR-WB SDP in redwood-ims; (2) the CPVE messages to
  the CP -- transport not found yet (no /dev/umts path in libsec-ims; look at how CPVEInterface sends: an IPC
  group in libsec-ril / a secril-client request / a separate SBD channel); (3) the CP's voice to the speaker/earpiece and
  mic: an ABOX route CP <-> codec (redwood audio); (4) the iOS call UI.
- Next concrete step: the user calls the S20 -> /var/root/ims-invite-*.txt gives Jio's SDP (codecs, preconditions,
  100rel) -> decide the INVITE answer path. Then disassemble CPVEInterface::CreateChannel/StartChannel in libsec-ims.

## 2026-11-13 21:45 (A): the CP voice commands (from stock libsec-ims.so, symbols intact; extracted privately)
OemIPCHandler builds a full Samsung IPC frame ([u16 len][mseq][aseq][grp][idx][type][payload], len = whole frame) and
sends it through 0x81e750 -> RIL OEM hook -> libsec-ril -> CP. IMS group = 0x16. Table (grp idx type len):
    0x7d2364 OemIPCHandler::ImsSetChannel(int, int, int, int, int, int, i grp #0x16 idx #1 type #3 len #0x36
    0x7d24c0 OemIPCHandler::ImsGetCodecList(int, int*, _stSAE_CodecInfo*) grp #0x16 idx #2 type #2 len #9
    0x7d2d24 OemIPCHandler::ImsSetCodec(int, int, int, _stSAE_CodecInfo*) grp #0x16 idx #2 type #3 len #0x41
    0x7d3400 OemIPCHandler::ImsSetRtpMedia(int, int, int, int, int)       grp #0x16 idx #3 type #3 len #0xb
    0x7d3630 OemIPCHandler::ImsSetDtmf(int, int, int, int, int, unsigned  grp #0x16 idx #4 type #3 len #0xd
    0x7d37f8 OemIPCHandler::ImsHandleDtmf(int, int, int, int, int)        grp #0x16 idx #4 type #1 len #0xb
    0x7d3980 OemIPCHandler::ImsGetOption(int, int, _SAE_RTPStatistics*)   grp #0x16 idx #5 type #2 len #0x4e
    0x7d3c80 OemIPCHandler::ImsSetOption(int, int, int, IPCIMSOptionParam grp #0x16 idx #5 type #3 len #0x4e
    0x7d3eac OemIPCHandler::ImsSetPdnIp(int, int, unsigned char, void*, c grp #0x16 idx #6 type #3 len #0x2d
    0x7d4034 OemIPCHandler::ImsStopSessRefresh(int, int)                  grp #0x16 idx #7 type #1 len #0xc
    0x7d424c OemIPCHandler::ImsStartSessRefresh(int, int, void*, void*, u grp #0x16 idx #7 type #1 len None
    0x7d43d8 OemIPCHandler::ImsInitSessRefresh(int, int, int, int, int)   grp #0x16 idx #7 type #1 len #0x47a
    0x7d450c OemIPCHandler::ImsStartEvent(int, unsigned char)             grp #0x16 idx #1 type #5 len #8
    0x7d463c OemIPCHandler::ImsSimInfo(int, int)                          grp #0x16 idx #0x20 type #3 len #8
    0x7d4794 OemIPCHandler::ImsSessRefreshGetCseq(int, int)               grp #0x16 idx #7 type #2 len #0xb
    0x7d48f0 OemIPCHandler::ImsSessRefreshSetCseq(int, int, int)          grp #0x16 idx #7 type #3 len #0xf
    0x7d4a70 OemIPCHandler::ImsEngineSet(int, int, int)                   grp #0x16 idx #9 type #3 len #9
    0x7d4bf4 OemIPCHandler::ImsFrameTimeGet(int, int, int, _SAE_RTCPRepor grp #0x16 idx #0xa type #2 len #9
    0x7d4d78 OemIPCHandler::ImsFrameTimeSet(int, int, int, _SAE_TimeStamp grp #0x16 idx #0xa type #3 len #0x11
    0x7d559c OemIPCHandler::ImsNotiInfo(int, char*, int)                  grp #0x16 idx #0xc type #5 len #0x6c
    0x7d5684 OemIPCHandler::ImsSIPInfoForACB(int, int)                    grp #0x16 idx #0x11 type #5 len #8
    0x7d57b4 OemIPCHandler::ImsGetNetworkInfo(int, _SAE_NetworkInfoReport grp #0x16 idx #0x18 type #2 len #8
    0x7d5ba4 OemIPCHandler::GetVersion(int, char*, int)                   grp #0x16 idx #0x1b type #2 len #7
    0x7d5d1c OemIPCHandler::GetEVSRateAdapt(int)                          grp #0x16 idx #0x23 type #2 len #8
    0x7d5ec8 OemIPCHandler::ImsDeleteCpClientSocket(int, int)             grp #0x16 idx #0x1c type #3 len #9
    0x7d6028 OemIPCHandler::ImsGetRadioEnvironmentInfo(int, _SAE_RadioEnv grp #0x16 idx #0x21 type #2 len #8
    0x7d6418 OemIPCHandler::ImsSendIPC(int, char*, int)                   grp #0x16 idx #0xf type #2 len #0x6c
    0x7d6d38 DefaultOemIPCHandler::ImsSetCSTuneAway(int, unsigned char)   grp #0x16 idx #0x12 type #3 len #8
    0x7d6e90 DefaultOemIPCHandler::ImsSignalingInfo(int, int, int, int)   grp #0x16 idx #0x19 type #3 len #0xa
ImsSetChannel(phoneId, a2, a3, a4, a5, a6, a7, a8, ipver, void *ipA, void *ipB, a12) -> IPC 16/01 SET, frame 0x36:
    +7 u8 a2 (channel)   +8 u8 a4 (op: only 1 or 3 fill the rest; anything else = ff: delete)
    +9 u16 a5  +0xb u16 a6  +0xd u16 a7  +0xf u16 a8   (the RTP/RTCP ports, local and remote: order to confirm
    against CPVEInterface::CreateChannel)   +0x11 u8 ipver (2: 16-byte IPv6, else 4-byte IPv4)
    +0x12 ipB (16 of 36 bytes)   +0x22 u8 ipver   +0x23 ipA (16 of 19)   +0x33 u8 a12   +0x34 u16 a3
Next: the layouts of 16/02 SetCodec (_stSAE_CodecInfo, 0x41), 16/06 SetPdnIp (0x2d), 16/03 SetRtpMedia (0x0b),
16/09 EngineSet (0x09); CPVEInterface::CreateChannel/StartChannel for the call order; then send them from
redwood-cellulard (it already frames IPC: live-modem.c ipc()). Audio: CPVEInterface::SetAudioPath is a stub (8 bytes):
the voice route CP <-> codec is the audio HAL's (RIL sound path IPC + ABOX), still to find. Test call still needed
(/var/root/ims-invite-*.txt for Jio's SDP).

### VoLTE calls: the full path, cross-checked (A, 2026-11-13 22:00; no phone changes)
Sources: stock G980FXXSNHYB7 libsec-ims.so / libsec-ril.so / vendor mixer_paths.xml (private extracts under
work/hackios-cellular/out/cellular/private-rom/{voice,audio}); Samsung's Exynos audio proxy (open source, carried by
the AOSP Exynos 990 tree sneedxynos/android_device_samsung_exynos990-common hardware/audio/proxy, copy in
git/_ref-exynos990-audio); ExtremeXT exynos990 kernel sound/soc/samsung/abox/abox_vss.c, drivers/soc/samsung/cpif/
shm_ipc.c; libsamsung-ipc (Replicant) for IPC group 0x09. AOSP ROMs (Lineage exynos9820-common, the x1slte trees)
run Samsung's own vendor RIL + IMS + audio HAL blobs: they show the wiring, not a re-implementation.

1. SIP (redwood-ims): INVITE -> 100/180/183 (+SDP AMR-WB, preconditions, 100rel) -> PRACK/UPDATE -> 200 -> ACK; BYE.
   Needs Jio's real INVITE: user calls the S20 (486 answer saves /var/root/ims-invite-*.txt).
2. Media in the CP (CPVE), IMS IPC group 0x16 through redwood-cellulard's ipc(): 16/06 SetPdnIp (our IMS address),
   16/01 SetChannel (IPs + RTP/RTCP ports, layout above), 16/02 SetCodec (_stSAE_CodecInfo, 0x41), 16/03 SetRtpMedia
   (start/stop), 16/09 EngineSet; 16/04 DTMF. Order: CPVEInterface::CreateChannel / StartChannel (to read next).
3. The CP's audio route: IPC group 0x09 (SND): AUDIO_PATH_CTRL, CLOCK_CTRL, SPKR_VOLUME_CTRL, MIC_MUTE_CTRL
   (libsec-ril IpcTxSndSetAudioPath; numbering per libsamsung-ipc, to confirm in libsec-ril).
4. ABOX (redwood_audio): voice runs in Calliope's VSS between the CP and ABOX through the CP's shared memory region
   SHMEM_VSS (cpif shm_ipc.c; in the CP rmem 0xf0000000+8M / 0xd0000000+64M per the CP TOC; abox-vss magic-offset
   0x700000 zeroed at probe). At call start: abox_set_system_state(SYSTEM_CALL, true) to the firmware
   (abox_vss_notify_call), the mixer path "incall-handset"/"incall-speaker" (ABOX Path Change ID 12, media path,
   route-vssrxif-vssbridge-rxse-to-srccom4, route-vpcmoutdai4-to-srccom4_b; mic: route-seriallif-cpcall-mic into the
   VSS adapter), and the proxy's virtual PCMs 104 (VRX playback) / 113 (VTX capture) opened.
5. iOS: incoming call UI (CallKit-like path or SpringBoard banner + answer/end), then outgoing dialling.
Milestone order: (a) capture INVITE; (b) answer with SDP + CPVE channel -> CP RTP running (check RTP stats event
16/05); (c) ABOX VSS route -> hear the caller; (d) mic; (e) UI.

### VoLTE calls, 2026-11-13 22:40 (A): the CP voice engine answers
- Live, no reboot: redwood-cellulard's command pipe sends any IPC frame -- `echo "fmt GRP IDX TYPE HEXBYTES" >
  /var/tmp/redwood-cell/cmd`; replies are logged in /var/tmp/redwood-cellular.log when /var/root/cell-verbose
  existed at the daemon's start (touch it, then `echo restart > /var/tmp/redwood-cell/cmd`: ~1 min without data).
- `fmt 16 1b 02` (CPVE GetVersion) -> `IPC <- group 16 index 1b type 02: 00 00 28 00`. The IMS group is live.
- The chain inside stock libsec-ims (symbols intact): CallManager -> CPVEInterface (CreateChannel, StartChannel,
  SetCodecInfo, SetPdnInfo) -> CpAudioEngineClient::SAE_* (SetPdnIp, CreateChannel, SetSendPort/SetRecPort,
  SetRTCPSendPort/SetRTCPRecPort, StartFDSR/StopFDSR, SetCodec ...; 196 functions) -> VoIpEngineProxy (per-channel
  state at this+0x505+8*ch; SetChannel/SetCodec/StartRtpMedia/SetPdnIp/SetEngine) -> OemIPCHandler::Ims* (IPC 16/xx).
- Decoded payloads (frame = [u16 len][mseq][aseq][16][idx][type] + payload; offsets below are payload bytes):
    16/09 SET EngineSet(phone, a2, a3):           [a2 u8][a3 u8]
    16/03 SET SetRtpMedia(phone, a2, a3, a4, a5): [a2 u8][a3 u8][a4 u8][a5 u8]; VoIpEngineProxy::StartRtpMedia(ph,
          ch, x) calls it as (ph, x, ch, channel->field_0xbc, ?): [x][ch][cp channel id][?]
    16/01 SET SetChannel: see the table above (47 bytes: ports, ipver, two IPs, op 1/3; op else = delete)
- Next: SAE_SetPdnIp -> 16/06 layout (0x7ca9f4 -> VoIpEngineProxy::SetPdnIp 0x7cea4c -> 0x7d3d34), 16/02 SetCodec
  (_stSAE_CodecInfo; 0x7d2910, 0x4c4 bytes), then try on the phone in order: 16/06 (our IMS IPv6), 16/01 op 1,
  16/02 AMR-WB, 16/03 start -- watch for 16/xx notifications (RTP stats 16/05). Needs Jio's SDP from a test call.
- 16/06 SET SetPdnIp, payload 38 bytes: [ipver u8: 1 v4 / 2 v6][IP 16 bytes][u32 len][interface name, <=15 chars, in
  17][flag u8]. From CpAudioEngineClient::SAE_SetPdnIp(phone, ipText, flag, ifname, ifnameLen) (inet_pton v4 then v6;
  "SetPdnIp failed. Interface name is null"). The name is Android's IMS PDN interface (Exynos: rmnetN; which N for the
  IMS PDN = the CP context the IMS bearer uses -- redwood-cellulard knows it) -- to confirm against stock behaviour.
- 16/02 SET SetCodec (frame 0x41, payload 58): built from _stSAE_CodecInfo (x22) at OemIPCHandler::ImsSetCodec
  0x7d2910; payload byte = frame offset - 7. Frame: +7 u8 channel(w24), +0xc u8, +0xd u8, +0xe u16, +0x14 u8 (info+0x18),
  +0x15 u32 (info+0x24), +0x1d u8, +0x1e u16 (info+0x38), +0x20 u8 (+0x3a), +0x21 u16, +0x23 u8, +0x24 u8 (+0x45),
  +0x25 u8 (+0x4a), +0x26 u8 (+0x47), +0x27 u16 (+0x48), +0x29 u8 (+0x4c), +0x2a/+0x2e/+0x32/+0x36 u32 (table-mapped
  enums of info+0x50/54/58/5c/60: codec, mode set, bandwidths ...), +0x3a u8 (+0x64), +0x3b u8 (+0x68), +0x40 u8 (+0x6c).
  Field meanings: from CPVEInterface::SetCodecInfo's arguments (codec name, payload types, mode-set, ptime, octet-align,
  EVS bandwidth range ...) -- the next RE step; then fill it from Jio's SDP.

### VoLTE calls: Jio's INVITE (captured 2026-11-13 22:37; the raw file stays root-only on the phone and in the
private extract folder -- it holds names, numbers and the IMEI: never paste it)
- SIP over the registration's TCP connection from the P-CSCF (Alcatel-Lucent PCSF); Record-Route + Contact on the
  P-CSCF; Supported: replaces,path,100rel (100rel NOT required); P-Early-Media supported; Expires 180.
- SDP: IPv6 media gateway, `m=audio <port> RTP/AVP 100 8 0 101`; 100 = AMR/8000 (narrowband) with
  mode-change-capability=2, bandwidth-efficient (no octet-align); 8 PCMA; 0 PCMU; 101 telephone-event 0-15;
  ptime 20, maxptime 80. **No preconditions** (no a=curr/a=des): the answer flow is simply
  100 Trying -> 180 Ringing -> (user answers) 200 OK + SDP answer -> ACK; BYE either way.
- Our SDP answer: c=IN IP6 <our IMS IPv6>, m=audio <local RTP port> RTP/AVP 100 101, a=rtpmap:100 AMR/8000,
  a=fmtp:100 mode-change-capability=2, a=rtpmap:101 telephone-event/8000, a=fmtp:101 0-15, a=ptime:20,
  a=maxptime:80, a=sendrecv.
- The CP media for it (CPVE): 16/06 SetPdnIp (our IMS IPv6), 16/01 SetChannel (local port; remote = the SDP's
  c=/m= address and port), 16/02 SetCodec (AMR-NB, PT 100, DTMF PT 101, ptime 20), 16/03 start -- after the 200 OK.

### VoLTE calls, milestone 2 (2026-11-13 22:50, user-verified): SIP answer works end to end
redwood-ims (3f7ea76, 6c061c5) with /var/root/cell-answer: the caller's phone rings, the call connects (silent: no
media yet), the network's session-timer UPDATE is answered, and the call ends by our BYE (sent on the registration
connection from port-c: on the P-CSCF's own connection it was dropped) or by the caller's. Without the switch file:
486 Busy as before. Next: the CP voice channel (CPVE 16/06, 16/01, 16/02, 16/03 after the ACK), then the ABOX route.

### 16/01 SetChannel, fully decoded (2026-11-13 23:00, A)
CPVEInterface::CreateChannel(phone, localIp, localPort, remoteIp, remotePort, ...) -> SAE_SetSendIP(ch, remote, local),
SAE_SetSendPort(remote RTP), SAE_SetRecPort (local RTP), SAE_SetRTCPSendPort, SAE_SetRTCPRecPort, SAE_SetStreamType ->
the channel record (+0xc remote IPv6 / +8 v4, +0x20 local IPv6 / +0x1c v4, +0x30 ipver, +0x38 remote RTP, +0x3c local
RTP, +0x40 remote RTCP, +0x44 local RTCP, +0x48 stream type) -> VoIpEngineProxy::SetChannel(phone, ch, op, x) ->
ImsSetChannel. Payload (47 bytes, little-endian u16):
    p0  u8  channel            p1  u8  op (1 = set up; 2/3 the other cases; else ff = delete)
    p2  u16 local RTCP port    p4  u16 remote RTCP port    p6  u16 local RTP port    p8  u16 remote RTP port
    p10 u8  ipver (2 = IPv6)   p11 16 bytes remote IP      p27 u8  ipver           p28 16 bytes local IP
    p44 u8  stream type        p45 u16 x (VoIpEngineProxy::SetChannel's 4th argument)
For Jio's call: local = our IMS IPv6 and CALL_RTP_PORT/+1; remote = the SDP's c=/m= (RTCP = RTP + 1).

### VoLTE calls, first media test (2026-11-13 23:00, A)
- During an answered call: 16/06, 16/01 (open), 16/02 (AMR-NB 12.2, third field = 1: 0 is refused 0x8001) all 0x8000.
- The network's dedicated bearer arrives as an unsolicited **16/0b type 03** (74 bytes at setup: `01 01 01`, four
  u32 0x78, then TFT filters with exactly our RTP/RTCP ports and the remote's; 22 bytes `01 01 03 ...` at release).
  In libsec-ims this is the NetworkQoS path (CPVEInterface::HandleNetworkQoSEvent -> CallManager::OnNetworkQosEvent).
- **16/03 SetRtpMedia [1][ch][x][0] answers 0x0064 (100)** for x = 0, 1, 5, 7: start not understood yet. The caller
  heard dead silence (no comfort noise): no evidence of RTP from the CP yet.
- **Do not send 16/05 GET with an all-zero 71-byte struct**: no reply, the IMS handler stopped answering (stop/close
  unacknowledged) and the watchdog restarted the modem (~1 min without data).
- Next: what 16/03's code 100 means (OemIPCHandler / VoIpEngineProxy::IpcCallback 0x7cf340 decode the 0x80 codes);
  whether the CP needs the audio path first (IPC group 0x09 SND AUDIO_PATH / CLOCK) or 16/09 EngineSet before it
  starts encoding; the ABOX VSS route.

### VoLTE calls, 2026-11-13 23:15 (A): the CP side is complete
- Call flow now (1453c42): 09/03 SET path=1, 09/09 EXEC clock=1, 16/06, 16/01 open, 16/02 AMR-NB, 16/03 start -- every
  one 0x8000 except 16/03 -> 100 while a channel is open over the bearer; with no channel open 16/03 is accepted
  (32/32, 23:09). Reading: 100 = already running (the CP starts the stream at 16/01 once the bearer is up).
- The caller still hears dead silence: the CP encodes what ABOX's VSS hands it and plays into VSS; without the ABOX
  call state + routes it has nothing. Next is the ABOX side (redwood_audio): VSS shared-memory magic, Calliope
  SYSTEM_CALL state (abox_vss_notify_call), mixer route "incall-handset" (vendor mixer_paths.xml), the virtual PCMs
  104/113 the audio proxy opens -- kernel work, needs reboots.
- P-CSCF reconnects: a resent INVITE on the new connection carries the call on (1453c42).
- **Found (23:20):** redwood_exynos_sound.c maps Calliope's VSS IOVA window (CAL_IOVA_VSS 0xA0000000: VSS firmware,
  parameter +16 MB, PCI +32 MB, doorbell +48 MB) to CAL_VSS_SPARE_PHYS 0xF5200000, one cleared spare MB -- a
  placeholder from the sound bring-up. Samsung maps it to the CP's shared-memory VSS region (cpif shm_ipc.c
  SHMEM_VSS, offsets from the CP's TOC; cp_rmem 0xf0000000+8M / cp_rmem_2 0xd0000000+64M), so ABOX and the CP never
  meet here. Next (kernel, reboot, behind a boot-arg): find SHMEM_VSS's physical range (the CP TOC that
  redwood-cellulard loads / cell-toc.bin), map CAL_IOVA_VSS there, zero the abox-vss magic word (+0x700000), send
  Calliope SYSTEM_CALL on/off at call start/end (abox_vss_notify_call -> abox_set_system_state), set the
  "incall-handset" routes. Then the caller should hear the S20's mic and vice versa.


## 2026-11-14 — calls work: the AP is the voice engine (A)
The CP's CPVE accepted every command (16/06, 16/01, 16/02, 16/03) but never took the RTP stream: the caller's packets
(dport 50010/50011) arrive at the AP on SBD channels 10/11 as bare IPv6. So the voice is done on the AP, as a VoIP app
does it: redwood-ims (`tools/cellular/ims-media.h`) decodes/encodes AMR-NB with opencore-amr and plays/records through
/dev/redwood-audio and /dev/redwood-mic; redwood-cellulard hands those packets to the IMS interface; redwood-ims adds a
host route to the media peer (only the P-CSCFs were routed: "No route to host"). Verified both ways by the user.
The ABOX/Calliope route (cpcall, call-audio.sh, abox_tplg ids) stays documented here and in SHIPPING.md but is unused.
Testing rule learned: never restart redwood-ims/cellulard while a call is up (the network keeps the call: "busy").

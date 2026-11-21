#!/bin/bash
# call-audio.sh on|off|status — VoLTE call audio through ABOX (A, 2026-11-13). Run on the Mac; talks to the phone over ssh s20.
#
# Needs a kernel booted with cpcall=1 or snd=3 (redwood_exynos_sound.c maps abox-pci-rmem 0xF4200000 at IOVA 0xA2000000 and the
# modem's PCIe doorbell at 0xA3000000, which is where the S5123's voice engine meets Calliope on the S20).
#
# The routes are stock's mixer_paths.xml "incall_nb-handset" + "incall_nb-handset-mic", turned into what the stock
# kernel's abox_tplg.c sends Calliope (gid/id/value from abox_tplg.conf, enum value = index in the "subject" list):
#   IPC_SYSTEM (1) ABOX_UPDATE_COMPONENT_CONTROL (0xC1 = 193): param1 gid, param2 id, param3 0, bundle[0] value
# plus the DAPM mixer widgets on the path powered (abox_tplg_mixer_event: value 1 on PRE_PMU, 0 on POST_PMD),
# ABOX_START_VSS (0xA0) as abox_modem_notifier sends when the modem is online, ABOX_SET_MODE (0x50) IN_CALL (2) and
# ABOX_SET_TYPE (0x60) VOICE (0). The UAIF2 / SPUS / amp side (RDMA3 -> SIFS0 -> UAIF2) is our own playback path.
set -u
SSH=${SSH:-ssh s20}
send() { $SSH "/var/root/rwsys kern.redwood.poke 'snd.send $*'" 2>&1 | head -1; }
ctl() { send 1 193 "$1" "$2" 0 "$3"; }          # gid id value
G=0xab0c; GO=0xc291                              # default gid, OEM gid

# mux selections: name id value
MUX=(
  "RDMA3_A 110 42"          # BD_MIXER
  "MCD_RXSE_A 304 46"       # VSS_RXBRIDGE  (modem downlink)
  "BD_SRCCOM4_A 215 70"     # MCD_RXSE
  "TDM_DEMUX3_A 268 17"     # WDMA4 (mics)
  "VSS_TXADAPTER_A 235 58"  # TDM_DEMUX3
  "MCD_TXSE1_A 292 47"      # VSS_TXADAPTER
  "MCD_TXSE2_A 295 66"      # MCD_TXSE1
  "VSSIF_TX_A 280 67"       # MCD_TXSE2 (modem uplink)
)
# mixer widgets, source to sink
WIDGETS=(
  "VSS_RXBRIDGE 231" "MCD_RXSE 303" "BD_SRCCOM4 214" "BD_MIXER 223" "RDMA3 109"
  "WDMA4 148" "TDM_DEMUX3 267" "VSS_TXADAPTER 234" "MCD_TXSE1 291" "MCD_TXSE2 294" "VSSIF_TX 279"
)

case "${1:-}" in
on)
  # the mics: /dev/redwood-mic starts DMIC -> VTS SLIF -> UAIF6 -> NSRC4 -> WDMA4 while it is open (docs/devices/x1slte/audio.md)
  $SSH 'nohup sh -c "cat /dev/redwood-mic > /dev/null" >/dev/null 2>&1 & echo $! > /var/tmp/call-mic.pid'
  echo "s2mpu: $($SSH "/var/root/rwsys kern.redwood.poke 'cp.s2mpu'" 2>&1 | head -1)"
  echo "START_VSS: $(send 1 160)"
  echo "SET_MODE in-call: $(send 1 80 2)"
  echo "SET_TYPE voice: $(send 1 96 0)"
  echo "path change 12: $(ctl $GO 1792 12)"
  echo "path change 8: $(ctl $GO 1792 8)"
  echo "nxp mode: $(ctl $GO 1030 1)"
  for m in "${MUX[@]}"; do set -- $m; echo "$1=$3: $(ctl $G $2 $3)"; done
  for w in "${WIDGETS[@]}"; do set -- $w; echo "$1 on: $(ctl $G $2 1)"; done
  ;;
off)
  for ((i=${#WIDGETS[@]}-1; i>=0; i--)); do set -- ${WIDGETS[i]}; echo "$1 off: $(ctl $G $2 0)"; done
  for m in "${MUX[@]}"; do set -- $m; echo "$1=None: $(ctl $G $2 0)"; done
  echo "SET_MODE normal: $(send 1 80 0)"
  $SSH 'kill $(cat /var/tmp/call-mic.pid) 2>/dev/null; pkill -f "cat /dev/redwood-mic"; rm -f /var/tmp/call-mic.pid' 
  ;;
status)
  $SSH "/var/root/rwsys kern.redwood.poke snd.log" 2>&1 | tail -40
  ;;
*) echo "usage: $0 on|off|status"; exit 1 ;;
esac

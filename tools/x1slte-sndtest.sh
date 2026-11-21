#!/bin/zsh
# x1slte-sndtest.sh "KNOBS" ["KNOBS" ...]: for each set of sound boot-args: payload + pack + hands-free flash + boot,
# then the audio driver's own start-up check from the kernel log (does RDMA3 read frames?).
cd "$(dirname "$0")/.."
IP=$(phone-ip 2>/dev/null || echo ${Y2S_WIFI_IP:-10.77.0.2}); R=logs/sndtest-$(date +%H%M%S).txt
./x1slte.sh kernel 2>&1 | tail -1
grep -E "redwood_exynos_sound.c:[0-9]+:[0-9]+: (error|warning)" out/kernel-build.log && exit 1
for k in "$@"; do
  echo; echo "=== try: $k"
  ./x1slte.sh payload "$(cat devices/exynos990/x1slte/x1slte.cmdline) $k" > /dev/null 2>&1 && ./x1slte.sh pack > /dev/null 2>&1 || { echo "pack failed"; exit 1; }
  tools/x1slte-auto.sh --recovery 2>&1 | grep -E "^\[auto\] (iOS is back|the phone|phone not)|uflash:" 
  sleep 20; ping -c1 -t1 $IP >/dev/null
  log=$(curl -s -m 90 http://$IP:8080/log | tr -d '\0')
  res=$(echo "$log" | grep -a -E "snd: (x1slte knobs|SIFS count|RDMA3 enabled|started|after .* the DMA read|.*DMA at|.*frames from iOS)|W6: " | head -8)
  echo "$res" | cut -c1-200; { echo "=== $k"; echo "$res"; } >> $R
done
echo; echo "results: $R"

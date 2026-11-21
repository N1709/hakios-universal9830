#!/bin/sh
# bg-status.sh: what of ours (and the known iOS offenders) runs in the background on the phone, what it costs,
# and how each debug switch stands. Read-only. See docs/devices/x1slte/background.md.
ssh -o ConnectTimeout=6 -o BatchMode=yes s20 '
echo "== load"; uptime
echo "== top CPU (more than 2 %)"; ps -A -o pid,%cpu,time,rss,comm -r | sed -n "2,30p" | while read p c t r n; do case "$c" in 0.*|1.*) ;; *) echo "  $p $c% $t ${r}K $n";; esac; done
echo "== our services (launchd: pid or -, last exit)"; launchctl list | grep -E "com\.redwood\." | sed "s/^/  /"
echo "== known iOS spinners"; for j in aggregated coreduetd biomed vmd; do ps -A -o %cpu,comm | grep -i "[/]$j\b" | sed "s/^/  /"; done
echo "== debug switches"
echo "  cellular verbose log: $(test -e /var/root/cell-verbose && echo ON || echo off)  (echo \"verbose on|off\" > /var/tmp/redwood-cell/cmd; file /var/root/cell-verbose = on at start)"
echo "  cellular log size: $(wc -c < /var/tmp/redwood-cellular.log) bytes (cut at 8 MB)"
echo "  ims log size: $(wc -c < /var/tmp/redwood-ims.log 2>/dev/null) bytes"
echo "  VoLTE client: $(test -e /var/root/cell-noimsclient && echo DISABLED || echo enabled)  registered: $(test -e /var/tmp/redwood-cell/ims-registered && echo yes || echo no)"
echo "  island conf: $(cat /var/jb/etc/redwood-island.conf 2>/dev/null || echo built-in)"
echo "  saved clock: $(cat /private/var/db/redwood-time 2>/dev/null || echo none)"
echo "  powerlog archives: $(ls /private/var/containers/Shared/SystemGroup/*/Library/BatteryLife/Archives 2>/dev/null | wc -l) files"
echo "  injected libraries: $(/jb/usr/sbin/sysctl -n kern.exec_inject | tr ";" "\n" | cut -d" " -f1 | tr "\n" " ")"
echo "== last cellular health"; grep -a "^cellular: health" /var/tmp/redwood-cellular.log | tail -1 | sed "s/^/  /"
' 2>&1

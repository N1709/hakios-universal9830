#!/bin/sh
# Copy the current live-modem build to the phone as the service override, restart the service, and wait
# (at most 5 min) for a log line matching $1 (default: the IMS interface). Phone lock holder only.
pat=${1:-"IMS utun[0-9]+ up"}
d=$(cd "$(dirname "$0")/../.." && pwd)
timeout 60 "$d/tools/cellular/stop-service.sh" | tail -1
scp -q "$d/out/cellular/live/live-modem" s20:/var/root/redwood-cellulard.new || exit 1
timeout 30 ssh s20 'cd /var/root && chmod 755 redwood-cellulard.new && mv redwood-cellulard.new redwood-cellulard && rm -f /var/tmp/redwood-cell/cmd && : > /var/tmp/redwood-cellular.log && launchctl kickstart system/com.redwood.cellular 2>/dev/null'
i=0; while [ $i -lt 30 ]; do sleep 10; i=$((i+1))
  timeout 10 ssh s20 "grep -qE '$pat' /var/tmp/redwood-cellular.log" && { echo "ready after $((i*10)) s"; exit 0; }
done
echo "not ready after 300 s"; exit 1

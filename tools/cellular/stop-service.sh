#!/bin/sh
# Stop the modem service on the phone cleanly (SIGTERM -> its own shutdown), wait up to 30 s, report CP power.
ssh -o BatchMode=yes -o ConnectTimeout=5 s20 '
for p in $(ps -A -o pid= -o comm= | grep -E "redwood-cellulard|cell-live-modem" | sed "s/^ *//; s/ .*//"); do kill -TERM "$p"; done
i=0; while [ $i -lt 15 ] && ps -A -o comm= | grep -qE "redwood-cellulard|cell-live-modem"; do sleep 2; i=$((i+1)); done
ps -A -o comm= | grep -E "redwood-cellulard|cell-live-modem" && echo "still running" || echo "service stopped"
/var/root/rwsys kern.redwood.poke "r 0x15c30284 1"'

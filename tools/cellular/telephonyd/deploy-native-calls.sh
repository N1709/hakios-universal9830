#!/bin/bash
# deploy-native-calls.sh: puts the native-calls pieces on a running phone, no reboot (B, 2026-11-15). Refuses during a call.
#   redwood-ims (/var/root, old kept as redwood-ims.before-native), libredwood-calls, redwood-telephonyd (+ callctl)
# Rollback: tools/cellular/telephonyd/deploy-native-calls.sh rollback
set -e
cd "$(dirname "$0")/../../.."
if [ "${1:-}" = rollback ]; then
	ssh s20 'launchctl bootout system/com.redwood.telephonyd 2>/dev/null; cp /var/root/redwood-ims.before-native /var/root/redwood-ims;
	  launchctl bootstrap system /var/jb/redwood/com.redwood.phone.plist; kill $(cat /var/tmp/redwood-cell/ims.pid) 2>/dev/null; sync; echo rolled back (libredwood-calls: rebuild from x1slte)'
	exit 0
fi
ssh s20 '! grep -q "^active" /var/tmp/redwood-cell/call 2>/dev/null && ! grep -q -E "^(ringing|answered|up)" /var/tmp/redwood-cell/call 2>/dev/null' || { echo "a call is up: not deployed"; exit 1; }
scp -q -O out/redwood-ims s20:/var/root/redwood-ims.new
ssh s20 'test -f /var/root/redwood-ims.before-native || cp /var/root/redwood-ims /var/root/redwood-ims.before-native; mv /var/root/redwood-ims.new /var/root/redwood-ims; chmod 755 /var/root/redwood-ims; sync'
bash userland/build-calls.sh install
bash tools/cellular/telephonyd/build.sh
bash tools/cellular/telephonyd/install.sh
scp -q -O tools/cellular/telephonyd/out/callctl s20:/var/root/callctl
# the new redwood-ims: the running one leaves (cellulard starts the new one; it re-registers in a few seconds)
ssh s20 'kill $(cat /var/tmp/redwood-cell/ims.pid) 2>/dev/null; sleep 15; tail -4 /var/tmp/redwood-ims.log; head -3 /var/tmp/redwood-cell/call-ui/calls; /var/root/callctl providers'

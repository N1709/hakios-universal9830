#!/bin/bash
# install.sh: redwood-telephonyd to /var/jb/redwood/bin, launchd job com.redwood.telephonyd (as mobile); RedwoodPhone (the
# third-party CallKit app it replaces) stopped. Phone lock holder only; never while a call is up.
set -e
cd "$(dirname "$0")"
scp -q -O out/redwood-telephonyd s20:/var/tmp/redwood-telephonyd.new
scp -q -O com.redwood.telephonyd.plist s20:/var/tmp/com.redwood.telephonyd.plist
ssh s20 'set -e; mkdir -p /var/jb/redwood/bin
  launchctl bootout system/com.redwood.telephonyd 2>/dev/null || true
  launchctl bootout system/com.redwood.phone 2>/dev/null || true
  mv /var/tmp/redwood-telephonyd.new /var/jb/redwood/bin/redwood-telephonyd; chown root:wheel /var/jb/redwood/bin/redwood-telephonyd; chmod 755 /var/jb/redwood/bin/redwood-telephonyd
  mv /var/tmp/com.redwood.telephonyd.plist /var/jb/redwood/com.redwood.telephonyd.plist; chown root:wheel /var/jb/redwood/com.redwood.telephonyd.plist
  launchctl bootstrap system /var/jb/redwood/com.redwood.telephonyd.plist; sleep 2; launchctl list | grep -E "telephonyd|com.redwood.phone" || true
  tail -3 /var/mobile/Library/Logs/redwood-telephonyd.log; sync'

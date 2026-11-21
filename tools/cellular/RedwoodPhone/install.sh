#!/bin/bash
# install.sh: RedwoodPhone.app to /var/jb/Applications, registered (uicache), run by launchd as mobile.
set -e
cd "$(dirname "$0")"
scp -q -O -r out/RedwoodPhone.app s20:/var/tmp/RedwoodPhone.app.new
scp -q -O com.redwood.phone.plist s20:/var/tmp/com.redwood.phone.plist
# callservicesd (Apple's, from the iOS rootfs; never in git) where the boot image's job runs it from
CSD=../../../sysimage/rootfs/System/Library/PrivateFrameworks/TelephonyUtilities.framework/callservicesd
[ -f "$CSD" ] && ssh s20 'test -x /var/jb/redwood/callservicesd' || { [ -f "$CSD" ] && ssh s20 'mkdir -p /var/jb/redwood' && scp -q -O "$CSD" s20:/var/jb/redwood/callservicesd && ssh s20 'chmod 755 /var/jb/redwood/callservicesd'; }
ssh s20 'launchctl bootout system/com.redwood.phone 2>/dev/null; rm -rf /var/jb/Applications/RedwoodPhone.app &&
  mv /var/tmp/RedwoodPhone.app.new /var/jb/Applications/RedwoodPhone.app && chown -R root:wheel /var/jb/Applications/RedwoodPhone.app &&
  /var/jb/usr/bin/uicache -p /var/jb/Applications/RedwoodPhone.app &&
  mkdir -p /var/jb/redwood && mv /var/tmp/com.redwood.phone.plist /var/jb/redwood/ && chown root:wheel /var/jb/redwood/com.redwood.phone.plist &&
  launchctl bootstrap system /var/jb/redwood/com.redwood.phone.plist && sleep 2 && launchctl list | grep com.redwood.phone;
  tail -3 /var/mobile/Library/Logs/redwood-phone.log'

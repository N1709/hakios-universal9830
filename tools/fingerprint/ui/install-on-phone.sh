#!/bin/sh
# Run on the S20 as root, by the current phone-lock holder, from the bundle.
# Does not restart SpringBoard or reboot. Relaunch Preferences manually.
set -eu
[ "$(id -u)" = 0 ] || { echo 'Root is required'; exit 1; }
cd "$(dirname "$0")"
S=/var/jb/usr/sbin/sysctl
"$S" kern.redwood.tee | /var/jb/usr/bin/grep -q 'fingerprint ui: reject-v1 (no authentication)' || {
  echo 'Compatible cancellable preview kernel is required; nothing installed'; exit 1;
}
for f in redwood-fingerprint-ui libredwood-fingerprint-ui.dylib com.redwood.fingerprint-ui.plist RedwoodFingerprintPreview.plist; do
  [ -f "$f" ] || { echo 'Incomplete preview bundle'; exit 1; }
done
mkdir -p /var/jb/redwood/bin /var/jb/redwood/lib /var/jb/Library/LaunchDaemons /var/jb/Library/MobileSubstrate/DynamicLibraries
# Explicitly refuse replacement; preserve a previous installation for review.
for p in /var/jb/redwood/bin/redwood-fingerprint-ui /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodFingerprintPreview.dylib; do
  [ ! -e "$p" ] || { echo 'Preview already installed; remove it before updating'; exit 1; }
done
cp redwood-fingerprint-ui /var/jb/redwood/bin/redwood-fingerprint-ui
chmod 0755 /var/jb/redwood/bin/redwood-fingerprint-ui
cp libredwood-fingerprint-ui.dylib /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodFingerprintPreview.dylib
cp RedwoodFingerprintPreview.plist /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodFingerprintPreview.plist
cp com.redwood.fingerprint-ui.plist /var/jb/Library/LaunchDaemons/com.redwood.fingerprint-ui.plist
chmod 0644 /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodFingerprintPreview.* /var/jb/Library/LaunchDaemons/com.redwood.fingerprint-ui.plist
/var/jb/bin/launchctl bootstrap system /var/jb/Library/LaunchDaemons/com.redwood.fingerprint-ui.plist
echo 'Installed. Open Settings after relaunching Preferences. SpringBoard must load the tweak at a coordinated restart.'

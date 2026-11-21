#!/bin/sh
# Independent native AOD service + Settings, default OFF. No hardware reboot.
set -eu
[ "$(id -u)" = 0 ] || exit 1
cd "$(dirname "$0")"
for f in RedwoodAlwaysOn.dylib RedwoodAlwaysOn.plist aod-preview; do [ -f "$f" ] || exit 1; done
[ -f /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodSoftwarePasscode.dylib ] || {
    echo 'Existing passcode/relock bridge is required'; exit 1;
}
[ ! -e /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAlwaysOn.dylib ] || {
    echo 'Already installed; use an atomic reviewed update'; exit 1;
}
mkdir -p /var/jb/redwood/bin
./aod-preview disable
cp RedwoodAlwaysOn.dylib /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAlwaysOn.dylib
cp RedwoodAlwaysOn.plist /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAlwaysOn.plist
cp aod-preview /var/jb/redwood/bin/aod-preview.new
chmod 0755 /var/jb/redwood/bin/aod-preview.new
mv /var/jb/redwood/bin/aod-preview.new /var/jb/redwood/bin/aod-preview
chmod 0644 /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAlwaysOn.dylib /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAlwaysOn.plist
echo 'Persistent native AOD installed, default OFF. No refresh or reboot performed.'

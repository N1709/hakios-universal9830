#!/bin/sh
# Root, phone-lock holder only. Copies an idle manual test; does not respring.
set -eu
[ "$(id -u)" = 0 ] || exit 1
cd "$(dirname "$0")"
for f in RedwoodAODPreview.dylib RedwoodAODPreview.plist aod-preview; do
    [ -f "$f" ] || { echo 'Incomplete test bundle'; exit 1; }
done
# Preserve the tested repeat-lock bridge. This preview does not replace it.
[ -f /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodSoftwarePasscode.dylib ] || {
    echo 'Expected standalone passcode/relock bridge absent; review before installing'; exit 1;
}
for p in /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAODPreview.dylib /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAODPreview.plist /var/jb/redwood/bin/aod-preview; do
    [ ! -e "$p" ] || { echo 'Preview already exists; do not overwrite an active test'; exit 1; }
done
mkdir -p /var/jb/redwood/bin
cp RedwoodAODPreview.dylib /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAODPreview.dylib
cp RedwoodAODPreview.plist /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAODPreview.plist
cp aod-preview /var/jb/redwood/bin/aod-preview
chmod 0644 /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAODPreview.dylib /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAODPreview.plist
chmod 0755 /var/jb/redwood/bin/aod-preview
echo 'Installed idle manual test; no restart requested.'

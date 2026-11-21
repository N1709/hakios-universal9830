#!/bin/bash
# Host helper. The current phone-lock holder may install/respring/begin/end.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
workspace=$(cd "$repo/../.." && pwd)
action=${1:-}
case "$action" in install|respring|begin|end|status) ;; *) echo 'usage: CLAUDE_SESSION=OWNER tools/aod/device.sh install|respring|begin|end|status' >&2; exit 2;; esac
[ -n "${CLAUDE_SESSION:-}" ] || { echo 'Set the current phone-lock holder explicitly' >&2; exit 1; }
owner=$(awk 'NR==1 {print $1}' "$workspace/build/phone.lock/owner" 2>/dev/null || true)
[ "$owner" = "$CLAUDE_SESSION" ] || { echo 'Another agent has priority; no phone operation performed' >&2; exit 1; }
case "$action" in
install)
    [ -f "$repo/out/aod/preview-bundle.tar.gz" ] || { echo 'Build/package first'; exit 1; }
    target=$(ssh -o BatchMode=yes -o ConnectTimeout=5 s20 'umask 077; mktemp -d /var/root/aod-preview.XXXXXX')
    [[ "$target" =~ ^/var/root/aod-preview\.[[:alnum:]]+$ ]] || { echo 'Unexpected staging path'; exit 1; }
    scp -q -O "$repo/out/aod/RedwoodAODPreview.dylib" "$repo/out/aod/aod-preview" \
        "$here/RedwoodAODPreview.plist" "$here/install-on-phone.sh" "s20:$target/"
    ssh -o BatchMode=yes -o ConnectTimeout=5 s20 "cd '$target' && sh install-on-phone.sh"
    ;;
respring)
    # User explicitly requested a SpringBoard refresh; no kernel/loader reboot.
    ssh -o BatchMode=yes -o ConnectTimeout=5 s20 'test -f /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodAODPreview.dylib && /var/jb/usr/bin/killall -TERM SpringBoard'
    ;;
begin|end|status)
    ssh -o BatchMode=yes -o ConnectTimeout=5 s20 "/var/jb/redwood/bin/aod-preview '$action'"
    ;;
esac

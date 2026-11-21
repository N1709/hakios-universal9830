#!/bin/zsh
# Keep the Apple-account Store broker + phone USB tunnel running on this Mac.
# Re-resolves the Hackintosh's link-local IPv6 (it changes per boot) by matching the
# pinned host key, rewrites the SSH config's account-hackintosh HostName, then starts
# the broker (Mac<->Hackintosh signing) and the phone tunnel (reverse forward over USB).
# KeepAlive in the launch agent restarts this wrapper if it exits.
set -u
ROOT=${ROOT:-$HOME/ios-port-research/work/hackios-apple-account}
P=$ROOT/out/apple-account/private
CONF=$P/account-hotspot-ssh.conf
KH=$P/hackintosh-known-hosts
log(){ echo "$(date '+%H:%M:%S') $*"; }

# pinned Hackintosh ed25519 fingerprint
PIN=$(ssh-keygen -lf "$KH" 2>/dev/null | awk '{print $2}' | head -1)

resolve_hackintosh(){
  [ -z "$PIN" ] && return 0
  ping6 -c2 -i1 ff02::1%en0 >/dev/null 2>&1
  for a in $(ndp -an 2>/dev/null | grep -oE 'fe80::[0-9a-f:]+%en0' | sort -u); do
    fp=$(ssh-keyscan -T 3 -t ed25519 "$a" 2>/dev/null | ssh-keygen -lf - 2>/dev/null | awk '{print $2}' | head -1)
    if [ "$fp" = "$PIN" ]; then echo "$a"; return 0; fi
  done
  return 0
}

cleanup(){ pkill -f 'phone-helper-tunnel.py' 2>/dev/null; }
trap cleanup EXIT

cd "$ROOT" || exit 1
NEW=$(resolve_hackintosh)
if [ -n "$NEW" ]; then
  log "hackintosh at $NEW; updating config"
  python3 - "$CONF" "$NEW" <<'PY'
import sys,re
p,new=sys.argv[1],sys.argv[2]
s=open(p).read()
repl='fe80::'+new.split('fe80::',1)[1] if 'fe80::' in new else new
s=re.sub(r'(Host account-hackintosh\n  HostName )fe80::[0-9A-Fa-f:]+%%en0',
         lambda m:m.group(1)+repl.replace('%','%%'), s)
open(p,'w').write(s)
print('config updated')
PY
fi

log "starting phone USB tunnel"
python3 -u tools/apple-account/phone-helper-tunnel.py --ssh-config "$CONF" >>"$P/keepalive-tunnel.log" 2>&1 &
sleep 2
log "starting broker (foreground)"
exec python3 -u tools/apple-account/native-provider-server.py \
  --host "${HACKINTOSH_SSH_HOST:?Set HACKINTOSH_SSH_HOST to your SSH target}" --ssh-config "$CONF" \
  --helper hackintosh-provider-v8 --media-helper native-media-provider-v4 \
  --key "$P/hackintosh_ed25519" --known-hosts "$KH" --token-file "$P/broker-token"

#!/var/jb/bin/sh
# Re-apply Store account injection rules at boot (survives reboot). Race-proof:
# waits for init's base rules to settle, then applies account rules ON TOP and
# restarts the account services; re-applies for ~2 min if the rules get reset.
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin:/sbin:/usr/sbin
RW=/redwood/bin/rwsys
LC=/var/jb/bin/launchctl
AL=/var/jb/usr/lib/libredwood-account-layer-d3c28bbd2c3f506f.dylib
MO=/var/jb/usr/lib/libredwood-metaloff.dylib
SB=/var/jb/usr/lib/libredwood-store-build.dylib
RC=/var/jb/usr/lib/libredwood-media-recorder.dylib
ACC="$MO Preferences;$AL akd accountsd appleaccountd amsaccountsd appstored itunesstored storeaccountd Preferences AppStore;$SB accountsd amsaccountsd appstored itunesstored Preferences AppStore;$RC accountsd Preferences AppStore"
echo "=== account-store boot $(date 2>/dev/null) ==="
for f in "$AL" "$SB"; do [ -f "$f" ] || { echo "missing $f, abort"; exit 1; }; done

# wait until init has set the base rules (marker: libredwood.dylib backboardd)
i=0
while [ $i -lt 120 ]; do
  cur=$($RW kern.exec_inject 2>/dev/null)
  case "$cur" in *"libredwood.dylib backboardd"*) break;; esac
  i=$((i+1)); sleep 1
done
# let base rules settle (init may still be writing)
sleep 5

apply(){
  cur=$($RW kern.exec_inject 2>/dev/null)
  base=$(printf '%s' "$cur" | tr ';' '\n' | grep -vE 'account-layer|store-build|store-amd|store-guid|store-public|media-recorder|media-transaction|libredwood-metaloff' | tr '\n' ';' | sed -E 's/^;+//;s/;+$//;s/;;+/;/g')
  new="$base;$ACC"
  $RW kern.exec_inject "$new" >/dev/null 2>&1
  [ "$($RW kern.exec_inject)" = "$new" ]
}
restart(){
  for s in akd accountsd appleaccountd amsaccountsd appstored itunesstored; do
    for d in user/501 system; do j=$d/com.apple.$s
      if $LC print "$j" >/dev/null 2>&1; then $LC kickstart -k "$j" >/dev/null 2>&1; break; fi
    done
  done
}

if apply; then echo "rules applied"; restart; echo "services restarted"; else echo "apply failed"; fi

# guard against a late reset for ~2 min
n=0
while [ $n -lt 8 ]; do
  sleep 15
  cur=$($RW kern.exec_inject 2>/dev/null)
  case "$cur" in
    *libredwood-store-build*) ;;              # still present, fine
    *) echo "rules were reset; re-applying"; apply && restart;;
  esac
  n=$((n+1))
done
echo "boot setup done"

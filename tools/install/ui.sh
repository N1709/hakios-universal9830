# ui.sh — the look of the install scripts (sourced): a title, numbered steps, ✓ / ! / ✗ lines.
#   STEPS=5; source ui.sh; title "Jesvi iOS: install"; step "Checking the phone"; ok "TWRP 3.7.1"; die "..."
if [ -t 1 ]; then
	B=$'\033[1m' D=$'\033[2m' G=$'\033[32m' Y=$'\033[33m' R=$'\033[31m' C=$'\033[36m' N=$'\033[0m'
else
	B= D= G= Y= R= C= N=
fi
STEP=0
STEPS=${STEPS:-0}
title() { local l; l=$(printf '%*s' "${#1}" '' | tr ' ' '-'); printf '\n%s%s%s\n%s%s%s\n' "$B" "$1" "$N" "$D" "$l" "$N"; }
step() { STEP=$((STEP + 1)); printf '\n%s[%d/%d]%s %s%s%s\n' "$C" "$STEP" "$STEPS" "$N" "$B" "$*" "$N"; }
ok() { printf '      %s✓%s %s\n' "$G" "$N" "$*"; }
info() { printf '      %s%s%s\n' "$D" "$*" "$N"; }
say() { printf '      %s\n' "$*"; }
warn() { printf '      %s! %s%s\n' "$Y" "$*" "$N"; }
die() { printf '\n      %s✗ %s%s\n\n' "$R" "$*" "$N" >&2; exit 1; }
done_() { printf '\n%s%s✓ %s%s\n\n' "$G" "$B" "$*" "$N"; }

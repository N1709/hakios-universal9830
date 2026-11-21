#!/bin/bash
# push-private.sh [REPO]: push every local branch and tag of this repository to the PRIVATE GitHub repo
# jesvi-eulah/REPO (default Jesvi-iOS), creating it if needed. Refuses to push if any branch contains
# secrets or files that must stay on this Mac (Samsung/Apple firmware, ssh/, jb/, local configs).
#
# Token: ~/.config/jesvi/github-token (one line, chmod 600). Never put it in the repo or a remote URL.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
REPO=${1:-Jesvi-iOS} OWNER=jesvi-eulah
TOKEN_FILE=$HOME/.config/jesvi/github-token
[ -f "$TOKEN_FILE" ] || { echo "put a GitHub token in $TOKEN_FILE (chmod 600)"; exit 2; }
[ "$(stat -f %Lp "$TOKEN_FILE" 2>/dev/null || stat -c %a "$TOKEN_FILE")" = 600 ] || { echo "chmod 600 $TOKEN_FILE"; exit 2; }
TOKEN=$(tr -d ' \n' < "$TOKEN_FILE")

echo "1/3 checking history of all branches for files that must not leave this Mac"
bad=$(git log --branches --tags --name-only --format= | sort -u |
	grep -E '^(ssh|jb|apps|fw-[a-z]+)/|\.local\.conf$|(^|/)fw-[a-z]+/network$|\.ipsw$|\.im4p$|\.img4$|_cache_arm64|\.tar\.md5$|\.pem$|id_(rsa|ed25519)' || true)
[ -z "$bad" ] || { echo "REFUSED, tracked in history:"; echo "$bad" | head -20; exit 1; }
echo "2/3 scanning file contents for tokens, keys and passwords"
hits=$(git rev-list --branches --tags --objects | git cat-file --batch-check='%(objecttype) %(objectname) %(objectsize)' |
	awk '$1=="blob" && $3<4000000{print $2}' | git cat-file --batch |
	LC_ALL=C grep -aoE 'ghp_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|-----BEGIN [A-Z ]*PRIVATE KEY-----|AKIA[0-9A-Z]{16}|wpa_passphrase|psk="[^"]{8,}' | sort -u || true)
[ -z "$hits" ] || { echo "REFUSED, secrets found:"; echo "$hits" | sed -E 's/(.{8}).*/\1.../'; exit 1; }

# personal details (phone numbers, this Mac's user folder and name, link-local addresses) in the current files of
# every branch; old history may still hold some (rewrite separately). Add the owner's own strings in
# ~/.config/jesvi/personal-patterns (one extended regex per line, kept off the repo).
pp='\+91[ -]?[6-9][0-9]{4} ?[0-9]{5}|/Users/[a-z]+/|[a-z]+s-MacBook[-A-Za-z]*\.local|fe80::[0-9a-f]{1,4}:[0-9a-f]{1,4}:[0-9a-f]{1,4}:'
[ -f "$HOME/.config/jesvi/personal-patterns" ] && pp="$pp|$(grep -v '^#' "$HOME/.config/jesvi/personal-patterns" | paste -sd'|' -)"
for br in $(git for-each-ref --format='%(refname:short)' refs/heads); do
	# allowed: Jio's SMS centre (a carrier number) and the placeholder test sender
	p=$(git grep -I -o -E "$pp" "$br" -- . ':!tools/push-private.sh' ':!*.patch' 2>/dev/null | grep -v -E ':\+917012075009$|:\+919999999999$' || true)
	[ -z "$p" ] && continue
	# main refuses; a feature branch only warns (its owner cleans it when merging main)
	if [ "$br" = main ]; then echo "REFUSED, personal details in main:"; echo "$p" | head -10; exit 1; fi
	echo "warning: personal details in $br ($(echo "$p" | wc -l | tr -d ' ') places): merge main into it"
done

api() { curl -sS -H "Authorization: token $TOKEN" -H "Accept: application/vnd.github+json" "$@"; }
code=$(api -o /dev/null -w '%{http_code}' "https://api.github.com/repos/$OWNER/$REPO")
if [ "$code" = 404 ] && [ "${CREATE:-0}" != 1 ]; then
	echo "REFUSED: $OWNER/$REPO does not exist (the argument is a REPOSITORY name, not a branch). CREATE=1 to make it."; exit 1
elif [ "$code" = 404 ]; then
	api -X POST https://api.github.com/user/repos \
		-d "{\"name\":\"$REPO\",\"private\":true,\"description\":\"iOS 16.5 on Exynos 990 Galaxy phones (S20 first): XNU, loader, userland, release tooling\"}" >/dev/null
	echo "created private $OWNER/$REPO"
elif [ "$(api "https://api.github.com/repos/$OWNER/$REPO" | python3 -c 'import sys,json;print(json.load(sys.stdin)["private"])')" != True ]; then
	echo "REFUSED: $OWNER/$REPO exists and is not private"; exit 1
fi
echo "3/3 pushing all branches and tags"
AUTH=$(printf 'x-access-token:%s' "$TOKEN" | base64)
git -c "http.https://github.com/.extraheader=AUTHORIZATION: basic $AUTH" \
	push --porcelain "https://github.com/$OWNER/$REPO.git" '+refs/heads/*:refs/heads/*' '+refs/tags/*:refs/tags/*' |
	grep -E '^[*+!-]|Done' | sed 's/^/  /'
api -X PATCH "https://api.github.com/repos/$OWNER/$REPO" -d '{"default_branch":"main"}' >/dev/null
echo "done: https://github.com/$OWNER/$REPO (private)"

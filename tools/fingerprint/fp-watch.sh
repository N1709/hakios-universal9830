#!/bin/zsh
# fp-watch.sh: live view of the fingerprint enroll loop -- animated fingerprint (fp-anim.py)
exec python3 -I "$(dirname "$0")/fp-anim.py"

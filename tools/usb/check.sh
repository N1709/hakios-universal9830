#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
testdir=$(mktemp -d "${TMPDIR:-/tmp}/jesvi-usbmux.XXXXXX")
trap 'rm -rf "$testdir"' EXIT
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$repo/tools/usb/wire-test.c" -o "$testdir/wire-test"
"$testdir/wire-test"

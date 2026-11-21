#!/bin/bash
# Jesvi iOS: the one entry point for checks, builds and packages. It never flashes a phone.
#   ./build.sh --help              every command (devices, validate, doctor, kernel, images, release, twrp, restore, ...)
#   ./build.sh verify|plan ...     offline package verification and install planning (tools/release/flash_plan.py)
set -euo pipefail
cd "$(dirname "$0")"
case "${1:-}" in
	verify|plan) exec python3 tools/release/flash_plan.py "$@" ;;
esac
exec python3 tools/release/manage.py "$@"

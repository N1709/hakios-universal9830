#!/bin/sh
# Host-only RAW10, concurrency, I2C failure and normalization tests. No phone access.
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
artifact_dir=${1:-$(mktemp -d "${TMPDIR:-/tmp}/s20-camera-quality.XXXXXX")}
mkdir -p "$artifact_dir"
artifact_dir=$(CDPATH= cd -- "$artifact_dir" && pwd)
cd "$repo_root"
camera_cc=${CC:-clang}
"$camera_cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iuserland/src tools/camera/test_isp_frame_state.c userland/src/camera_isp.c \
  -lm -lpthread -o "$artifact_dir/test-isp-state"
ASAN_OPTIONS=detect_leaks=0 "$artifact_dir/test-isp-state"
"$camera_cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=thread \
  -Iuserland/src tools/camera/test_isp_frame_state.c userland/src/camera_isp.c \
  -lm -lpthread -o "$artifact_dir/test-isp-state-tsan"
"$artifact_dir/test-isp-state-tsan"
python3 tools/camera/test_s20_controls.py
python3 -m unittest discover -s tools/camera -p 'test_samsung*.py'
printf 'Host test artifacts: %s\n' "$artifact_dir"

#!/bin/bash
# Isolated pinned source build; never use a library from another agent's tree.
set -euo pipefail
cd "$(dirname "$0")/.."
set +u # Legacy environment setup reads optional unset variables.
source ./xnu/xnu-env.sh
set -u
python3 - <<'PY'
import json, pathlib, subprocess
root = pathlib.Path.cwd()
lock = json.loads((root/'ci/sources.lock.json').read_text())['sources']['openh264']
source = root/'.cache/release-deps/openh264-source'
if not source.exists():
    source.mkdir(parents=True)
    subprocess.run(['git','init','-q',str(source)],check=True)
    subprocess.run(['git','-C',str(source),'fetch','--depth=1',lock['url'],lock['commit']],check=True)
    subprocess.run(['git','-C',str(source),'checkout','--detach',lock['commit']],check=True)
head = subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
if head != lock['commit'] or subprocess.check_output(['git','-C',str(source),'status','--porcelain','--untracked-files=no']):
    raise SystemExit('OpenH264 source differs from reviewed lock')
PY
# OpenH264 evaluates an xcrun SDK probe even with SDKROOT on the command line.
# Resolve that probe to our supplied SDK; CLT-only Macs have no registered iOS SDK.
mkdir -p .cache/release-deps/openh264-tools
cat > .cache/release-deps/openh264-tools/xcrun <<'SH'
#!/bin/sh
if [ "$1" = --sdk ] && [ "${3:-}" = --show-sdk-path ]; then
    printf '%s\n' "$OPENH264_SDK"
    exit 0
fi
exec /usr/bin/xcrun "$@"
SH
chmod +x .cache/release-deps/openh264-tools/xcrun
OPENH264_SDK="$THEOS/sdks/iPhoneOS16.5.sdk" PATH="$PWD/.cache/release-deps/openh264-tools:/usr/bin:/bin:$PATH" \
    make -C .cache/release-deps/openh264-source -j4 OS=ios ARCH=arm64 SDK_MIN=16.0 SDKROOT="$THEOS/sdks/iPhoneOS16.5.sdk" libraries

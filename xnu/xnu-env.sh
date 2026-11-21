# source this (from bash) before building XNU on Linux
# Everything is found relative to this file. THEOS (default ~/theos) provides the cctools and the
# iOS SDK; XNU_SRC is the kernel tree (xnu/apply.sh creates it).
DARWIN_REDWOOD=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export DARWIN_REDWOOD
export XNU_HOST_OS=$(uname)
# On macOS there is no theos: xnu-deps/setup.sh lays out xnu-deps/theos-mac like one (links to the
# cctools of the Command Line Tools and to the iPhoneOS 16.5 SDK of github.com/theos/sdks).
if [ "$XNU_HOST_OS" = Darwin ]; then
	export THEOS=${THEOS:-$DARWIN_REDWOOD/xnu-deps/theos-mac}
else
	export THEOS=${THEOS:-$HOME/theos}
fi
export XNU_SRC=${XNU_SRC:-$DARWIN_REDWOOD/xnu-src/xnu-8796.121.2}
export XNUDEPS=$DARWIN_REDWOOD/xnu-deps
if [ "$(uname -s)" = Darwin ]; then
	# Build natively on macOS: GNU userland from Homebrew + the two macOS shims.
	. "$DARWIN_REDWOOD/common/macos-env.sh"
	# legacy-hackios's macOS path: its xcrun (Darwin-aware: target SDK ours, host SDK the real one) and tools first
	export PATH=$XNUDEPS/host/bin:$PATH
else
	export PATH=$XNUDEPS/host/bin:$PATH
fi
export XNU_SDK_PATH=$XNUDEPS/sdk/iPhoneOS16.5.xnu.sdk
export XNU_SDK_VERSION=16.5
export XNU_PLATFORM_PATH=$XNUDEPS/sdk/Platforms/iPhoneOS.platform

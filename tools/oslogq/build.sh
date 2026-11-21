#!/bin/sh
# oslogq for the phone: Xcode's clang against the iOS 16.5 SDK, signed with its entitlement.
cd "$(dirname "$0")" && xcrun clang -target arm64-apple-ios15.0 -isysroot "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk" \
	-fobjc-arc -framework Foundation -o oslogq oslogq.m && ldid -Soslogq.ent oslogq

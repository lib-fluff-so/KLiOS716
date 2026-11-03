#!/usr/bin/env bash
#
# build.sh: build the SweetMali libdrm stand-in (libdrm.2.dylib) and the F2a
# test for iOS arm64.  Runs on the macOS VM (Xcode + the iPhoneOS SDK); the
# host has no iPhoneOS SDK.
#
#   cd ~/f2build && bash build.sh
#
# Products (signed ad-hoc, the way f1_test was):
#
#   libdrm.2.dylib   install name /var/root/libdrm.2.dylib
#   f2_test          links against it by that name
#
# The install names match the paths mkf1blob.py writes, so the pair can be
# delivered by the F1 blob mechanism with no kernel or rootfs change.
set -euo pipefail

cd "$(dirname "$0")"

SDK="${SDK:-$(xcrun --sdk iphoneos --show-sdk-path)}"
CC="${CC:-$(xcrun --sdk iphoneos -f clang)}"
ARCH=arm64
MIN=16.0
CFLAGS=(-arch "$ARCH" "-miphoneos-version-min=$MIN" -isysroot "$SDK"
        -O2 -Wall -Wextra -Wno-unused-parameter)
DYLIB=libdrm.2.dylib
TEST=f2_test

echo "sdk:  $SDK"
echo "cc:   $CC"

# --- the stand-in -------------------------------------------------------
"$CC" "${CFLAGS[@]}" \
	-dynamiclib \
	-install_name /var/root/"$DYLIB" \
	-fvisibility=hidden \
	-fno-common \
	-framework IOKit -framework CoreFoundation \
	sweet_drm.c -o "$DYLIB"

# --- the F2a test ------------------------------------------------------
"$CC" "${CFLAGS[@]}" \
	-I. -L. -ldrm.2 \
	-framework IOKit -framework CoreFoundation \
	sweet_mali_test.c -o "$TEST"

codesign --force --sign - "$DYLIB"
codesign --force --sign - "$TEST"

echo
echo "built:"
ls -l "$DYLIB" "$TEST"
echo
echo "dylib install name:"
otool -D "$DYLIB" | tail -1
echo "f2_test links:"
otool -L "$TEST" | grep -E 'libdrm|IOKit' || true

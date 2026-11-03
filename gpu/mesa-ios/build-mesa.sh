#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Cross-build Mesa 25.0.7 for iOS arm64 on the macOS VM: the OSMesa frontend
# with GL/GLES contexts and gallium panfrost over the SweetMali libdrm stand-in
# (gpu/mali-sweet). No LLVM, no X11/Wayland/GBM/EGL/GLX.
#
#   WORK=$HOME/f2build bash gpu/mesa-ios/build-mesa.sh
#
# WORK must contain (or the script creates):
#   mesa/            Mesa 25.0.7 checkout, patched with mesa-sweet.patch
#   libdrm/          libdrm-2.4.124 clone, headers only
#   sweet_drm.c ...  the stand-in sources (gpu/mali-sweet), copied here
#   venv/            meson/ninja (pip)
#
# Needs Xcode with the iPhoneOS SDK and bison >= 3.0 + pkgconf in $HOME/tools
# (installed there by hand: Homebrew refuses to run on x86_64 macOS).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:?set WORK (e.g. $HOME/f2build)}"
ARCH="${ARCH:-arm64}"
MIN="${MIN:-16.0}"
TAG="${MESA_TAG:-mesa-25.0.7}"
STANDIN="${STANDIN:-$WORK}"

mkdir -p "$WORK"; cd "$WORK"

export PATH="$WORK/venv/bin:$HOME/tools/bin:$PATH"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
BISON="${BISON:-$HOME/tools/bin/bison}"
PKGCONF="${PKGCONF:-$HOME/tools/bin/pkgconf}"
[ -x "$BISON" ]   || { echo "no bison at $BISON" >&2; exit 1; }
[ -x "$PKGCONF" ] || { echo "no pkgconf at $PKGCONF" >&2; exit 1; }

# --- sources -----------------------------------------------------------
if [ ! -d mesa ]; then
	git clone -q --depth 1 --branch "$TAG" \
		https://gitlab.freedesktop.org/mesa/mesa.git mesa
fi
if [ -f "$HERE/mesa-sweet.patch" ]; then
	if git -C mesa apply --check "$HERE/mesa-sweet.patch" 2>/dev/null; then
		git -C mesa apply "$HERE/mesa-sweet.patch"
		echo "mesa: patch applied"
	elif git -C mesa apply --check --reverse "$HERE/mesa-sweet.patch" 2>/dev/null; then
		echo "mesa: patch already applied"
	else
		echo "mesa: patch neither applies nor is applied; tree is dirty" >&2
		exit 1
	fi
fi

if [ ! -d libdrm ]; then
	git clone -q --depth 1 --branch libdrm-2.4.124 \
		https://gitlab.freedesktop.org/mesa/drm.git libdrm
fi

# --- the libdrm stand-in (arm64 dylib, install name /var/root/libdrm.2.dylib)
mkdir -p drm-standin pc
"$SDK/usr/bin/true" 2>/dev/null || true
xcrun --sdk iphoneos clang -arch "$ARCH" "-miphoneos-version-min=$MIN" \
	-isysroot "$SDK" -O2 -dynamiclib \
	-install_name /var/root/libdrm.2.dylib \
	-fvisibility=hidden -fno-common \
	-framework IOKit -framework CoreFoundation \
	"$STANDIN/sweet_drm.c" -o drm-standin/libdrm.2.dylib
ln -sf libdrm.2.dylib drm-standin/libdrm.dylib

cat > pc/libdrm.pc <<EOP
Name: libdrm
Description: SweetMali KL7 libdrm stand-in (gpu/mali-sweet)
Version: 2.4.124
Cflags: -I$WORK/libdrm/include/drm -I$WORK/libdrm
Libs: -L$WORK/drm-standin -ldrm
EOP

# --- the cross file ----------------------------------------------------
CROSS="$WORK/ios-$ARCH.cross"
cat > "$CROSS" <<EOT
[binaries]
c = ['xcrun','--sdk','iphoneos','clang','-arch','$ARCH']
cpp = ['xcrun','--sdk','iphoneos','clang++','-arch','$ARCH']
objc = ['xcrun','--sdk','iphoneos','clang','-arch','$ARCH']
objcpp = ['xcrun','--sdk','iphoneos','clang++','-arch','$ARCH']
ar = ['xcrun','--sdk','iphoneos','ar']
strip = ['xcrun','--sdk','iphoneos','strip']
ranlib = ['xcrun','--sdk','iphoneos','ranlib']
bison = '$BISON'
pkg-config = '$PKGCONF'

[built-in options]
pkg_config_path = ['$WORK/pc']
c_args = ['-miphoneos-version-min=$MIN','-isysroot','$SDK','-I$WORK/libdrm/include/drm','-I$WORK/libdrm']
cpp_args = ['-miphoneos-version-min=$MIN','-isysroot','$SDK','-I$WORK/libdrm/include/drm','-I$WORK/libdrm']
c_link_args = ['-miphoneos-version-min=$MIN','-isysroot','$SDK','-L$WORK/drm-standin','-ldrm']
cpp_link_args = ['-miphoneos-version-min=$MIN','-isysroot','$SDK','-L$WORK/drm-standin','-ldrm']

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = '$ARCH'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOT

# --- configure + build -------------------------------------------------
BLD="$WORK/build-$ARCH"
if [ ! -d "$BLD" ]; then
	(cd mesa && meson setup "$BLD" --cross-file "$CROSS" --buildtype=release \
		-Dgallium-drivers=panfrost,softpipe -Dvulkan-drivers= -Dllvm=disabled \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dplatforms= \
		-Dosmesa=true -Dopengl=true -Dgles1=enabled -Dgles2=enabled \
		-Dshared-glapi=enabled \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
		-Dxmlconfig=enabled -Dbuild-tests=false -Dglvnd=disabled \
		-Dlmsensors=disabled -Dgallium-opencl=disabled -Dgallium-rusticl=false \
		-Dshader-cache=disabled -Dprefix=/)
fi
ninja -C "$BLD"
ls -l "$BLD/src/gallium/targets/osmesa/libOSMesa.8.dylib"

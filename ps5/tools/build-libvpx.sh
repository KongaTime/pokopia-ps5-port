#!/usr/bin/env bash
# libvpx (BSD-3-Clause) built with the PS5 toolchain for the runtime's VP8/VP9 movie decoders (runtime/src/media/video_codec.cpp),
# the same version the PC build takes from vcpkg. Decoders only, static, with the x86 SIMD paths (nasm) and runtime CPU detection.
#   ps5/tools/build-libvpx.sh [prefix]     default $PKP_DEPS/libvpx (ps5/local.env); ps5/game/build.sh links it from there
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/../env.sh"
pkp_need PS5_SDK
deps=${PKP_DEPS:-${PKP_BUILD:?set PKP_BUILD in ps5/local.env}/deps}
prefix=${1:-$deps/libvpx}
ver=1.16.0
sum=7a479a3c66b9f5d5542a4c6a1b7d3768a983b1e5c14c60a9396edc9b649e015c
command -v nasm >/dev/null || { echo "error: nasm is not installed" >&2; exit 2; }

src=$deps/libvpx-src
mkdir -p "$src"
tar=$src/v$ver.tar.gz
[[ -f $tar ]] || curl -sSL --max-time 300 -o "$tar" "https://github.com/webmproject/libvpx/archive/refs/tags/v$ver.tar.gz"
echo "$sum  $tar" | sha256sum -c --quiet - || { echo "checksum mismatch: $tar" >&2; exit 1; }
rm -rf "$src/libvpx-$ver" && tar xzf "$tar" -C "$src"

build=$deps/libvpx-build
rm -rf "$build" && mkdir -p "$build"
cd "$build"
export CC="$PS5_SDK/bin/prospero-clang" CXX="$PS5_SDK/bin/prospero-clang++" AR="$PS5_SDK/bin/prospero-ar" LD="$PS5_SDK/bin/prospero-clang"
export NM="$PS5_SDK/bin/llvm-nm" STRIP=true CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC"
"$src/libvpx-$ver/configure" --target=x86_64-linux-gcc --as=nasm \
    --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-vp8-encoder --disable-vp9-encoder \
    --enable-vp8-decoder --enable-vp9-decoder --enable-static --disable-shared --enable-pic --enable-multithread \
    --disable-install-bins --disable-install-srcs --prefix="$prefix" > configure.log
make -j"${JOBS:-6}" > make.log 2>&1 || { tail -20 make.log >&2; exit 1; }
make install > install.log
echo "libvpx $ver: $prefix/lib/libvpx.a"

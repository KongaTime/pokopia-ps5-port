#!/usr/bin/env bash
# Builds ps5/compat into libps5compat.a with the PS5 toolchain (the same flags the title build will use).
# Usage: ps5/tools/build-compat.sh [out-dir]       default /home/kai/pkp-ps5-build/compat
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
SDK=${PS5_SDK:-/home/kai/khdays-build/ps5-sdk}
OUT=${1:-/home/kai/pkp-ps5-build/compat}
mkdir -p "$OUT"
FLAGS=(-std=c++20 -O2 -g1 -fno-strict-aliasing -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter
       -I"$HERE/ps5/compat" -include "$HERE/ps5/compat/prelude.h")
objs=()
for src in "$HERE"/ps5/compat/*.cpp; do
  obj=$OUT/$(basename "${src%.cpp}").o
  "$SDK/bin/prospero-clang++" "${FLAGS[@]}" -c "$src" -o "$obj"
  objs+=("$obj")
done
rm -f "$OUT/libps5compat.a"
"$SDK/bin/llvm-ar" rcs "$OUT/libps5compat.a" "${objs[@]}"
echo "built $OUT/libps5compat.a (${#objs[@]} objects)"

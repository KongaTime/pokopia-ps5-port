#!/usr/bin/env bash
# Builds ps5/compat into libps5compat.a with the PS5 toolchain (the same flags the title build will use).
# Usage: ps5/tools/build-compat.sh [out-dir]       default $PKP_BUILD/compat (ps5/local.env)
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
source "$HERE/ps5/env.sh"
pkp_need PS5_SDK
SDK=$PS5_SDK
OUT=${1:-${PKP_BUILD:?set PKP_BUILD in ps5/local.env or pass out-dir}/compat}
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

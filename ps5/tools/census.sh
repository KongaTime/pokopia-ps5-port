#!/usr/bin/env bash
# Compile census of the nxrt runtime against the PS5 toolchain.
#
# Compiles every .cpp under the given runtime/src subdirectories with prospero-clang++ (syntax only)
# and sorts each into accepted / missing header / other error, so the port's work queue is a list of
# files and first errors rather than a guess. Run in the WSL distro that holds the PS5 SDK.
#
# Usage: ps5/tools/census.sh <out-dir> [subdir ...]     (default: the non-graphics runtime)
set -uo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${1:?usage: census.sh <out-dir> [subdir ...]}
shift
SUBDIRS=("$@")
[ ${#SUBDIRS[@]} -eq 0 ] && SUBDIRS=(core kernel mem os ipc loader eh diag services timestep)

source "$(dirname -- "${BASH_SOURCE[0]}")/../env.sh"
pkp_need PS5_SDK
SDK=$PS5_SDK
CXX=$SDK/bin/prospero-clang++
mkdir -p "$OUT/logs"
REPORT=$OUT/census.txt
: > "$REPORT"

# What runtime/CMakeLists.txt gives nxrt, minus the Windows-only definitions (NOMINMAX etc.), plus the PS5 build's own:
# ps5/compat on the include path (the Win32 subset) and its prelude force-included.
INCLUDES=(-I"$HERE/runtime/include" -I"$HERE/runtime/src" -I"$HERE/recomp/include" -I"$HERE/runtime/third_party"
          -I"$HERE/ps5/compat")
# shellcheck disable=SC2206
[ -n "${CENSUS_EXTRA_INCLUDES:-}" ] && INCLUDES+=($CENSUS_EXTRA_INCLUDES)
FLAGS=(-std=c++20 -fsyntax-only -mcx16 -fno-strict-aliasing -ffp-contract=off -Wno-everything -ferror-limit=0
       -include "$HERE/ps5/compat/prelude.h")

total=0; ok=0; missing=0; other=0
declare -A headers=()
: > "$OUT/other-errors.txt"
for sub in "${SUBDIRS[@]}"; do
  while IFS= read -r file; do
    total=$((total + 1))
    rel=${file#"$HERE"/}
    log=$OUT/logs/$(echo "$rel" | tr '/' '_').log
    if "$CXX" "${FLAGS[@]}" "${INCLUDES[@]}" -c "$file" -o /dev/null > "$log" 2>&1; then
      ok=$((ok + 1)); echo "OK      $rel" >> "$REPORT"
    elif grep -q "file not found" "$log"; then
      missing=$((missing + 1))
      hdr=$(grep -o "error: '[^']*' file not found" "$log" | head -1 | sed "s/.*error: '//; s/' file not found//")
      hdr=${hdr:-unknown}
      headers["$hdr"]=$(( ${headers["$hdr"]:-0} + 1 ))
      echo "HEADER  $rel  <- $hdr" >> "$REPORT"
    else
      other=$((other + 1)); echo "ERROR   $rel  ($(grep -c 'error:' "$log") errors)" >> "$REPORT"
      { echo "== $rel"; grep -m3 "error:" "$log"; } >> "$OUT/other-errors.txt"
    fi
  done < <(find "$HERE/runtime/src/$sub" -name '*.cpp' | sort)
done

{
  echo
  printf 'translation units %d, accepted %d, missing header %d, other %d\n' "$total" "$ok" "$missing" "$other"
  for h in "${!headers[@]}"; do printf '%5d  %s\n' "${headers[$h]}" "$h"; done | sort -rn
} | tee -a "$REPORT"

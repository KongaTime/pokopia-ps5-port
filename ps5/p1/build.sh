#!/usr/bin/env bash
# P1 title: the runtime's lifted test programs on the console (ps5/p1/p1_main.cpp).
#
#   ps5/p1/build.sh                    prepare the cases, build, link and sign: dist/PPSA90002/
#   ps5/p1/build.sh --deploy <ip>      ... then upload it to /data/homebrew/PPSA90002 over the console's FTP (port 2121)
#
# Run in the archlinux WSL distro (the SDK, PS5_Vulkan's native tool and the cases' Python live there). The steps are
# the ones KongaTime's Monado-PS5 and The-Darkness-Recomp titles take: link with ps5-pie.ld, convert with ps5-native-tool,
# sign, and ship PS5_Vulkan's libc.prx beside the eboot (a title has no libc of its own).
#
# Launch PPSA90002 from the home screen. It writes p1-results/results.txt in its own folder (one line per step, then
# "RESULT n/m passed") and the runtime log p1-results/nxrt.log, and posts a notification when it is done.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ps5=$(dirname "$here")
sdk=${PS5_SDK:-/home/kai/khdays-build/ps5-sdk}
vulkan=${PS5_VULKAN_DIR:-/mnt/c/kb/PS5_Vulkan}
tool=${PS5_NATIVE_TOOL:-$vulkan/build/host/ps5-native-tool}
python=${PKP_PYTHON:-/home/kai/pkp-venv/bin/python}
work=${PKP_BUILD:-/home/kai/pkp-ps5-build}
cases=$work/p1-cases
build=$work/p1
tid=PPSA90002
dist=$ps5/../dist/$tid

fail() { printf 'error: %s\n' "$*" >&2; exit 2; }
[[ -f $sdk/toolchain/prospero.cmake ]] || fail "no PS5 SDK at $sdk (set PS5_SDK)"
[[ -x $tool ]] || fail "no ps5-native-tool at $tool (set PS5_NATIVE_TOOL)"
[[ -f $vulkan/runtime/libc.prx ]] || fail "no libc.prx in $vulkan/runtime"
[[ -x $python ]] || fail "no Python with the recompiler's packages at $python (see ps5/p1/README.md)"

echo "==> [1/5] test cases: guest ELFs, expected results, lifted C++"
if [[ ! -f $cases/cases.inc || ${PKP_REGEN:-0} == 1 ]]; then
    rm -rf "$cases"
    "$python" "$here/make_cases.py" "$cases"
fi

echo "==> [2/5] build (runtime, compat layer, 21 lifted modules, driver)"
cmake -S "$ps5" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DP1_CASES="$cases" >/dev/null
cmake --build "$build" -j "$(nproc)"

echo "==> [3/5] convert into a PS5 exec module and sign it"
rm -rf "$dist"
mkdir -p "$build/link" "$dist/sce_sys" "$dist/sce_module" "$dist/tests"
"$tool" link --in "$build/pokopia_p1" --out "$build/link/eboot.elf" --stub-dir "$sdk/target/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$build/link/eboot.elf" --out "$dist/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$dist/eboot.bin"

echo "==> [4/5] title folder"
cp "$here/param.json" "$dist/sce_sys/param.json"
for icon in "$sdk/samples/install_app/FAKE02932/icon0.png" "$HOME/ps5-sdk/samples/install_app/FAKE02932/icon0.png"; do
    [[ -f $icon ]] && { cp "$icon" "$dist/sce_sys/icon0.png"; break; }
done
cp "$vulkan/runtime/libc.prx" "$dist/sce_module/libc.prx"
cp "$cases"/elf/*.elf "$cases"/expected/*.expected "$dist/tests/"
( cd "$dist" && find . -type f | sort | xargs sha256sum ) > "$dist/../$tid.sha256"
printf '    %s: %s files, eboot.bin %s bytes\n' "$dist" "$(find "$dist" -type f | wc -l)" "$(stat -c%s "$dist/eboot.bin")"

if [[ ${1:-} == --deploy ]]; then
    host=${2:?usage: build.sh --deploy <console-ip>}
    ftp="ftp://$host:2121"
    echo "==> [5/5] upload to $ftp/data/homebrew/$tid"
    for d in /data/homebrew "/data/homebrew/$tid" "/data/homebrew/$tid/sce_sys" "/data/homebrew/$tid/sce_module" "/data/homebrew/$tid/tests"; do
        curl -s --max-time 15 -u anonymous: -Q "MKD $d" "$ftp/" >/dev/null || true
    done
    # Paths relative to the title folder (a glob here must expand there, not in the caller's directory).
    ( cd "$dist" && find sce_sys sce_module tests -type f | sort ) | while read -r g; do
        curl -sS --max-time 120 -u anonymous: -T "$dist/$g" "$ftp/data/homebrew/$tid/$(dirname "$g")/" >/dev/null ||
            fail "upload of $g failed"
    done
    # eboot.bin last: the title is not launchable until its main binary is there.
    curl -s --max-time 300 -u anonymous: -T "$dist/eboot.bin" "$ftp/data/homebrew/$tid/" >/dev/null
    echo "    uploaded: launch $tid from the home screen, then read /data/homebrew/$tid/p1-results/results.txt"
else
    echo "==> [5/5] not uploaded (pass --deploy <console-ip>)"
fi

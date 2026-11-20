#!/usr/bin/env bash
# The game title (PPSA90006): Pokémon Pokopia 2.0.1 on the PS5 runtime (ps5/game/pkp_ps5_main.cpp).
#
#   ps5/game/build.sh                          build, link and sign: dist/PPSA90006/ (no game data in it)
#   ps5/game/build.sh --deploy [<ip>]          ... and upload the title to /data/homebrew/PPSA90006
#   ps5/game/build.sh --game-data <ip> <dump>  upload exefs/ and romfs/ of the player's own 2.0.1 dump into that folder
#                                              (<dump> holds updates/655360/ or exefs/ + romfs/; once: 6.75 GB)
#   ps5/game/build.sh --shaders <ip> <spirv> [<order>]  upload a translated shader cache (<hash>.spv + results.tsv, made on a
#                                              PC from the player's romfs) to shader_cache/spirv, and shader_order.txt
#
# Paths come from ps5/local.env (see ps5/local.env.example); <ip> defaults to its PS5_HOST. Needs ps5/game/lift.sh's
# output (PKP_LIFT, default $PKP_BUILD/game-lift) and ps5/tools/fetch-deps.sh's (PKP_DEPS, default $PKP_BUILD/deps). The title carries no game files: the game data goes to the console's own title folder only, from the
# player's dump, and dist/ never holds it.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ps5=$(dirname "$here")
source "$ps5/env.sh"
pkp_need PS5_SDK PS5_VULKAN_DIR PKP_BUILD
sdk=$PS5_SDK
vulkan=$PS5_VULKAN_DIR
tool=${PS5_NATIVE_TOOL:-$vulkan/build/host/ps5-native-tool}
work=$PKP_BUILD
lift=${PKP_LIFT:-$work/game-lift}
deps=${PKP_DEPS:-$work/deps}
agc=${PKP_AGC_STUBS:-}   # RADV imports AGC: link stubs (ps5/radv.cmake)
radv=${PKP_RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
build=$work/game
tid=PPSA90006
dist=$ps5/../dist/$tid

fail() { printf 'error: %s\n' "$*" >&2; exit 2; }

# -g: dump folders carry brackets ("Pokopia_v2.0.1a[Base_Game_and_Update]"), which curl would read as a URL glob. The FTP
# client's .listing files are not the game's. A failed upload ends the script (the loop reads from a file, not a pipe, so
# fail() is in this shell).
upload_tree() {   # host local-dir remote-dir [skip-list: names already in remote-dir, not uploaded again]
    local ftp="ftp://$1:2121" src=$2 dst=$3 skip=${4:-} list
    list=$(mktemp)
    # Paths relative to src, without "./" (the console's FTP server refuses a "." component).
    ( cd "$src" && find . -mindepth 1 -type d | sed 's|^\./||' | sort ) > "$list"
    curl -gs --max-time 15 -u anonymous: -Q "MKD $dst" "$ftp/" >/dev/null 2>&1 || true
    while read -r d; do
        curl -gs --max-time 15 -u anonymous: -Q "MKD $dst/$d" "$ftp/" >/dev/null 2>&1 || true
    done < "$list"
    ( cd "$src" && find . -type f ! -name .listing | sed 's|^\./||' | sort ) > "$list"
    if [[ -n $skip ]]; then comm -23 "$list" "$skip" > "$list.todo"; mv "$list.todo" "$list"; fi
    while read -r f; do
        local dir=$dst
        [[ $f == */* ]] && dir=$dst/${f%/*}
        curl -gsS --max-time 3600 -u anonymous: -T "$src/$f" "$ftp$dir/" >/dev/null || fail "upload of $f failed"
        printf '    %s\n' "$f"
    done < "$list"
    rm -f "$list"
}

if [[ ${1:-} == --game-data ]]; then
    host=${2:?usage: build.sh --game-data <ip> <dump>}
    dump=${3:?usage: build.sh --game-data <ip> <dump>}
    [[ -d $dump/updates/655360 ]] && dump=$dump/updates/655360
    [[ -f $dump/exefs/main && -d $dump/romfs ]] || fail "no exefs/main and romfs/ in $dump"
    echo "==> game data from $dump to $tid on $host"
    for d in /data/homebrew "/data/homebrew/$tid"; do curl -s --max-time 15 -u anonymous: -Q "MKD $d" "ftp://$host:2121/" >/dev/null 2>&1 || true; done
    upload_tree "$host" "$dump/exefs" "/data/homebrew/$tid/exefs"
    upload_tree "$host" "$dump/romfs" "/data/homebrew/$tid/romfs"
    # ps5/setup.sh skips this step when the marker is there
    date | curl -s --max-time 15 -u anonymous: -T - "ftp://$host:2121/data/homebrew/$tid/game-data.ok" >/dev/null || true
    echo "    done"
    exit 0
fi

if [[ ${1:-} == --shaders ]]; then
    host=${2:?usage: build.sh --shaders <ip> <spirv dir> [<shader_order.txt>]}
    spirv=${3:?usage: build.sh --shaders <ip> <spirv dir> [<shader_order.txt>]}
    [[ -f $spirv/results.tsv ]] || fail "no results.tsv in $spirv"
    echo "==> shader cache from $spirv to $tid on $host"
    for d in "/data/homebrew/$tid/shader_cache"; do curl -s --max-time 15 -u anonymous: -Q "MKD $d" "ftp://$host:2121/" >/dev/null 2>&1 || true; done
    # About 100,000 files: those already on the console are skipped, so an upload that stopped goes on where it was;
    # results.tsv (the index) last, so the console never reads an index whose shaders are not all there.
    # (The console's FTP server has no NLST: the full LIST, a few minutes for 100,000 files. A file counts as there when
    # its size matches, so one cut short by a stopped upload is sent again.)
    have=$(mktemp) remote=$(mktemp)
    curl -s --max-time 1800 -u anonymous: "ftp://$host:2121/data/homebrew/$tid/shader_cache/spirv/" | tr -d '\r' |
        awk '$1 ~ /^-/ {print $9 "\t" $5}' | sort > "$remote" || true
    ( cd "$spirv" && find . -maxdepth 1 -type f -name '*.spv' -printf '%f\t%s\n' | sort ) | comm -12 - "$remote" | cut -f1 > "$have"
    echo "    $(wc -l < "$have") already on the console"
    echo results.tsv >> "$have"; sort -o "$have" "$have"; rm -f "$remote"
    upload_tree "$host" "$spirv" "/data/homebrew/$tid/shader_cache/spirv" "$have" | awk 'NR % 5000 == 0 {print "    " NR " uploaded"}'
    curl -gsS --max-time 600 -u anonymous: -T "$spirv/results.tsv" "ftp://$host:2121/data/homebrew/$tid/shader_cache/spirv/" >/dev/null || fail "results.tsv upload"
    rm -f "$have"
    if [[ -n ${4:-} ]]; then
        curl -gs --max-time 60 -u anonymous: -T "$4" "ftp://$host:2121/data/homebrew/$tid/shader_order.txt" || fail "shader_order.txt upload"
    fi
    echo "    done"
    exit 0
fi

[[ -f $sdk/toolchain/prospero.cmake ]] || fail "no PS5 SDK at $sdk"
[[ -x $tool ]] || fail "no ps5-native-tool at $tool"
[[ -d $lift/main ]] || fail "no lifted game at $lift (run ps5/game/lift.sh)"
[[ -d $deps/volk ]] || fail "no graphics dependencies at $deps (run ps5/tools/fetch-deps.sh)"
pkp_need PKP_AGC_STUBS PKP_SDL3_INCLUDE PKP_SDL3_LIB
[[ -f $agc/libSceAgc.so && -f $agc/libSceAgcDriver.so ]] || fail "no AGC link stubs in $agc"
[[ -f $PKP_SDL3_LIB && -f $PKP_SDL3_INCLUDE/SDL3/SDL.h ]] || fail "no PS5 SDL3 at $PKP_SDL3_LIB / $PKP_SDL3_INCLUDE"
[[ -f $radv ]] || fail "no RADV archive at $radv (build PS5_Vulkan, or set PKP_RADV_ARCHIVE)"

# Link stubs for system libraries the public SDK has none of (ps5/stubs): libSceCommonDialog (the console keyboard needs it).
stubs=$work/stubs
mkdir -p "$stubs"
"$sdk/bin/prospero-clang" -O2 -fPIC -c "$ps5/stubs/common_dialog_stub.c" -o "$stubs/libSceCommonDialog.o"
"$sdk/bin/prospero-lld" --shared -soname libSceCommonDialog.prx -o "$stubs/libSceCommonDialog.so" "$stubs/libSceCommonDialog.o"

echo "==> [1/4] build"
cmake -S "$ps5" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DPKP_LIFT="$lift" -DPKP_DEPS="$deps" -DPKP_BUILD_DIR="$work" -DPKP_AGC_STUBS="$agc" -DPKP_RADV_ARCHIVE="$radv"     -DPKP_SDL3_INCLUDE="$PKP_SDL3_INCLUDE" -DPKP_SDL3_LIB="$PKP_SDL3_LIB" -DPKP_LIBVPX="${PKP_LIBVPX:-$deps/libvpx}" >/dev/null
cmake --build "$build" --target pokopia_ps5 -j "${JOBS:-3}"

echo "==> [2/4] convert and sign"
rm -rf "$dist"
mkdir -p "$build/link" "$dist/sce_sys" "$dist/sce_module"
"$tool" link --in "$build/pokopia_ps5" --out "$build/link/eboot.elf" --stub-dir "$sdk/target/lib" \
    --stub "$agc/libSceAgc.so" --stub "$agc/libSceAgcDriver.so" --stub "$stubs/libSceCommonDialog.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$build/link/eboot.elf" --out "$dist/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$dist/eboot.bin" | grep -E 'container|integrity'

echo "==> [3/4] title folder"
cp "$here/param.json" "$dist/sce_sys/param.json"
for icon in "$sdk/samples/install_app/FAKE02932/icon0.png" "$HOME/ps5-sdk/samples/install_app/FAKE02932/icon0.png"; do
    [[ -f $icon ]] && { cp "$icon" "$dist/sce_sys/icon0.png"; break; }
done
# your own home screen pictures (PKP_ART in ps5/local.env: a folder with icon0.png 512x512 and/or the backdrops pic0.dds and
# pic1.dds, 3840x2160 BC7 DDS from PS5_Vulkan's tools/prepare-assets.sh; never committed)
for pic in icon0.png pic0.dds pic1.dds; do
    [[ -n ${PKP_ART:-} && -f $PKP_ART/$pic ]] && cp "$PKP_ART/$pic" "$dist/sce_sys/$pic"
done
cp "$vulkan/runtime/libc.prx" "$dist/sce_module/libc.prx"
mkdir -p "$dist/fonts"
cp "$ps5/../runtime/assets/fonts/DejaVuSans.ttf" "$dist/fonts/"   # the shared-font fallback (services/pl.cpp): the game needs one
printf '    %s: eboot.bin %s bytes\n' "$dist" "$(stat -c%s "$dist/eboot.bin")"

if [[ ${1:-} == --deploy ]]; then
    host=${2:-${PS5_HOST:?usage: build.sh --deploy <console-ip> (or PS5_HOST in ps5/local.env)}}
    echo "==> [4/4] upload to $host:/data/homebrew/$tid"
    for d in /data/homebrew "/data/homebrew/$tid"; do curl -s --max-time 15 -u anonymous: -Q "MKD $d" "ftp://$host:2121/" >/dev/null 2>&1 || true; done
    # eboot.bin last: the title is not launchable until its main binary is there.
    upload_tree "$host" "$dist/sce_sys" "/data/homebrew/$tid/sce_sys"
    upload_tree "$host" "$dist/sce_module" "/data/homebrew/$tid/sce_module"
    upload_tree "$host" "$dist/fonts" "/data/homebrew/$tid/fonts"
    curl -sS --max-time 600 -u anonymous: -T "$dist/eboot.bin" "ftp://$host:2121/data/homebrew/$tid/" >/dev/null
    echo "    uploaded: launch $tid, then read /data/homebrew/$tid/pkp-results/"
else
    echo "==> [4/4] not uploaded (pass --deploy [<console-ip>])"
fi

#!/usr/bin/env bash
# The game title (PPSA90006): Pokémon Pokopia 2.0.1 on the PS5 runtime (ps5/game/pkp_ps5_main.cpp).
#
#   ps5/game/build.sh                          build, link and sign: dist/PPSA90006/ (no game data in it)
#   ps5/game/build.sh --deploy <ip>            ... and upload the title to /data/homebrew/PPSA90006
#   ps5/game/build.sh --game-data <ip> <dump>  upload exefs/ and romfs/ of the player's own 2.0.1 dump into that folder
#                                              (<dump> holds updates/655360/ or exefs/ + romfs/; once: 6.75 GB)
#
# Needs ps5/game/lift.sh's output (PKP_LIFT, default /home/kai/pkp-ps5-build/game-lift) and ps5/tools/fetch-deps.sh's
# (PKP_DEPS). The title carries no game files: the game data goes to the console's own title folder only, from the
# player's dump, and dist/ never holds it.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ps5=$(dirname "$here")
sdk=${PS5_SDK:-/home/kai/khdays-build/ps5-sdk}
vulkan=${PS5_VULKAN_DIR:-/mnt/c/kb/PS5_Vulkan}
tool=${PS5_NATIVE_TOOL:-$vulkan/build/host/ps5-native-tool}
work=${PKP_BUILD:-/home/kai/pkp-ps5-build}
lift=${PKP_LIFT:-$work/game-lift}
deps=${PKP_DEPS:-$work/deps}
build=$work/game
tid=PPSA90006
dist=$ps5/../dist/$tid

fail() { printf 'error: %s\n' "$*" >&2; exit 2; }

# -g: dump folders carry brackets ("Pokopia_v2.0.1a[Base_Game_and_Update]"), which curl would read as a URL glob. The FTP
# client's .listing files are not the game's. A failed upload ends the script (the loop reads from a file, not a pipe, so
# fail() is in this shell).
upload_tree() {   # host local-dir remote-dir
    local ftp="ftp://$1:2121" src=$2 dst=$3 list
    list=$(mktemp)
    ( cd "$src" && find . -type d | sort ) > "$list"
    while read -r d; do
        curl -gs --max-time 15 -u anonymous: -Q "MKD $dst/${d#./}" "$ftp/" >/dev/null 2>&1 || true
    done < "$list"
    ( cd "$src" && find . -type f ! -name .listing | sort ) > "$list"
    while read -r f; do
        curl -gsS --max-time 3600 -u anonymous: -T "$src/${f#./}" "$ftp$dst/$(dirname "${f#./}")/" >/dev/null ||
            fail "upload of $f failed"
        printf '    %s\n' "${f#./}"
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
    echo "    done"
    exit 0
fi

[[ -f $sdk/toolchain/prospero.cmake ]] || fail "no PS5 SDK at $sdk"
[[ -x $tool ]] || fail "no ps5-native-tool at $tool"
[[ -d $lift/main ]] || fail "no lifted game at $lift (run ps5/game/lift.sh)"
[[ -d $deps/volk ]] || fail "no graphics dependencies at $deps (run ps5/tools/fetch-deps.sh)"

echo "==> [1/4] build"
cmake -S "$ps5" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DPKP_LIFT="$lift" -DPKP_DEPS="$deps" >/dev/null
cmake --build "$build" --target pokopia_ps5 -j "${JOBS:-3}"

echo "==> [2/4] convert and sign"
rm -rf "$dist"
mkdir -p "$build/link" "$dist/sce_sys" "$dist/sce_module"
"$tool" link --in "$build/pokopia_ps5" --out "$build/link/eboot.elf" --stub-dir "$sdk/target/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$build/link/eboot.elf" --out "$dist/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$dist/eboot.bin" | grep -E 'container|integrity'

echo "==> [3/4] title folder"
cp "$here/param.json" "$dist/sce_sys/param.json"
for icon in "$sdk/samples/install_app/FAKE02932/icon0.png" "$HOME/ps5-sdk/samples/install_app/FAKE02932/icon0.png"; do
    [[ -f $icon ]] && { cp "$icon" "$dist/sce_sys/icon0.png"; break; }
done
cp "$vulkan/runtime/libc.prx" "$dist/sce_module/libc.prx"
mkdir -p "$dist/fonts"
cp "$ps5/../runtime/assets/fonts/DejaVuSans.ttf" "$dist/fonts/"   # the shared-font fallback (services/pl.cpp): the game needs one
printf '    %s: eboot.bin %s bytes\n' "$dist" "$(stat -c%s "$dist/eboot.bin")"

if [[ ${1:-} == --deploy ]]; then
    host=${2:?usage: build.sh --deploy <console-ip>}
    echo "==> [4/4] upload to $host:/data/homebrew/$tid"
    for d in /data/homebrew "/data/homebrew/$tid"; do curl -s --max-time 15 -u anonymous: -Q "MKD $d" "ftp://$host:2121/" >/dev/null 2>&1 || true; done
    # eboot.bin last: the title is not launchable until its main binary is there.
    upload_tree "$host" "$dist/sce_sys" "/data/homebrew/$tid/sce_sys"
    upload_tree "$host" "$dist/sce_module" "/data/homebrew/$tid/sce_module"
    upload_tree "$host" "$dist/fonts" "/data/homebrew/$tid/fonts"
    curl -sS --max-time 600 -u anonymous: -T "$dist/eboot.bin" "ftp://$host:2121/data/homebrew/$tid/" >/dev/null
    echo "    uploaded: launch $tid, then read /data/homebrew/$tid/pkp-results/"
else
    echo "==> [4/4] not uploaded (pass --deploy <console-ip>)"
fi

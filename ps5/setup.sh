#!/usr/bin/env bash
# One command from the player's own dump to the game on the console (PPSA90006), on Linux or WSL:
#
#   ps5/setup.sh <dump> [--shaders <spirv-dir> [<shader_order.txt>]] [--host <ip>] [--skip-data] [--relift]
#
#   <dump>       the 2.0.1 game: a folder with exefs/ and romfs/ (or updates/655360/ holding them)
#   --shaders    a translated shader cache (<hash>.spv + results.tsv), made on a PC from the same romfs: the Windows
#                launcher's set-up, or tools/shaders' spirv_harness --batch (tools/shaders/README.md). Without one the
#                game still runs, but translates nothing: a missing shader is skipped (pieces of the scene stay unseen).
#   --host       the console (default PS5_HOST in ps5/local.env); its FTP server on port 2121 must be running
#   --skip-data  the console already has exefs/ and romfs/ (6.75 GB: the slow part of a first set-up)
#   --relift     lift the game again even when the lifted modules are there
#
# Steps: check the tools and the dump, fetch the graphics sources, lift the game (about 20 minutes), build and sign the
# title, upload the game data, the shaders and the title. Every step that is already done is skipped, so running it again
# after a failure goes on from there. Nothing made from the game is written into the repository (LEGAL.md).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$here/env.sh"

usage() { sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }
fail() { printf 'error: %s\n' "$*" >&2; exit 2; }
step() { printf '\n==> %s\n' "$*"; }

dump="" spirv="" order="" host=${PS5_HOST:-} skip_data=0 relift=0
while (($#)); do
    case $1 in
        --shaders) spirv=${2:?--shaders needs a folder}; shift 2
                   if [[ ${1:-} && ${1:0:2} != -- ]]; then order=$1; shift; fi ;;
        --host) host=${2:?--host needs an address}; shift 2 ;;
        --skip-data) skip_data=1; shift ;;
        --relift) relift=1; shift ;;
        -h|--help) usage ;;
        -*) fail "unknown option $1" ;;
        *) [[ -z $dump ]] || fail "one dump only"; dump=$1; shift ;;
    esac
done
[[ -n $dump ]] || usage
[[ -n $host ]] || fail "no console address: --host <ip>, or PS5_HOST in ps5/local.env"

step "[1/7] tools and settings"
pkp_need PS5_SDK PS5_VULKAN_DIR PKP_BUILD PKP_AGC_STUBS PKP_SDL3_INCLUDE PKP_SDL3_LIB
for t in cmake ninja curl unzip sha256sum nasm make; do command -v "$t" >/dev/null || fail "$t is not installed"; done
python=${PKP_PYTHON:-python3}
"$python" -c 'import sys; assert sys.version_info >= (3, 11)' 2>/dev/null || fail "$python is not Python 3.11 or newer (PKP_PYTHON)"
[[ -x $PS5_SDK/bin/prospero-clang ]] || fail "no PS5 SDK at $PS5_SDK"
tool=${PS5_NATIVE_TOOL:-$PS5_VULKAN_DIR/build/host/ps5-native-tool}
[[ -x $tool ]] || fail "no ps5-native-tool at $tool (build PS5_Vulkan's host tools)"
echo "    ok: SDK, PS5_Vulkan, AGC stubs, SDL3, $("$python" --version)"

step "[2/7] the dump"
[[ -d $dump/updates/655360 ]] && dump=$dump/updates/655360
[[ -f $dump/exefs/main && -d $dump/romfs ]] || fail "no exefs/main and romfs/ in $dump"
"$python" - "$dump/exefs/main" <<'PY'
import sys
bid = open(sys.argv[1], 'rb').read(0x60)[0x40:0x54].hex().upper()
if bid != '76A489F3BD3642FA5E18BD83D32C98BACC5F7D0E':
    raise SystemExit('error: exefs/main is build %s, not 2.0.1a (76A489F3...): only 2.0.1a is supported' % bid)
print('    2.0.1a (main build %s...)' % bid[:16])
PY
for m in rtld sdk subsdk0; do [[ -f $dump/exefs/$m ]] || fail "no exefs/$m in the dump"; done

step "[3/7] graphics sources and the movie decoder"
deps=${PKP_DEPS:-$PKP_BUILD/deps}
if [[ -d $deps/volk && -d $deps/vma && -d $deps/imgui && -d $deps/vulkan_headers ]]; then echo "    already there: $deps"
else bash "$here/tools/fetch-deps.sh" "$deps"; fi
if [[ -f $deps/libvpx/lib/libvpx.a ]]; then echo "    libvpx already built"
else bash "$here/tools/build-libvpx.sh" "$deps/libvpx"; fi

step "[4/7] lift the game"
lift=${PKP_LIFT:-$PKP_BUILD/game-lift}
if ((relift == 0)) && [[ -f $lift/.done ]]; then echo "    already lifted: $lift (--relift to do it again)"
else
    rm -f "$lift/.done"
    bash "$here/game/lift.sh" "$dump/exefs" "$lift"
    date > "$lift/.done"
fi

step "[5/7] build and sign"
PKP_LIFT=$lift PKP_DEPS=$deps bash "$here/game/build.sh"

step "[6/7] game data and shaders on $host"
curl -s --max-time 10 -u anonymous: "ftp://$host:2121/" >/dev/null || fail "no FTP server at $host:2121 (start one on the console)"
remote_size() { curl -gsI --max-time 20 -u anonymous: "ftp://$host:2121$1" | tr -d '\r' | awk -F': ' 'tolower($1)=="content-length"{print $2}'; }
if ((skip_data)); then echo "    skipped (--skip-data)"
elif [[ -n $(remote_size /data/homebrew/PPSA90006/game-data.ok || true) ]]; then
    echo "    already there (an earlier set-up uploaded it all)"
else
    bash "$here/game/build.sh" --game-data "$host" "$dump"
fi
if [[ -n $spirv ]]; then bash "$here/game/build.sh" --shaders "$host" "$spirv" ${order:+"$order"}
else echo "    no shader cache given (--shaders): the game runs without translated shaders"; fi

step "[7/7] the title"
bash "$here/game/build.sh" --deploy "$host"
echo
echo "Done. Start the title PPSA90006 from the console's home screen (install it once with your homebrew installer, as for"
echo "any fake-signed title). Logs: /data/homebrew/PPSA90006/pkp-results/pkp.log"

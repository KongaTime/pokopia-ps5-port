#!/usr/bin/env bash
# Lifts the four game modules of the player's own 2.0.1 dump to C++, as recomp/games/pkp/CMakeLists.txt does on Windows
# (same names, indexes, bases, shard counts, game config and hook sites), on Linux for the PS5 build.
#
#   ps5/game/lift.sh <exefs-dir> [out-dir]      out-dir default $PKP_BUILD/game-lift (ps5/local.env)
#
# The output is made from the dump and stays outside the repository (LEGAL.md). NX_JOBS (default 6) bounds the lifter's
# processes: the WSL machine has 7 GB.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
source "$here/../env.sh"
exefs=${1:?usage: lift.sh <exefs-dir> [out-dir]}
out=${2:-${PKP_LIFT:-${PKP_BUILD:?set PKP_BUILD in ps5/local.env or pass out-dir}/game-lift}}
python=${PKP_PYTHON:-python3}
jobs=${NX_JOBS:-6}

"$python" - "$exefs/main" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read(0x60)
bid = d[0x40:0x54].hex().upper()
if bid != '76A489F3BD3642FA5E18BD83D32C98BACC5F7D0E':
    raise SystemExit('main is build %s, not 2.0.1 (76A489F3...): the hook sites are for 2.0.1 only' % bid)
print('main: 2.0.1 build', bid)
PY

mkdir -p "$out"
lift() {   # module index base shards [extra args]
    local name=$1 index=$2 base=$3 shards=$4; shift 4
    echo "==> $name ($shards shards) $(date +%T)"
    ( cd "$root/recomp" && "$python" -m nxrecomp lift "$exefs/$name" "$out/$name" --name "$name" --index "$index" \
        --base "$base" --game pkp --shards "$shards" --jobs "$jobs" "$@" )
}
lift rtld 3 0x7700000000 1
lift subsdk0 2 0x7600000000 4
lift sdk 1 0x7500000000 45
lift main 0 0x7100000000 230 --sites "$root/recomp/games/pkp/main.sites"
echo "==> done $(date +%T)"; du -sh "$out"/*

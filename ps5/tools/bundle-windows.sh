#!/usr/bin/env bash
# The launcher's PS5 toolchain folder (tools/ps5 in the release), for players who build the title on Windows without WSL.
#
#   ps5/tools/bundle-windows.sh <out-dir> <clang+llvm-X-x86_64-pc-windows-msvc dir> <zig.exe>
#
# Run on Linux / WSL after ps5/game/build.sh (its build tree is where the prebuilt libraries and the compile and link recipe come
# from). The LLVM folder is LLVM's official Windows release of the same version as the Linux clang, extracted; zig.exe builds
# ps5-native-tool.exe. See ps5/tools/bundle_windows.py for what the folder holds; nothing in it comes from the game.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$here/../env.sh"
pkp_need PS5_SDK PS5_VULKAN_DIR PKP_BUILD PKP_AGC_STUBS
out=${1:?usage: bundle-windows.sh <out-dir> <llvm-windows-dir> <zig.exe>}
llvm=${2:?usage: bundle-windows.sh <out-dir> <llvm-windows-dir> <zig.exe>}
zig=${3:?usage: bundle-windows.sh <out-dir> <llvm-windows-dir> <zig.exe>}
python=${PKP_PYTHON:-python3}
PATH="$PS5_SDK/bin:$PATH" "$python" "$here/bundle_windows.py" --out "$out" --llvm "$llvm" --zig "$zig"

#!/usr/bin/env bash
# The graphics shell's pinned third-party sources (runtime/cmake/gfx_deps.cmake: the same URLs and SHA256s), fetched and
# unpacked for the PS5 build. SDL is not among them: the console build takes KongaTime's PS5 SDL3 (ps5/CMakeLists.txt).
#   ps5/tools/fetch-deps.sh [dir]        default $PKP_BUILD/deps (ps5/local.env)
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/../env.sh"
dir=${1:-${PKP_DEPS:-${PKP_BUILD:?set PKP_BUILD in ps5/local.env or pass dir}/deps}}
mkdir -p "$dir/archives"
fetch() {   # name url sha256
    local name=$1 url=$2 sum=$3 file=$dir/archives/$1-$(basename "$2")   # Vulkan-Headers and volk share a file name
    [[ -f $file ]] || curl -sSL --max-time 300 -o "$file" "$url"
    echo "$sum  $file" | sha256sum -c --quiet - || { echo "checksum mismatch: $file" >&2; exit 1; }
    rm -rf "$dir/$name" "$dir/.tmp" && mkdir -p "$dir/.tmp"
    unzip -q "$file" -d "$dir/.tmp" && mv "$dir/.tmp"/* "$dir/$name" && rmdir "$dir/.tmp"
    echo "$name: $(ls "$dir/$name" | head -3 | tr '\n' ' ')"
}
fetch vulkan_headers https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.zip \
    f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81
fetch volk https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.zip \
    889b90fdcbe9cee7b118339d640c6895b7be5c920d309c44b744692063275b3d
fetch vma https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.zip \
    ce9a61ea5e72284ac9ccc152c214eef381ab57bfd7383f20232e5a652e3afda6
fetch imgui https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip \
    e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8

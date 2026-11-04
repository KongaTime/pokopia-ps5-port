// SPDX-License-Identifier: GPL-3.0-or-later
// The graphics shell's Windows-only pieces, as the console build answers them (ps5/CMakeLists.txt leaves their files out):
//  - DLSS (extras/temporal_dlss.cpp, NVIDIA Streamline): not available, never started; streamline_preinit gives no loader,
//    so vk_device.cpp uses the normal one. FSR (temporal_ffx) stays, as on an AMD GPU on Windows.
//  - the Windows display HDR query (extras/win_display.cpp): no answer (valid = false), so the renderer keeps its SDR path
//    until the console's own HDR query is written (with the renderer, P3).
//  - vk_icdGetInstanceProcAddr: RADV's entry, which the console's SDL3 Vulkan driver names. P2 links no RADV ([boot] gfx =
//    none: no Vulkan device is made); the weak null here keeps that link, and RADV's own definition replaces it in P3.
#include <cstdio>

#include <vulkan/vulkan.h>

#include "extras/temporal.h"
#include "extras/win_display.h"

namespace nxrt::extras {
bool temporal_dlss_available() { return false; }
bool temporal_dlss_started() { return false; }
TemporalBackend* temporal_dlss(void*, bool) { return nullptr; }
PFN_vkGetInstanceProcAddr streamline_preinit() { return nullptr; }

bool query_display_hdr(void*, DisplayHdrInfo* out) {
    if (!out) return false;
    *out = DisplayHdrInfo{};
    std::snprintf(out->how, sizeof out->how, "no display HDR query on the console yet");
    return false;
}
}  // namespace nxrt::extras

extern "C" __attribute__((weak)) PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char*) { return nullptr; }

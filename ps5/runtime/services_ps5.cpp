// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 back ends of the runtime's Windows-only service pieces (ps5/PLAN.md, "Compat layer status"), first versions:
//  - audio:      XAudio2 is not on the console; the sceAudioOut sink stands in for it (audio_ps5.cpp)
//                (when the port cannot be opened the audio layer falls back to the null sink, audio_sink.h).
//  - album:      caps:su's IScreenShotApplicationService without WIC: the commands are accepted and nothing is saved yet.
//  - swkbd:      no native prompt window: the keyboard's text comes from [applet] swkbd_text, the game's initial text, or
//                "Player" (services_api.h's order without the PC prompt). The console's keyboard is ime_ps5.cpp.
//  - movie:      no Media Foundation: the game's own movie library in subsdk0 runs (install_movie_hle answers false, as
//                movie_mf.cpp does when MF is unavailable). The ffmpeg back end comes with P4.
//  - graphics:   hdr_note_layer_luminance belongs to the graphics shell (src/gfx/extras), which this build leaves out until P3.
#include "nxrt/audio_sink.h"
#include "nxrt/log.h"
#include "nxrt/movie.h"
#include "nxrt/services_api.h"
#include "../../runtime/src/services/services_internal.h"

namespace nxrt::audio {
// make_xaudio2_sink: the sceAudioOut sink, ps5/runtime/audio_ps5.cpp
std::vector<OutputDevice> list_output_devices() { return {}; }   // MMDevice: no endpoint list on the console (one output)
}  // namespace nxrt::audio

namespace nxrt::services {

std::shared_ptr<ipc::Service> make_screenshot_app() {
    return make_iface("IScreenShotApplicationService", [](ipc::Service& s) {
        s.add_cmd(32, "SetShimLibraryVersion", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        for (uint32_t id : {203u, 205u, 210u}) {
            s.add_cmd(id, "SaveScreenShotEx", [](ipc::Request&, ipc::Response&) -> uint32_t {
                NXLOG_I("caps", "screenshot requested: not saved on the console yet");
                return 0;
            });
        }
    });
}

// swkbdui::win32_prompt: the console keyboard (sceImeDialog), ps5/runtime/ime_ps5.cpp

}  // namespace nxrt::services

namespace nxrt::media {
bool install_movie_hle() { return false; }
const char* const* movie_import_names(size_t* count) {
    if (count) *count = 0;
    return nullptr;
}
MovieStats movie_stats() { return {}; }
}  // namespace nxrt::media

namespace nxrt::extras {
// Weak: a build that links the graphics shell (the game title) gets the real one from src/gfx/extras.
__attribute__((weak)) void hdr_note_layer_luminance(bool, unsigned, unsigned) {}
}  // namespace nxrt::extras

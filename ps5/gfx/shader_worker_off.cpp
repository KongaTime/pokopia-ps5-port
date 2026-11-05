// SPDX-License-Identifier: GPL-3.0-or-later
// The shader translator worker (runtime/src/gfx/nvn/nvn_shader_worker.cpp) runs spirv_harness.exe as a Windows child process to
// translate shaders missing from the cache while the game runs. A console title cannot start processes: the PS5 build leaves that
// file out (ps5/CMakeLists.txt) and answers here as a worker that never starts, so the back end keeps to the cache made on a PC
// (ps5/game/build.sh --shaders).
#include "nvn_shader_worker.h"

namespace nxrt::nvn::be {

ShaderTranslatorClient::~ShaderTranslatorClient() { stop(); }

bool ShaderTranslatorClient::start(const Options&, RowFn, LogFn, std::string* error) {
    if (error) *error = "no shader translator on the console (the cache is made on a PC)";
    return false;
}

bool ShaderTranslatorClient::submit(const std::string&, const uint8_t*, size_t, const uint8_t*, size_t) { return false; }

ShaderTranslatorClient::State ShaderTranslatorClient::state(const std::string&) const { return State::Unknown; }

size_t ShaderTranslatorClient::pending() const { return 0; }

void ShaderTranslatorClient::writer() {}
void ShaderTranslatorClient::reader() {}
void ShaderTranslatorClient::finish(const std::string&, bool) {}
void ShaderTranslatorClient::stop() {}

}  // namespace nxrt::nvn::be

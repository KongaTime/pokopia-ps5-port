// SPDX-License-Identifier: GPL-3.0-or-later
// The test tooling (src/testtools) as the runtime sees it with no [test] key set: off. The console build leaves the
// tooling out (its game probes read guest memory under SEH), so these are the answers tt_core.cpp gives when it is off:
// no deterministic mode, host time and sleeps, OS entropy, and no per-SVC frame source.
#include "nxrt/tt.h"
#include "../../runtime/src/testtools/tt_internal.h"

#include <windows.h>

#include <cstdlib>
#include <string>

namespace nxrt::tt {

bool init() { return false; }
void shutdown() {}
bool enabled() { return false; }
bool deterministic() { return false; }
bool virtual_tick(uint64_t*) { return false; }
bool virtual_sleep(int64_t) { return false; }
void rng_fill(void* p, size_t n) { arc4random_buf(p, n); }

// The plumbing the graphics shell and the game module register into it: kept nowhere, since no run is scripted.
void set_screenshot_provider(ScreenshotFn, void*) {}
void set_mouse_sink(MouseSinkFn) {}
void set_key_sink(KeySinkFn) {}
bool finished() { return false; }
void end_run(int code, const char*, const std::string&) { std::_Exit(code); }

namespace detail {
std::atomic<uint32_t> svc_every{0};
void on_svc(::Ctx*) {}
uint64_t game_base() { return 0x7100000000ull; }   // main's base (nxrt/memory.h kMainBase), as tt_core reports it
// tt_core's read: memory that may be gone answers false instead of faulting (ReadProcessMemory on ourselves).
bool read_mem(uint64_t addr, void* out, size_t n) {
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, n, nullptr) != 0;
}
}  // namespace detail

void (*wait_shot_writes)() = nullptr;   // tt_core.cpp (set by vk_renderer.cpp; nothing waits on it with the tools off)
}  // namespace nxrt::tt

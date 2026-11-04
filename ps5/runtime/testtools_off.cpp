// SPDX-License-Identifier: GPL-3.0-or-later
// The test tooling (src/testtools) as the runtime sees it with no [test] key set: off. The console build leaves the
// tooling out (its game probes read guest memory under SEH), so these are the answers tt_core.cpp gives when it is off:
// no deterministic mode, host time and sleeps, OS entropy, and no per-SVC frame source.
#include "nxrt/tt.h"

#include <cstdlib>

namespace nxrt::tt {

bool init() { return false; }
void shutdown() {}
bool enabled() { return false; }
bool deterministic() { return false; }
bool virtual_tick(uint64_t*) { return false; }
bool virtual_sleep(int64_t) { return false; }
void rng_fill(void* p, size_t n) { arc4random_buf(p, n); }

namespace detail {
std::atomic<uint32_t> svc_every{0};
void on_svc(::Ctx*) {}
}  // namespace detail

}  // namespace nxrt::tt

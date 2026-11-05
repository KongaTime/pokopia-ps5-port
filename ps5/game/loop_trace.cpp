// SPDX-License-Identifier: GPL-3.0-or-later
// Bring-up: the callees of the game's main loop (main 0x7100882c60, lifted into c_049) wrapped at link time (ps5/CMakeLists.txt)
// and logged when the loop calls them (call site in the loop's range): the order of calls shows which way the loop leaves after
// one frame on the console.
#include <atomic>
#include <cstdint>

#include "nxrecomp/ctx.h"
#include "nxrt/log.h"

namespace {
bool from_loop(uint64_t lr) { return lr >= 0x7100882c60ull && lr < 0x71008836f0ull; }
uint64_t peek(uint64_t a) {
    return (a >= 0x7800000000ull && a < 0x7c00000000ull && !(a & 7)) ? *reinterpret_cast<const volatile uint64_t*>(a) : 0;
}
}  // namespace

#define LOOP_WRAP(F)                                                                                                              \
    extern "C" void __real_##F(Ctx* c);                                                                                           \
    extern "C" void __wrap_##F(Ctx* c) {                                                                                          \
        const uint64_t lr = c->x[30], a0 = c->x[0];                                                                               \
        const uint64_t f28 = peek(a0 + 0x28), f48 = peek(a0 + 0x48);                                                               \
        __real_##F(c);                                                                                                            \
        if (from_loop(lr)) {                                                                                                      \
            static std::atomic<int> n{0};                                                                                         \
            if (n.fetch_add(1) < 400)                                                                                             \
                NXLOG_I("looptrace", "%s(%llx [+28] %llx [+48] %llx) at %llx -> %llx", #F, (unsigned long long)a0,                \
                        (unsigned long long)f28, (unsigned long long)f48, (unsigned long long)lr, (unsigned long long)c->x[0]);   \
        }                                                                                                                         \
    }

LOOP_WRAP(f_7101fdb040)
LOOP_WRAP(f_71009fc100)
LOOP_WRAP(f_7101fd9448)
LOOP_WRAP(f_7101fd9440)
LOOP_WRAP(f_7101fdd700)
LOOP_WRAP(f_7101ffaca0)
LOOP_WRAP(f_710069ed3c)
LOOP_WRAP(f_7100234140)
LOOP_WRAP(f_7102045420)
LOOP_WRAP(f_7101ff1c44)
LOOP_WRAP(f_7101fdd6b4)
LOOP_WRAP(f_7101fdcf20)
LOOP_WRAP(f_7101ff1d60)
LOOP_WRAP(f_7101ff1fe0)
LOOP_WRAP(f_7101fdd6bc)
LOOP_WRAP(f_7102045a60)
LOOP_WRAP(f_7101ff1f7c)
LOOP_WRAP(f_7101fdd6c0)

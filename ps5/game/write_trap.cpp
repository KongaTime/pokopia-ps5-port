// SPDX-License-Identifier: GPL-3.0-or-later
// Bring-up: PKP_TRAP=addr (pkp-env.txt) - who writes this guest word. Once it holds a non-zero value, its page is made read-only;
// a write fault on the page is caught by a vectored handler that logs the writing guest function (Ctx::fn, lr) when the fault is on
// the word itself, gives the page its access back and lets the write go on. A host thread arms the page again every millisecond, so
// later writes are caught as well. The main loop's frame job (field +0x28 of its handle) is cleared on the console after one frame.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "nxrecomp/ctx.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace {

uint64_t g_addr = 0;
std::atomic<bool> g_armed{false};
std::atomic<int> g_hits{0};

LONG CALLBACK on_fault(PEXCEPTION_POINTERS ep) {
    if (!g_addr || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t a = ep->ExceptionRecord->ExceptionInformation[1];
    const uint64_t page = g_addr & ~uint64_t(0x3fff);
    if (a < page || a >= page + 0x4000) return EXCEPTION_CONTINUE_SEARCH;
    if (a >= g_addr && a < g_addr + 8 && g_hits.fetch_add(1) < 40) {
        ::Ctx* c = nxrt::kernel::current_ctx();
        NXLOG_I("trap", "write to %llx (was %llx) by guest fn %llx lr %llx x0 %llx x1 %llx", (unsigned long long)a,
                (unsigned long long)*reinterpret_cast<const uint64_t*>(g_addr), c ? (unsigned long long)c->fn : 0ull,
                c ? (unsigned long long)c->x[30] : 0ull, c ? (unsigned long long)c->x[0] : 0ull, c ? (unsigned long long)c->x[1] : 0ull);
    }
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(page), 0x4000, PAGE_READWRITE, &old);
    g_armed = false;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void trap_thread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (g_armed) continue;
        if (*reinterpret_cast<volatile const uint64_t*>(g_addr) == 0 && g_hits.load() == 0) continue;   // not set yet
        DWORD old = 0;
        if (VirtualProtect(reinterpret_cast<void*>(g_addr & ~uint64_t(0x3fff)), 0x4000, PAGE_READONLY, &old)) g_armed = true;
    }
}

}  // namespace

// From pkp_ps5_main after the modules are mapped: starts the trap when PKP_TRAP names an address (after PKP_WATCH_AFTER_MS).
void pkp_ps5_start_trap() {
    const char* t = getenv("PKP_TRAP");
    if (!t || !*t) return;
    g_addr = std::strtoull(t, nullptr, 0);
    AddVectoredExceptionHandler(1, on_fault);
    const char* delay = getenv("PKP_WATCH_AFTER_MS");
    const int ms = delay ? std::atoi(delay) : 5000;
    std::thread([ms] {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        NXLOG_I("trap", "write trap on %llx armed", (unsigned long long)g_addr);
        trap_thread();
    }).detach();
}

// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime-provided half of the generated-code ABI (recomp/include/nxrecomp/ctx.h, ABI version 2, section "runtime-provided"):
// call_indirect, host_import, guest_trap, guest_unimpl, guest_svc, guest_mrs, guest_msr, stx_diag. The rest of the section lives next
// to its subsystem: nx_site_inline / nx_site_midhook / nx_hook_entry in hooks.cpp, nx_catch_call in eh/catch_call.cpp, and the
// registration of the lifted module tables (NxModule) in nxmodule.cpp.
// Pattern from smo-recomp runtime/core/hostrt.c (host_import with in_import bookkeeping, stx_diag, guest_trap / guest_unimpl).
#include "nxrt/config.h"
#include <string>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <set>

#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"
#include "nxrt/svc.h"

namespace nxrt {
namespace {
std::mutex g_sysreg_lock;
std::set<uint32_t> g_sysregs_seen;
}  // namespace
}  // namespace nxrt

extern "C" {

// [diag] trace_indirect = "lo,hi[,max]" (NXRT_DIAG_TRACE_INDIRECT): every indirect call made from a guest function whose address is
// in [lo, hi) is logged with its call site, target and the x0 it returns (at most max lines, default 400). Bring-up: which virtual call
// of a code path answers what, without a re-lift for mid hooks.
namespace {
struct IndirectTrace {
    uint64_t lo = 0, hi = 0;
    int max = 400;
    uint64_t after_ns = 0;   // [diag] trace_indirect_after_ms: nothing logged before then (from the first indirect call)
};
const IndirectTrace& indirect_trace() {
    static const IndirectTrace t = [] {
        IndirectTrace r;
        const std::string v = nxrt::config_get_str("diag", "trace_indirect", "");
        if (v.empty()) return r;
        char* end = nullptr;
        r.lo = std::strtoull(v.c_str(), &end, 0);
        if (end && *end == ',') r.hi = std::strtoull(end + 1, &end, 0);
        if (end && *end == ',') r.max = static_cast<int>(std::strtol(end + 1, nullptr, 0));
        r.after_ns = static_cast<uint64_t>(nxrt::config_get_int("diag", "trace_indirect_after_ms", 0)) * 1000000ull;
        return r;
    }();
    return t;
}
std::atomic<int> g_indirect_lines{0};
}  // namespace

void call_indirect(Ctx* c, uint64_t addr) {
    const IndirectTrace& t = indirect_trace();
    static const auto t0 = std::chrono::steady_clock::now();
    if (t.hi && c->fn >= t.lo && c->fn < t.hi && g_indirect_lines.load(std::memory_order_relaxed) < t.max &&
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) >= t.after_ns) {
        const uint64_t from = c->fn, lr = c->x[30], a0 = c->x[0], a1 = c->x[1], a2 = c->x[2];
        nxrt::dispatch_call(c, addr);
        g_indirect_lines.fetch_add(1, std::memory_order_relaxed);
        NXLOG_I("indirect", "%llx (lr %llx) -> %llx (x0 %llx x1 %llx x2 %llx) returned x0 %llx", (unsigned long long)from, (unsigned long long)lr,
                (unsigned long long)addr, (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)c->x[0]);
        c->fn = from;
        return;
    }
    nxrt::dispatch_call(c, addr);
}

// A call to a symbol another module provides: the lifted PLT stub passes NX_IMPORT_ID(module index, symbol index) of its own
// module. The registry (nxmodule.cpp) names the import and finds its target: a function another lifted module defines, else the HLE
// handler or logging stub of that name.
void host_import(Ctx* c, uint32_t import_id) {
    const uint64_t target = nxrt::import_resolve(import_id);
    if (!target) {
        char what[96];
        snprintf(what, sizeof what, "host_import with an unknown import id (module %u, symbol %u)", NX_IMPORT_MODULE(import_id), NX_IMPORT_SYM(import_id));
        nxrt::trap(what, import_id);
    }
    nxrt::detail::count_import();
    const char* saved_name = c->in_import;
    const uint64_t saved_from = c->import_from;
    c->in_import = nxrt::import_name(import_id);
    c->import_from = c->x[30] - 4;
    nxrt::dispatch_call(c, target);
    c->in_import = saved_name;
    c->import_from = saved_from;
}

void guest_trap(const char* what, uint64_t addr) { nxrt::trap(what, addr); }

void guest_unimpl(uint64_t pc, uint32_t insn, const char* what) {
    NXLOG_E("guest", "UNTRANSLATED INSTRUCTION at 0x%llx: %08x (%s)", static_cast<unsigned long long>(pc), insn, what);
    nxrt::log_flush();
    RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void guest_svc(Ctx* c, uint32_t number) { nxrt::svc_dispatch(c, number, 0); }

// System registers the lifter does not model inline: the generic timer counters are real (19.2 MHz system tick), other reads give 0, writes are ignored; each register is reported once.
uint64_t guest_mrs(Ctx* c, uint32_t sysreg) {
    switch (sysreg) {                       // the generic timer: the SDK reads CNTPCT_EL0 33 times (nn::os::GetSystemTick and friends)
        case 0xdf00: return nxrt::kernel::tick_hz();                 // CNTFRQ_EL0: 19.2 MHz (Switch 1) or 31.25 MHz (Switch 2)
        case 0xdf01:                                                // CNTPCT_EL0
        case 0xdf02: return nxrt::kernel::system_tick();            // CNTVCT_EL0
        default: break;
    }
    std::lock_guard<std::mutex> lk(nxrt::g_sysreg_lock);
    if (nxrt::g_sysregs_seen.insert(sysreg).second) NXLOG_W("guest", "MRS of system register 0x%04x (not modelled): returns 0 (in f_%llx)", sysreg, static_cast<unsigned long long>(c->fn));
    return 0;
}

void guest_msr(Ctx* c, uint32_t sysreg, uint64_t value) {
    std::lock_guard<std::mutex> lk(nxrt::g_sysreg_lock);
    if (nxrt::g_sysregs_seen.insert(sysreg | 0x10000u).second) NXLOG_W("guest", "MSR to system register 0x%04x = 0x%llx (not modelled): ignored (in f_%llx)", sysreg, static_cast<unsigned long long>(value), static_cast<unsigned long long>(c->fn));
}

// Called on every failed exclusive store; reports a loop that never succeeds.
void stx_diag(Ctx* c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {
    uint64_t n = ++c->ex_fail;
    if (n == 1000 || n == 100000)
        NXLOG_W("guest", "STXR failing %llu times in f_%llx: addr=%llx expected=%llx seen=%llx new=%llx (%s%d)", static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(c->fn), static_cast<unsigned long long>(a), static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(seen), static_cast<unsigned long long>(v), bits < 0 ? "monitor lost, " : "", bits < 0 ? -bits : bits);
}

}  // extern "C"

// SPDX-License-Identifier: GPL-3.0-or-later
// P1 (ps5/PLAN.md): the runtime's lifted test programs, run on the console by the real runtime, in one title.
//
// It is runtime/tests/lift/driver.cpp for one process: every program (ps5/p1/make_cases.py: linked at a base of its own,
// lifted as module tNN) is mapped by the module loader, the modules are linked together and registered, and each
// test_main runs on a guest thread made by the kernel layer, with the driver's checks: results[] word for word against
// the expected output (Unicorn, or the host's own C++ exceptions), and the callee-saved registers and SP intact across
// the call. 26_eh_threads then runs thread_main on 8 guest threads at once.
//
// The verdict goes to /data/pokopia-p1/results.txt, one line per step written before the step runs, so a file that
// stops says where. The runtime's own log is /data/pokopia-p1/nxrt.log. Read both back over FTP.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "nxrt/dispatch.h"
#include "nxrt/eh.h"
#include "nxrt/hooks.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"
#include "nxrt/runtime.h"

extern "C" int sceKernelSendNotificationRequest(int device, void* request, size_t size, int blocking);

namespace {

constexpr int kNres = 512;
constexpr uint64_t kHostBase = 0x7110000000ull;      // host services of the S1 programs' rt.h (raise / resume)
constexpr uint64_t kHarnessEntry = 0x7120000000ull;  // guest addresses of the thread entries below (host functions)
constexpr const char* kTests = "/app0/tests/";
constexpr const char* kOut = "/data/pokopia-p1";

struct Case {
    const char* name;
    const char* elf;
    const char* expected;
    const NxModule* module;
    int threads;
    int mask[16];
    int nmask;
};
#include "cases.inc"
constexpr int kCount = sizeof kCases / sizeof kCases[0];

std::FILE* g_report = nullptr;

void report(const char* format, ...) {
    va_list a;
    va_start(a, format);
    std::vfprintf(g_report ? g_report : stderr, format, a);
    va_end(a);
    if (g_report) {
        std::fputc('\n', g_report);
        std::fflush(g_report);
        fsync(fileno(g_report));
    }
}

void notify(const char* text) {
    struct {
        char padding[45];
        char message[3075];
    } request{};
    std::snprintf(request.message, sizeof request.message, "%s", text);
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

// ---- the current case (the guest thread entries read these) ----
const Case* g_case = nullptr;
uint64_t g_test_main = 0, g_thread_main = 0, g_results = 0;
std::vector<uint64_t> g_expected;
int g_thread_status = -1;
std::atomic<int> g_thread_failures{0};

bool read_expected(const Case& c) {
    const std::string path = std::string(kTests) + c.expected;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    g_expected.assign(kNres, 0);
    const size_t n = std::fread(g_expected.data(), sizeof(uint64_t), kNres, f);
    std::fclose(f);
    return n == static_cast<size_t>(kNres);
}

bool masked(int i) {
    for (int k = 0; k < g_case->nmask; k++)
        if (g_case->mask[k] == i) return true;
    return false;
}

int compare_results() {
    const uint64_t* got = reinterpret_cast<const uint64_t*>(g_results);
    int bad = 0;
    for (int i = 0; i < kNres; i++) {
        if (masked(i) || got[i] == g_expected[i]) continue;
        if (bad++ < 8)
            report("  results[%d] = 0x%llx, expected 0x%llx", i, static_cast<unsigned long long>(got[i]),
                   static_cast<unsigned long long>(g_expected[i]));
    }
    return bad;
}

// Guest thread entry: sentinels in the callee-saved registers, run test_main, check them (driver.cpp's harness_entry).
void harness_entry(::Ctx* c) {
    const uint64_t sentinel = 0x5eed000000000000ull;
    for (int i = 19; i <= 29; i++) c->x[i] = sentinel + static_cast<uint64_t>(i);
    for (int i = 0; i < 8; i++) {
        c->v[8 + i].d[0] = sentinel + 0x100 + static_cast<uint64_t>(i);
        c->v[8 + i].d[1] = 0;
    }
    c->x[30] = 0;
    const uint64_t sp_in = c->x[31];
    nxrt::dispatch_call(c, g_test_main);
    bool ok = true;
    for (int i = 19; i <= 29; i++)
        if (c->x[i] != sentinel + static_cast<uint64_t>(i)) {
            report("  callee-saved x%d changed: 0x%llx", i, static_cast<unsigned long long>(c->x[i]));
            ok = false;
        }
    for (int i = 0; i < 8; i++)
        if (c->v[8 + i].d[0] != sentinel + 0x100 + static_cast<uint64_t>(i)) {
            report("  callee-saved d%d changed", 8 + i);
            ok = false;
        }
    if (c->x[31] != sp_in) {
        report("  SP changed across test_main: 0x%llx -> 0x%llx", static_cast<unsigned long long>(sp_in),
               static_cast<unsigned long long>(c->x[31]));
        ok = false;
    }
    g_thread_status = ok ? 0 : 1;
}

void thread_entry(::Ctx* c) {
    const uint64_t idx = c->x[0];
    const uint64_t sentinel = 0x7a7a000000000000ull + (idx << 32);
    for (int i = 19; i <= 29; i++) c->x[i] = sentinel + static_cast<uint64_t>(i);
    c->x[30] = 0;
    const uint64_t sp_in = c->x[31];
    nxrt::dispatch_call(c, g_thread_main);
    bool ok = c->x[31] == sp_in;
    for (int i = 19; i <= 29; i++) ok = ok && c->x[i] == sentinel + static_cast<uint64_t>(i);
    if (!ok) g_thread_failures++;
}

uint32_t start_guest_thread(const char* name, uint64_t entry, uint64_t arg, uint64_t stack_size) {
    const uint64_t stack = nxrt::mem::alloc_kernel(stack_size, nxrt::mem::kAllocGranularity);
    if (!stack) return 0;
    nxrt::kernel::ThreadParams p;
    p.name = name;
    p.entry = entry;
    p.arg = arg;
    p.stack_top = stack + stack_size;
    const uint32_t th = nxrt::kernel::thread_create(p);
    if (!th || nxrt::kernel::thread_start(th) != 0) return 0;
    return th;
}

// One case: test_main on a guest thread, then (26_eh_threads) thread_main on N threads, then the comparison.
bool run_case(const Case& c, nxrt::Module* m) {
    g_case = &c;
    g_test_main = m->find_symbol("test_main");
    g_results = m->find_symbol("results");
    if (!g_test_main || !g_results) {
        report("FAIL %s: no test_main / results symbol", c.name);
        return false;
    }
    if (!read_expected(c)) {
        report("FAIL %s: cannot read %s%s", c.name, kTests, c.expected);
        return false;
    }
    g_thread_status = -1;
    const uint32_t th = start_guest_thread("lift-test", kHarnessEntry, 0, 8ull << 20);
    if (!th) {
        report("FAIL %s: cannot start the guest thread", c.name);
        return false;
    }
    if (!nxrt::kernel::thread_join(th, 120000)) {
        report("FAIL %s: the guest thread did not finish in 120 s", c.name);
        return false;
    }
    if (c.threads > 0) {
        g_thread_main = m->find_symbol("thread_main");
        g_thread_failures = 0;
        std::vector<uint32_t> handles;
        for (int i = 0; i < c.threads; i++) {
            const uint32_t h = start_guest_thread("lift-thread", kHarnessEntry + 16, static_cast<uint64_t>(i), 1ull << 20);
            if (!h) {
                report("FAIL %s: cannot start thread %d", c.name, i);
                return false;
            }
            handles.push_back(h);
        }
        for (uint32_t h : handles)
            if (!nxrt::kernel::thread_join(h, 120000)) {
                report("FAIL %s: a guest thread did not finish in 120 s", c.name);
                return false;
            }
        if (g_thread_failures) g_thread_status = 1;
    }
    const int bad = compare_results();
    const bool ok = g_thread_status == 0 && bad == 0;
    report("%s %s%s%s", ok ? "PASS" : "FAIL", c.name, bad ? " (results differ)" : "",
           g_thread_status != 0 ? " (registers or SP not preserved)" : "");
    return ok;
}

int run() {
    nxrt::InitOptions io;
    const std::string log = std::string(kOut) + "/nxrt.log";
    io.ini_path = "";
    io.log_path = log.c_str();
    io.watchdog = false;
    io.memguard = false;
    io.load_symbols = false;
    report("STAGE runtime init");
    if (!nxrt::init(io)) {
        report("FAIL runtime init");
        return 2;
    }
    nxrt::eh::register_unwind_hle();
    nxrt::fn_register(kHostBase + 0, nxrt::eh::legacy_raise);
    nxrt::fn_register(kHostBase + 16, nxrt::eh::legacy_resume);
    nxrt::fn_register(kHarnessEntry, harness_entry);
    nxrt::fn_register(kHarnessEntry + 16, thread_entry);

    report("STAGE load %d programs", kCount);
    std::vector<nxrt::Module*> mods;
    for (const Case& c : kCases) {
        std::string err;
        const std::string path = std::string(kTests) + c.elf;
        nxrt::Module* m = nxrt::module_load_file(path.c_str(), {}, &err);
        if (!m) {
            report("FAIL load %s: %s", c.name, err.c_str());
            return 2;
        }
        mods.push_back(m);
    }
    report("STAGE link");
    if (!nxrt::module_link_all()) {
        report("FAIL link");
        return 2;
    }

    // As the Windows build checks with 00_smoke: a module lifted against another ABI version is refused and leaves nothing.
    report("STAGE ABI mismatch check");
    bool abi_ok = true;
    for (uint32_t wrong : {nxrt::kAbiVersion - 1, nxrt::kAbiVersion + 1, 0u}) {
        nxrt::RegResult bad = nxrt::nxmodule_register(mods[0], kCases[0].module, wrong);
        if (bad.status != nxrt::RegStatus::AbiMismatch || bad.functions != 0 || nxrt::nxmodule_by_index(kCases[0].module->index))
            abi_ok = false;
    }
    report("%s abi_mismatch_refused", abi_ok ? "PASS" : "FAIL");

    report("STAGE register %d modules", kCount);
    for (int i = 0; i < kCount; i++) {
        nxrt::RegResult r = NXRT_REGISTER_NXMODULE(mods[i], *kCases[i].module);
        if (!r.ok()) {
            report("FAIL register %s: %s", kCases[i].name, r.message.c_str());
            return 2;
        }
        // entry hooks on the stubs of guest/rt2.h, by this module's own symbols
        struct { const char* sym; nxrt::SiteHookFn fn; } hooks[] = {
            {"_Unwind_RaiseException", nxrt::eh::hook_unwind_raise_exception},
            {"_Unwind_Resume", nxrt::eh::hook_unwind_resume},
        };
        for (auto& h : hooks)
            if (uint64_t a = mods[i]->find_symbol(h.sym)) nxrt::hook_add_entry(a, h.fn);
    }
    nxrt::eh::discover_typeinfo_vtables();

    int passed = abi_ok ? 1 : 0;
    for (int i = 0; i < kCount; i++) {
        report("RUN %s", kCases[i].name);
        if (run_case(kCases[i], mods[i])) passed++;
    }
    const nxrt::eh::Stats st = nxrt::eh::stats();
    report("eh: %llu raised, %llu caught, %llu cleanups, %llu uncaught", static_cast<unsigned long long>(st.raised),
           static_cast<unsigned long long>(st.caught), static_cast<unsigned long long>(st.cleanups),
           static_cast<unsigned long long>(st.uncaught));
    report("RESULT %d/%d passed", passed, kCount + 1);
    nxrt::shutdown();
    char text[96];
    std::snprintf(text, sizeof text, "Pokopia P1: %d/%d passed", passed, kCount + 1);
    notify(text);
    return passed == kCount + 1 ? 0 : 1;
}

}  // namespace

int main() {
    mkdir(kOut, 0777);
    const std::string path = std::string(kOut) + "/results.txt";
    g_report = std::fopen(path.c_str(), "wb");
    report("pokopia-ps5 P1: %d lifted programs on the runtime", kCount);
    notify("Pokopia P1 starting");
    const int status = run();
    if (g_report) std::fclose(g_report);
    return status;
}

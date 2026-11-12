// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia (Switch 2, 0400A3A01EE4C000, 2.0.1) as a PS5 title: game-build/src/pkp_main.cpp's boot on the console.
//
// The steps are pkp_main.cpp's, in its order, with what Horizon's kernel and loader do before the first guest instruction:
//   1. map the four NSOs of the ExeFS (rtld, main, sdk, subsdk0) at the bases the lifter baked into the generated code,
//   2. hand the lifted module tables to the runtime,
//   3. let the game's own rtld relocate them ([boot] link = rtld, the default) after the host protects them,
//   4. turn this thread into the guest main thread,
//   5. enter rtld at offset 0 with x0 = 0 and x1 = the main thread's handle.
//
// What is the console's: the game folder is the title's own (/app0, /data/homebrew/<title> over FTP) with exefs/ and romfs/
// copied there from the player's dump, and the output goes to /app0/pkp-results (a ShadowMount title cannot write /data).
// P2 (ps5/PLAN.md) runs with [boot] gfx = none: nvnBootstrapLoader and the NVN entry points are the runtime's logging stubs
// and no Vulkan device is made, so the boot shows how far the game gets before its first frame without graphics on the
// console. [boot] max_seconds (0 here: no limit) ends the run after that many seconds with the runtime's summary in the log.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/gfx.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/nvn.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/nxmodule.h"
#include "nxrt/platform.h"
#include "nxrt/play.h"
#include "nxrt/runtime.h"

#include "pkp/pkp_game.h"
#include "title_screen.h"

namespace ps5compat { void crumb_dump(); void fault_uninstall(); }   // compat/win32_core.cpp, win32_fault.cpp

extern "C" {
extern const NxModule nxmod_main, nxmod_sdk, nxmod_subsdk0, nxmod_rtld;
extern const uint32_t nxmod_main_abi, nxmod_sdk_abi, nxmod_subsdk0_abi, nxmod_rtld_abi;
int sceKernelSendNotificationRequest(int device, void* request, size_t size, int blocking);
}

namespace {

constexpr const char* kGame = "/app0";
constexpr const char* kOut = "/app0/pkp-results";

struct GameModule {
    const char* file;
    const NxModule* nx;
    const uint32_t* abi;
    uint64_t base;
};
// Load order = the order the console maps them: the entry module first.
const GameModule kModules[] = {
    {"rtld", &nxmod_rtld, &nxmod_rtld_abi, 0x7700000000ull},
    {"main", &nxmod_main, &nxmod_main_abi, 0x7100000000ull},
    {"sdk", &nxmod_sdk, &nxmod_sdk_abi, 0x7500000000ull},
    {"subsdk0", &nxmod_subsdk0, &nxmod_subsdk0_abi, 0x7600000000ull},
};

std::FILE* g_stage = nullptr;
std::unique_ptr<nxrt::gfx::Device> g_gfx_device;
std::unique_ptr<nxrt::gfx::Renderer> g_gfx_renderer;

// One line per boot step in pkp-results/boot.txt, written before the step runs: a file that stops says where.
void stage(const char* format, ...) {
    va_list a;
    va_start(a, format);
    if (g_stage) {
        std::vfprintf(g_stage, format, a);
        std::fputc('\n', g_stage);
        std::fflush(g_stage);
        fsync(fileno(g_stage));
    }
    va_end(a);
}

void notify(const char* text) {
    struct {
        char padding[45];
        char message[3075];
    } request{};
    std::snprintf(request.message, sizeof request.message, "%s", text);
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

[[noreturn]] void die(const char* format, const char* a = "", const char* b = "") {
    NXLOG_E("boot", format, a, b);
    nxrt::log_flush();
    char text[512];
    std::snprintf(text, sizeof text, format, a, b);
    stage("FAIL %s", text);
    notify(text);
    ps5compat::fault_uninstall();   // no crash reports from the teardown
    std::_Exit(1);
}

bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Bring-up: memory every 5 s into boot.txt (the title heap, direct and flexible memory left), to tell an exhausted pool
// from the rest when the driver answers VK_ERROR_OUT_OF_HOST_MEMORY.
}  // namespace
extern "C" const char* nxrt_render_where();
bool pkp_ps5_pad_init();   // game/pad_ps5.cpp
extern "C" int sceSystemServiceHideSplashScreen(void);
#ifdef PKP_PS5_BRINGUP
void pkp_ps5_start_trap();   // game/write_trap.cpp
#endif
extern "C" const char* SDL_GetError(void);   // runtime/src/gfx/vk_renderer.cpp
namespace {
void memory_monitor() {
    for (int t = 5;; t += 5) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        struct ps5_heap_stats hs{};
        ps5_heap_stats(&hs);
        int64_t dstart = 0;
        size_t dsize = 0, flex = 0;
        sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x10000, &dstart, &dsize);
        sceKernelAvailableFlexibleMemorySize(&flex);
        ps5compat::crumb_dump();
        stage("MEM %3d s: heap mapped %zu MiB (peak %zu, %u arenas, libc fallbacks %llu), direct free (largest run) %zu MiB of %lld, flexible free %zu MiB; presenting thread in %s",
              t, hs.mapped_bytes >> 20, hs.peak_bytes >> 20, hs.arenas, hs.libc_fallbacks, dsize >> 20,
              static_cast<long long>(sceKernelGetDirectMemorySize() >> 20), flex >> 20, nxrt_render_where());
    }
}

// Bring-up: nn::fs mount and unmount calls into the log (the game aborted in nn::fs::Unmount with 2-6905, a name that was
// never mounted). Each wrapped export logs its name argument and result around the lifted original.
struct Traced {
    const char* sym;
    nxrt::GuestFn orig;
};
Traced g_traced[96];
int g_traced_count = 0;

void guest_string(uint64_t a, char* out, size_t cap) {
    out[0] = 0;
    if (a < 0x7000000000ull || a >= 0x8000000000ull) return;   // not a guest pointer (an id argument)
    size_t i = 0;
    for (; i + 1 < cap; ++i) {
        const char ch = *reinterpret_cast<const char*>(a + i);
        if (!ch) break;
        out[i] = ch;
    }
    out[i] = 0;
}

template <int N>
void traced(::Ctx* c) {
    char name[64];
    guest_string(c->x[0], name, sizeof name);
    NXLOG_I("fstrace", "%s(\"%s\")", g_traced[N].sym, name);
    g_traced[N].orig(c);
    NXLOG_I("fstrace", "%s(\"%s\") -> 0x%x", g_traced[N].sym, name, static_cast<uint32_t>(c->x[0]));
}

template <int... I>
constexpr std::array<nxrt::GuestFn, sizeof...(I)> traced_table(std::integer_sequence<int, I...>) {
    return {&traced<I>...};
}

#ifdef PKP_PS5_BRINGUP
// Bring-up: the main loop's job lookup (f_7101fdcde0) and run (f_7101fe00a0), wrapped at link time (ps5/CMakeLists.txt) and
// logged when the main loop or its job call makes them (the loop ended after one frame on a run that returned 0).
extern "C" void __real_f_7101fdcde0(::Ctx* c);
extern "C" void __real_f_7101fe00a0(::Ctx* c);
extern "C" void __wrap_f_7101fdcde0(::Ctx* c) {
    const uint64_t lr = c->x[30], a0 = c->x[0], a1 = c->x[1];
    __real_f_7101fdcde0(c);
    if (lr == 0x7100882d88ull || lr == 0x7100882db4ull) {
        static std::atomic<int> n{0};
        if (n.fetch_add(1) < 40) NXLOG_I("jobtrace", "lookup(%llx, hash %llx) from %llx -> %llx", (unsigned long long)a0, (unsigned long long)(a1 & 0xffffffff),
                                         (unsigned long long)lr, (unsigned long long)c->x[0]);
    }
}
extern "C" void __wrap_f_7101fe00a0(::Ctx* c) {
    const uint64_t lr = c->x[30], a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3];
    __real_f_7101fe00a0(c);
    if (lr == 0x7101fdc1ccull) {
        static std::atomic<int> n{0};
        if (n.fetch_add(1) < 80 || c->x[0] == 0)
            NXLOG_I("jobtrace", "run(%llx, %llx, %llx, %llx) -> %llx", (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2,
                    (unsigned long long)a3, (unsigned long long)c->x[0]);
    }
}
#endif

// PKP_SAMPLE=start_ms,stop_ms (pkp-env.txt): a host thread reads the main guest thread's current function (Ctx::fn) every 20 us in
// that window and logs each change (with lr): a rough call trace of the main thread without a re-lift.
::Ctx* g_main_ctx = nullptr;
void sample_main(int start_ms, int stop_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(start_ms));
    const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(stop_ms - start_ms);
    uint64_t last = 0;
    int lines = 0;
    while (std::chrono::steady_clock::now() < stop && lines < 20000) {
        if (::Ctx* c = g_main_ctx) {
            const uint64_t fn = __atomic_load_n(&c->fn, __ATOMIC_RELAXED);
            if (fn != last) {
                NXLOG_I("sample", "main fn %llx lr %llx x0 %llx", (unsigned long long)fn, (unsigned long long)c->x[30], (unsigned long long)c->x[0]);
                last = fn;
                ++lines;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
}

// PKP_WATCH=addr[,addr...] (pkp-env.txt): a host thread polls those guest 64-bit words every 200 us and logs every change
// with its time: answers "when, and to what, did the game set this" without a debugger (the main loop's exit flags).
void watch_words(std::vector<uint64_t> addrs) {
    std::vector<uint64_t> last(addrs.size(), ~0ull);
    // Not before the guest heap holds them: a read of reserved memory faults (the first try, at 1 s, did).
    const char* delay = getenv("PKP_WATCH_AFTER_MS");
    std::this_thread::sleep_for(std::chrono::milliseconds(delay ? std::atoi(delay) : 5000));
    for (;;) {
        for (size_t i = 0; i < addrs.size(); ++i) {
            const uint64_t a = addrs[i];
            if (a < 0x7000000000ull || a >= 0x8000000000ull) continue;
            if (nxrt::kernel::current_ctx() == nullptr && false) break;
            const uint64_t v = __atomic_load_n(reinterpret_cast<const uint64_t*>(a), __ATOMIC_RELAXED);
            if (v != last[i]) {
                NXLOG_I("watch", "%llx: %llx -> %llx", (unsigned long long)a, (unsigned long long)last[i], (unsigned long long)v);
                if (v == 0 && last[i] != ~0ull && last[i] != 0) {   // cleared: where every guest thread is right now
                    for (const auto& t : nxrt::kernel::threads())
                        if (t) NXLOG_I("watch", "  %s fn %llx lr %llx x0 %llx", t->name.c_str(), (unsigned long long)t->ctx.fn,
                                       (unsigned long long)t->ctx.x[30], (unsigned long long)t->ctx.x[0]);
                }
                last[i] = v;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void start_watch() {
    const char* w = getenv("PKP_WATCH");
    if (!w || !*w) return;
    std::vector<uint64_t> addrs;
    for (const char* p = w; *p;) {
        char* end = nullptr;
        const uint64_t a = std::strtoull(p, &end, 0);
        if (end == p) break;
        addrs.push_back(a);
        p = *end ? end + 1 : end;
    }
    NXLOG_I("watch", "watching %zu guest words", addrs.size());
    std::thread(watch_words, addrs).detach();
}

// The game's main loop (main 0x7100882c60): runs while the flags at x0+0x1e8 have none of bits 21-23 and x0+0xf0 holds a
// pointer; the first P3 runs left it at once. Logs both on the way in and out.
nxrt::GuestFn g_loop_orig = nullptr;
void traced_loop(::Ctx* c) {
    const uint64_t self = c->x[0];
    auto rd = [](uint64_t a) -> uint64_t { return (a >= 0x7000000000ull && a < 0x8000000000ull) ? *reinterpret_cast<const uint64_t*>(a) : 0; };
    NXLOG_I("looptrace", "main loop enter: this %llx flags %llx [+0xf0] %llx", (unsigned long long)self, (unsigned long long)rd(self + 0x1e8),
            (unsigned long long)rd(self + 0xf0));
    g_loop_orig(c);
    NXLOG_I("looptrace", "main loop left: flags %llx [+0xf0] %llx", (unsigned long long)rd(self + 0x1e8), (unsigned long long)rd(self + 0xf0));
}

// Bring-up: strcmp from the name lookup that misses before the game leaves its loop (main 0x71017a9c80 and its callers
// around 0x71017bc900): both strings and the answer.
const auto g_boot_time = std::chrono::steady_clock::now();   // static init: the title's start
nxrt::GuestFn g_strcmp_orig = nullptr;
void traced_strcmp(::Ctx* c) {
    const uint64_t lr = c->x[30], a = c->x[0], b = c->x[1];
    g_strcmp_orig(c);
    const auto t0 = g_boot_time;
    static const int after_ms = getenv("PKP_STRTRACE_AFTER_MS") ? std::atoi(getenv("PKP_STRTRACE_AFTER_MS")) : 0;
    if (lr >= 0x71017a9c80ull && lr < 0x71017bd000ull &&
        std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(after_ms)) {
        static std::atomic<int> n{0};
        if (n.fetch_add(1) < 400) {
            char sa[96], sb[96];
            guest_string(a, sa, sizeof sa);
            guest_string(b, sb, sizeof sb);
            NXLOG_I("strtrace", "strcmp(\"%s\", \"%s\") from %llx -> %d", sa, sb, (unsigned long long)lr, static_cast<int>(c->x[0]));
        }
    }
}

void trace_fs_mounts() {
    static constexpr auto table = traced_table(std::make_integer_sequence<int, 96>{});
    static const char* const kSyms[] = {
        "_ZN2nn2fs7UnmountEPKc", "_ZN2nn2fs8MountRomEPKcPvm", "_ZN2nn2fs13MountSaveDataEPKcNS0_6UserIdE",
        "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", "_ZN2nn2fs13MountSaveDataEPKcNS_13ApplicationIdERKNS_7account3UidE",
        "_ZN2nn2fs13MountSaveDataEPKcNS_3ncm13ApplicationIdENS0_6UserIdE", "_ZN2nn2fs17MountCacheStorageEPKc",
        "_ZN2nn2fs17MountCacheStorageEPKci", "_ZN2nn2fs19MountDeviceSaveDataEPKc", "_ZN2nn2fs17MountBcatSaveDataEPKcNS_3ncm13ApplicationIdE",
        "_ZN2nn2fs17MountAddOnContentEPKciPvm", "_ZN2nn2fs15MountSystemDataEPKcNS_3ncm12SystemDataIdE",
        "_ZN2nn2fs15MountSystemDataEPKcNS_3ncm12SystemDataIdEPvm", "_ZN2nn2fs11MountSdCardEPKc", "_ZN2nn2fs12MountRamDiskEPKcPvm",
        "_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE",
        "_ZN2nn2fs20MountTemporaryStorageEPKc", "_ZN2nn4bcat24MountDeliveryCacheStorageEv",
        "_ZN2nn4bcat24MountDeliveryCacheStorageENS_13ApplicationIdE",
    };
    for (const char* sym : kSyms) {
        const uint64_t a = nxrt::nxmodule_find_export(sym);
        if (!a || g_traced_count >= 96) continue;
        nxrt::GuestFn orig = nxrt::fn_lookup(a);
        if (!orig) continue;
        g_traced[g_traced_count] = {sym, orig};
        nxrt::fn_register(a, table[g_traced_count]);
        ++g_traced_count;
    }
    NXLOG_I("fstrace", "%d nn::fs mount functions traced", g_traced_count);
    if ((g_loop_orig = nxrt::fn_lookup(0x7100882c60ull))) nxrt::fn_register(0x7100882c60ull, traced_loop);
    if (const uint64_t sc = nxrt::nxmodule_find_export("strcmp"))
        if ((g_strcmp_orig = nxrt::fn_lookup(sc))) nxrt::fn_register(sc, traced_strcmp);
}

// [boot] max_seconds: the summary of what ran (svc, ipc, stubs, modules, threads, NVN calls) goes to the log, then the run ends.
void deadline(int seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    NXLOG_W("boot", "max_seconds reached: the guest is still running; ending the run");
    NXLOG_I("boot", "state at the deadline:\n%s", nxrt::summary().c_str());
    if (nxrt::nvn::backend_installed()) {
        const nxrt::nvn::BackendStats b = nxrt::nvn::backend_stats();
        stage("nvn: submits %llu draws %llu (skipped %llu) presents %llu pipelines %llu textures %llu (failed %llu)",
              (unsigned long long)b.submits, (unsigned long long)b.draws, (unsigned long long)b.draws_skipped,
              (unsigned long long)b.presents, (unsigned long long)b.pipelines_created, (unsigned long long)b.textures_created,
              (unsigned long long)b.textures_failed);
        NXLOG_I("boot", "nvn unsupported operations: %s", nxrt::nvn::backend_unsupported_report().c_str());
    }
    nxrt::log_flush();
    stage("DEADLINE %d s: the guest was still running (state in pkp.log)", seconds);
    char text[96];
    std::snprintf(text, sizeof text, "Pokopia: ran %d s, see pkp-results", seconds);
    notify(text);
    ps5compat::fault_uninstall();
    std::_Exit(124);
}

}  // namespace

int main() {
    nxrt_register_game_pkp();   // before anything reads it (nxrt::init, play mode, the services): docs/GAME_BOUNDARY.md
    mkdir(kOut, 0777);
    // RADV and Mesa report on standard error (why vkCreateInstance fails, for one): into klog, as PS5_Vulkan's radv_smoke does.
    // Bring-up without etaHEN's klog server: RADV's stderr goes to a file in the results folder instead.
    // (PKP_STDERR_FILE; unbuffered writes to /app0 cost about 50 ms a line, so the default stays klog.)
    if (!getenv("PKP_STDERR_FILE")) ps5_klog_capture_stderr("[pkp:stderr] ");
    else if (std::freopen((std::string(kOut) + "/stderr.txt").c_str(), "w", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
    g_stage = std::fopen((std::string(kOut) + "/boot.txt").c_str(), "wb");
    stage("pokopia-ps5 P3: the 2.0.1 game on the runtime, Vulkan through RADV");
    notify("Pokopia (P2) starting");
    // The title's sandbox refuses chdir into /app0 (first console run: "cannot enter the game folder"; Monado-PS5 only enters
    // /download0). So nothing relies on the working directory: every path the runtime would take relative to the game folder
    // gets /app0 here, unless pkp.ini names it.
    stage("image: stage() at %p (link address of stage in the ELF gives the load bias)", reinterpret_cast<void*>(&stage));
    stage("chdir %s: %s", kGame, chdir(kGame) == 0 ? "ok" : "refused (absolute paths are used)");

    // Console defaults, as NXRT_* variables (the config reads the environment first, then the ini): set before nxrt::init,
    // because the services take some of them during init (fsp-srv logged RomFS 'game/romfs' on the second run when they
    // were set after it). A variable already set wins (setenv with overwrite 0); being variables, these also win over
    // /app0/pkp.ini, which sets every other key.
    const struct { const char* env; const char* path; } kPaths[] = {
        {"NXRT_FS_ROMFS", "/romfs"},
        {"NXRT_FS_EXEFS", "/exefs"},
        // Saves: Windows puts them under %APPDATA%\<save_folder>\save (services/common.cpp save_dir); the console has no
        // APPDATA, and the default would be a relative path with backslashes.
        {"NXRT_FS_SAVE_DIR", "/save"},
        {"NXRT_PLAY_CONTROLS", "/controls.ini"},
        {"NXRT_NVN_PIPELINE_CACHE", "/pipeline_cache.bin"},
        {"NXRT_NVN_PIPELINE_KEYS", "/pipeline_keys.bin"},
        {"NXRT_NVN_SHADER_ORDER", "/shader_order.txt"},
        // Translated shaders (<hash>.spv + results.tsv), made on a PC from the player's own romfs (pokopia-recomp's
        // extract_corpus.py and spirv_harness) and uploaded beside the game data.
        {"NXRT_NVN_SHADER_CACHE", "/shader_cache/spirv"},
        // Frame traces on demand: uploading a file named pkp-trace traces the next frame into the log, with a screenshot and the
        // render targets as PNGs in pkp-results/trace/ (runtime/docs/CONFIG.md [nvn] trace_trigger).
        {"NXRT_NVN_TRACE_TRIGGER", "/pkp-trace"},
        {"NXRT_NVN_TRACE_DIR", "/pkp-results/trace/"},
    };
    for (const auto& p : kPaths) setenv(p.env, (std::string(kGame) + p.path).c_str(), 0);
    // P3: the NVN back end on RADV, in a window that is the console's video output (SDL3's ps5 driver: VK_KHR_display).
    // [boot] gfx = none keeps P2's logging stubs.
    setenv("NXRT_BOOT_GFX", "window", 0);
    setenv("NXRT_GFX_VALIDATION", "0", 0);   // no validation layer on the console
    // NXRT_IPC_LOG_FAILURES=1 (pkp-env.txt) logs every failing service command.
    setenv("SDL_VIDEO_DRIVER", "ps5", 0);
    setenv("SDL_VIDEODRIVER", "ps5", 0);
    // Mesa's shader disk cache: a writable folder (the default is under $HOME, which the console does not set).
    setenv("MESA_SHADER_CACHE_DIR", (std::string(kGame) + "/mesa_cache").c_str(), 0);
    // Bring-up: no Mesa disk cache (the fifth P3 run ended, no signal, at RADV's first compile of a game shader)
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);
    // /app0/pkp-env.txt: NAME=VALUE lines set as environment variables before anything reads them (any NXRT_* config key,
    // SDL or Mesa variables), so a run can be configured over FTP without a rebuild. Its values replace the defaults above.
    if (std::FILE* env = std::fopen((std::string(kGame) + "/pkp-env.txt").c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, env)) {
            line[std::strcspn(line, "\r\n")] = 0;
            char* eq = std::strchr(line, '=');
            if (!eq || line[0] == '#') continue;
            *eq = 0;
            setenv(line, eq + 1, 1);   // the file is the player's override: it wins over the console defaults above
            stage("env %s=%s (pkp-env.txt)", line, eq + 1);
        }
        std::fclose(env);
    }
    // Audio: the device holds the game's queue plus this margin (runtime default 30 ms). Loading stalls the game's audio thread
    // for longer than that (about 20 underruns in the first 30 s of a console run at 30 ms); 80 ms covers them at little cost.
    setenv("NXRT_AUDIO_OUT_LATENCY_MS", "80", 0);
    // The game's shaders come from NVIDIA's 32-wide warps (shuffles, ballots, votes). The runtime asks for 32 per pipeline
    // stage where Vulkan allows it (nvn_pipeline.cpp subgroup32_info); RADV lets compute ask but runs pixel and vertex
    // shaders 64 wide unless told otherwise. Read by RADV when the Vulkan instance is made, so set before the renderer starts.
    setenv("RADV_PERFTEST", "pswave32,gewave32,cswave32", 0);
    // Bring-up switches, as files in the title folder (uploaded by FTP, no rebuild): /app0/pkp-debug (debug-level log) and
    // /app0/pkp-breadcrumbs (one line per compat kernel call, svc and HLE call in pkp-results/calls.txt). Both slow the game.
    struct stat sw;
    const bool debug_log = getenv("PKP_DEBUG") || stat((std::string(kGame) + "/pkp-debug").c_str(), &sw) == 0;
    const bool crumbs = getenv("PKP_BREADCRUMBS") || stat((std::string(kGame) + "/pkp-breadcrumbs").c_str(), &sw) == 0;
    setenv("NXRT_LOG_LEVEL", debug_log ? "debug" : "info", 0);         // debug-level lines, each written to /app0, slowed the whole game down        // bring-up: the loader's map and protect steps
    // Bring-up: the compat layer's kernel calls, one line each (ps5/compat win32_internal.h crumb). The console ended the fourth
    // run with SYSTEM_ILLEGAL_FUNCTION_CALL and no signal; this file's last line names the call it refused.
    // Off by default since P3: one write() to /app0 per call slowed every guest thread to a crawl (calls fell from ~320/s
    // to ~3/s over two minutes). PKP_BREADCRUMBS=1 turns them back on.
    if (crumbs) setenv("NXRT_PS5_BREADCRUMBS", (std::string(kOut) + "/calls.txt").c_str(), 0);

    // /app0/pkp.ini, when there is one, sets anything else.
    const std::string ini = std::string(kGame) + "/pkp.ini";
    nxrt::InitOptions opt;
    const std::string log = std::string(kOut) + "/pkp.log";
    opt.ini_path = file_exists(ini) ? ini.c_str() : "";
    opt.log_path = log.c_str();
    opt.load_symbols = false;
    stage("STAGE runtime init");
    if (!nxrt::init(opt)) die("runtime start-up failed (address-space layout)");
    const std::string exefs = nxrt::config_get_str("fs", "exefs", "/app0/exefs");
    nxrt::config_set("fs", "exefs", exefs.c_str());
    nxrt::config_set("process", "program_id", "0x0400A3A01EE4C000");
    NXLOG_I("boot", "Pokémon Pokopia %s on the PS5: exefs '%s', romfs '%s', graphics '%s'", nxrt::game::info().version, exefs.c_str(),
            nxrt::config_get_str("fs", "romfs", "").c_str(), nxrt::config_get_str("boot", "gfx", "").c_str());

    const bool nvn = nxrt::config_get_bool("boot", "nvn", true);
    if (nvn) nxrt::nvn::install();   // nvnBootstrapLoader and the NVN entry points (logging stubs for what no back end implements)
    // The NVN back end (Vulkan through RADV), as game-build/src/pkp_main.cpp starts it on the PC.
    bool play_started = false;
    const std::string gfxmode = nxrt::config_get_str("boot", "gfx", "window");
    if (nvn && gfxmode != "none") {
        const bool headless = gfxmode != "window";
        nxrt::play::DisplayConfig disp;
        if (!headless) {
            stage("STAGE window");
            disp = nxrt::play::display();
            if (!nxrt::platform::init()) die("the platform layer (SDL3) did not start (see pkp.log)");
            // The DualSense as an SDL gamepad (game/pad_ps5.cpp): without one the game finds no controller and leaves its loop.
            if (pkp_ps5_pad_init()) NXLOG_I("boot", "DualSense attached as an SDL gamepad");
            else NXLOG_W("boot", "no DualSense: %s", SDL_GetError());
            nxrt::platform::WindowDesc wd;
            wd.title = nxrt::game::info().title;
            wd.width = disp.width;
            wd.height = disp.height;
            if (!nxrt::platform::create_window(wd)) die("cannot create the game window (see pkp.log)");
            // The system keeps its launch splash up until the title says otherwise (KongaTime's ports do this at start); system
            // dialogs (the keyboard) wait behind it: sceImeDialogInit never returned without it.
            NXLOG_I("boot", "sceSystemServiceHideSplashScreen -> 0x%08x", static_cast<unsigned>(sceSystemServiceHideSplashScreen()));
        }
        stage("STAGE vulkan device (%s)", gfxmode.c_str());
        nxrt::gfx::DeviceDesc dd;
        dd.headless = headless ? 1 : 0;
        std::string err;
        g_gfx_device = nxrt::gfx::Device::create(dd, &err);
        if (!g_gfx_device) die("graphics device: %s", err.c_str());
        stage("STAGE renderer on %s", g_gfx_device->gpu().name.c_str());
        nxrt::gfx::RendererDesc rd;
        rd.headless_width = 1280;
        rd.headless_height = 720;
        if (!headless) rd.present_mode = disp.present_mode;
        g_gfx_renderer = nxrt::gfx::Renderer::create(*g_gfx_device, rd, &err);
        if (!g_gfx_renderer) die("renderer: %s", err.c_str());
        // The start screen (game/title_screen.cpp): Prepare game the first time, then Play. PKP_TITLE=0 (pkp-env.txt) goes straight in.
        if (!headless && !(getenv("PKP_TITLE") && std::strcmp(getenv("PKP_TITLE"), "0") == 0)) {
            stage("STAGE start screen");
            if (!pkp_ps5::title_screen_run(*g_gfx_device, *g_gfx_renderer, kGame)) {
                stage("EXIT from the start screen");
                g_gfx_renderer.reset();
                g_gfx_device.reset();
                nxrt::shutdown();
                return 0;
            }
        }
        nxrt::nvn::BackendDesc bd;
        bd.device = g_gfx_device.get();
        bd.renderer = g_gfx_renderer.get();
        if (!headless) {
            bd.limiter = nxrt::play::limiter();
            bd.keep_aspect = disp.keep_aspect;
        }
        if (!nxrt::nvn::install_backend(bd)) die("the NVN back end failed to start (see pkp.log)");
        stage("STAGE nvn back end running");
        NXLOG_I("boot", "NVN back end running on %s (%s)", g_gfx_device->gpu().name.c_str(), gfxmode.c_str());
        nxrt::play::StartOptions po;
        po.device = g_gfx_device.get();
        po.renderer = g_gfx_renderer.get();
        po.window = !headless;
        po.overlay = false;
        nxrt::play::start(po);
        play_started = true;
    }
    if (!play_started) {
        nxrt::play::StartOptions po;   // no graphics: play mode still turns scripted keys into pad input
        po.overlay = false;
        nxrt::play::start(po);
    }

    // 1. map, 2. register
    for (const GameModule& g : kModules) {
        const std::string path = exefs + "/" + g.file;
        stage("STAGE map %s", g.file);
        if (!file_exists(path)) die("the game files were not found (%s): copy exefs/ and romfs/ of the 2.0.1 dump into the title folder", path.c_str());
        nxrt::LoadOptions lo;
        lo.name = g.file;
        lo.base = g.base;
        std::string err;
        nxrt::Module* m = nxrt::module_load_nso_file(path.c_str(), lo, &err);
        if (!m) die("cannot load %s: %s", path.c_str(), err.c_str());
        if (m->base() != g.nx->base) die("module %s is mapped at a base different from the one the library was lifted for", g.file);
        const nxrt::RegResult r = nxrt::nxmodule_register(m, g.nx, *g.abi);
        if (!r.ok()) die("lifted module %s refused: %s", g.file, r.message.c_str());
    }

    // NVN is reached only through nvnBootstrapLoader: forced to the runtime's front end (pkp_main.cpp, debug note 120).
    if (nvn) {
        nxrt::module_override_symbol("nvnBootstrapLoader");
        if (const uint64_t def = nxrt::nxmodule_find_export("nvnBootstrapLoader"))
            if (nxrt::HleFn fn = nxrt::hle_lookup("nvnBootstrapLoader")) nxrt::fn_register(def, fn);
    }

    // PKP_FSTRACE=1 (pkp-env.txt): nn::fs mounts, the main loop and strcmp traced (every guest strcmp pays for it, so off by default)
    if (getenv("PKP_FSTRACE")) trace_fs_mounts();
    // Registered after the runtime's own atexit handlers, so it runs before them: the teardown that exit() starts is not
    // caught as crashes (each turned into another exit on the console).
    std::atexit([] { ps5compat::fault_uninstall(); });
    start_watch();
#ifdef PKP_PS5_BRINGUP
    pkp_ps5_start_trap();
#endif

    // 3. link: the game's own rtld relocates ([boot] link = rtld), or the runtime's linker ([boot] link = host)
    stage("STAGE link (%s)", nxrt::config_get_str("boot", "link", "rtld").c_str());
    if (nxrt::config_get_str("boot", "link", "rtld") == "host") {
        if (!nxrt::module_link_all()) die("linking the modules failed (see the log)");
    } else {
        nxrt::module_protect_all();
    }

    const int64_t seconds = nxrt::config_get_int("boot", "max_seconds", 0);
    if (seconds > 0) std::thread(deadline, static_cast<int>(seconds)).detach();
    std::thread(memory_monitor).detach();

    // 4. the main thread: the npdm of main says 1 MiB of stack, priority 44, core 0
    const uint64_t stack = static_cast<uint64_t>(nxrt::config_get_int("boot", "main_stack_kb", 1024)) << 10;
    const uint32_t h = nxrt::kernel::main_thread_adopt(stack, "main");
    if (!h) die("cannot create the main thread");
    ::Ctx* ctx = nxrt::kernel::current_ctx();
    g_main_ctx = ctx;
    if (const char* smp = getenv("PKP_SAMPLE")) {
        int a = 0, b = 0;
        if (std::sscanf(smp, "%d,%d", &a, &b) == 2 && b > a) std::thread(sample_main, a, b).detach();
    }
    if (!nxrt::fn_lookup(kModules[0].base)) die("the lifted rtld has no function at its entry: lift the game again");

    // 5. the first instruction: rtld offset 0
    ctx->x[0] = 0;
    ctx->x[1] = h;
    ctx->x[30] = 0;
    stage("STAGE enter rtld at 0x%llx (the game runs from here; see pkp.log)", static_cast<unsigned long long>(kModules[0].base));
    NXLOG_I("boot", "entering rtld at 0x%llx (x1 = main thread handle 0x%x)", static_cast<unsigned long long>(kModules[0].base), h);
    nxrt::dispatch_call(ctx, kModules[0].base);

    NXLOG_I("boot", "the entry function returned (x0 = 0x%llx): ending the process", static_cast<unsigned long long>(ctx->x[0]));
    stage("RETURNED x0 = 0x%llx", static_cast<unsigned long long>(ctx->x[0]));
    nxrt::play::stop();
    nxrt::kernel::main_thread_release();
    nxrt::shutdown();
    return static_cast<int>(ctx->x[0] & 0xff);
}

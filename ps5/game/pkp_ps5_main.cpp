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
// console. [boot] max_seconds (default 120 here) ends the run with the runtime's summary in the log.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/nvn.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/nxmodule.h"
#include "nxrt/play.h"
#include "nxrt/runtime.h"

#include "pkp/pkp_game.h"

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
    std::_Exit(1);
}

bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// [boot] max_seconds: the summary of what ran (svc, ipc, stubs, modules, threads, NVN calls) goes to the log, then the run ends.
void deadline(int seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    NXLOG_W("boot", "max_seconds reached: the guest is still running; ending the run");
    NXLOG_I("boot", "state at the deadline:\n%s", nxrt::summary().c_str());
    nxrt::log_flush();
    stage("DEADLINE %d s: the guest was still running (state in pkp.log)", seconds);
    char text[96];
    std::snprintf(text, sizeof text, "Pokopia: ran %d s, see pkp-results", seconds);
    notify(text);
    std::_Exit(124);
}

}  // namespace

int main() {
    nxrt_register_game_pkp();   // before anything reads it (nxrt::init, play mode, the services): docs/GAME_BOUNDARY.md
    mkdir(kOut, 0777);
    g_stage = std::fopen((std::string(kOut) + "/boot.txt").c_str(), "wb");
    stage("pokopia-ps5 P2: the 2.0.1 game on the runtime, graphics off");
    notify("Pokopia (P2) starting");
    // The title's sandbox refuses chdir into /app0 (first console run: "cannot enter the game folder"; Monado-PS5 only enters
    // /download0). So nothing relies on the working directory: every path the runtime would take relative to the game folder
    // gets /app0 here, unless pkp.ini names it.
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
    };
    for (const auto& p : kPaths) setenv(p.env, (std::string(kGame) + p.path).c_str(), 0);
    setenv("NXRT_BOOT_GFX", "none", 0);          // P2: no Vulkan device, NVN logging stubs
    setenv("NXRT_LOG_LEVEL", "debug", 0);        // bring-up: the loader's map and protect steps
    // Bring-up: the compat layer's kernel calls, one line each (ps5/compat win32_internal.h crumb). The console ended the fourth
    // run with SYSTEM_ILLEGAL_FUNCTION_CALL and no signal; this file's last line names the call it refused.
    setenv("NXRT_PS5_BREADCRUMBS", (std::string(kOut) + "/calls.txt").c_str(), 0);

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
    if (nvn) nxrt::nvn::install();   // nvnBootstrapLoader and the NVN entry points (logging stubs: no back end in P2)
    {
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

    // 3. link: the game's own rtld relocates ([boot] link = rtld), or the runtime's linker ([boot] link = host)
    stage("STAGE link (%s)", nxrt::config_get_str("boot", "link", "rtld").c_str());
    if (nxrt::config_get_str("boot", "link", "rtld") == "host") {
        if (!nxrt::module_link_all()) die("linking the modules failed (see the log)");
    } else {
        nxrt::module_protect_all();
    }

    const int64_t seconds = nxrt::config_get_int("boot", "max_seconds", 120);
    if (seconds > 0) std::thread(deadline, static_cast<int>(seconds)).detach();

    // 4. the main thread: the npdm of main says 1 MiB of stack, priority 44, core 0
    const uint64_t stack = static_cast<uint64_t>(nxrt::config_get_int("boot", "main_stack_kb", 1024)) << 10;
    const uint32_t h = nxrt::kernel::main_thread_adopt(stack, "main");
    if (!h) die("cannot create the main thread");
    ::Ctx* ctx = nxrt::kernel::current_ctx();
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

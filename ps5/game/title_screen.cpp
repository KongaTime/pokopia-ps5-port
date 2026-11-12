// SPDX-License-Identifier: GPL-3.0-or-later
// The title's start screen (title_screen.h). Drawn with Dear ImGui on the renderer's overlay hook, before the NVN back end owns the
// renderer. Prepare game runs on a worker thread:
//   1. the game files: exefs/rtld, main, sdk, subsdk0 and romfs/, and that main is the 2.0.1a build the title was lifted from;
//   2. the shader cache: results.tsv and every <hash>.spv it lists (an upload that stopped part way shows here as missing files);
//   3. the folders the game writes (save/), then pkp-prepared.txt, which later starts skip straight to Play.
// Pictures are the player's own, never shipped: /app0/title.png is the background (cover-fitted; otherwise a drawn gradient) and
// /app0/logo.png goes where the name is (otherwise the name in letters; with a title.png and no logo.png the picture's own name
// is used and none is drawn). The fonts are built in (ps5/assets/fonts, SIL OFL 1.1).
#include "title_screen.h"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "nxrt/config.h"
#include "nxrt/gfx.h"
#include "nxrt/input.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"

// The start screen's fonts, built into the title (PKP_FONT_DIR: ps5/assets/fonts, set by ps5/CMakeLists.txt).
#define PKP_EMBED(sym, file)                                                                                                        \
    asm(".section .rodata\n.balign 16\n.global " #sym "\n" #sym ":\n.incbin \"" PKP_FONT_DIR "/" file "\"\n.global " #sym       \
        "_end\n" #sym "_end:\n.previous\n");                                                                                       \
    extern "C" const unsigned char sym[], sym##_end[];
PKP_EMBED(pkp_font_menu, "Montserrat-Medium.ttf")
PKP_EMBED(pkp_font_text, "Inter-SemiBold.ttf")
PKP_EMBED(pkp_font_serif, "KaTeX_Main-Italic.ttf")

namespace pkp_ps5 {

namespace {

constexpr const char* kTag = "title";
constexpr const char* kMainBuild = "76A489F3BD3642FA5E18BD83D32C98BACC5F7D0E";   // exefs/main of 2.0.1a (ps5/setup.sh checks the same)
constexpr int kPrepareVersion = 1;                                               // bump when Prepare does more: every console prepares again

using Clock = std::chrono::steady_clock;

// ---- Prepare (worker thread) -------------------------------------------------------------------------------------------------

struct Prepare {
    std::atomic<float> progress{0.f};            // 0..1 over all steps
    std::atomic<bool> done{false};
    std::mutex mu;                               // guards the strings below
    std::string step;                            // what it is doing now
    std::string error;                           // set when the game cannot start (missing or wrong game files)
    std::string warning;                         // set when it can, with something missing (shaders)
    std::thread worker;

    void set_step(const char* s) { std::lock_guard lk(mu); step = s; }
};

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// The build id of an NSO (exefs/main): 0x14 bytes at 0x40 of its header, as upper-case hex. Empty if unreadable.
std::string nso_build_id(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    unsigned char h[0x60];
    const size_t n = std::fread(h, 1, sizeof h, f);
    std::fclose(f);
    if (n != sizeof h) return {};
    char out[41];
    for (int i = 0; i < 20; ++i) std::snprintf(out + i * 2, 3, "%02X", h[0x40 + i]);
    return out;
}

bool dir_has_entries(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    bool any = false;
    while (dirent* e = readdir(d))
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) { any = true; break; }
    closedir(d);
    return any;
}

std::string marker_path(const std::string& game) { return game + "/pkp-prepared.txt"; }

// The marker's first line: what a finished Prepare wrote. A start with the same line (and the same main) goes straight to Play.
std::string marker_line(const std::string& build) { return "prepared " + std::to_string(kPrepareVersion) + " " + build; }

// Read once at start: true when Prepare already passed for these game files. *warning gets the shader note it left.
bool already_prepared(const std::string& game, std::string* warning) {
    std::FILE* f = std::fopen(marker_path(game).c_str(), "r");
    if (!f) return false;
    char line[256] = {}, note[512] = {};
    const bool got = std::fgets(line, sizeof line, f) != nullptr;
    if (std::fgets(note, sizeof note, f)) {
        note[std::strcspn(note, "\r\n")] = 0;
        if (std::strncmp(note, "warning ", 8) == 0) *warning = note + 8;
    }
    std::fclose(f);
    if (!got) return false;
    line[std::strcspn(line, "\r\n")] = 0;
    const std::string build = nso_build_id(game + "/exefs/main");
    return !build.empty() && marker_line(build) == line;
}

void prepare_run(Prepare* p, std::string game) {
    auto fail = [&](std::string msg) {
        NXLOG_W(kTag, "prepare: %s", msg.c_str());
        std::lock_guard lk(p->mu);
        p->error = std::move(msg);
    };
    // 1. the game files (5 %)
    p->set_step("Checking the game files");
    const std::string exefs = nxrt::config_get_str("fs", "exefs", (game + "/exefs").c_str());
    const std::string romfs = nxrt::config_get_str("fs", "romfs", (game + "/romfs").c_str());
    for (const char* m : {"rtld", "main", "sdk", "subsdk0"})
        if (!exists(exefs + "/" + m)) {
            fail("The game files were not found (no " + exefs + "/" + m + ").\nCopy exefs/ and romfs/ from your 2.0.1a dump into " + game + ".");
            p->done = true;
            return;
        }
    const std::string build = nso_build_id(exefs + "/main");
    if (build != kMainBuild) {
        fail("These game files are not version 2.0.1a (exefs/main is build " + build.substr(0, 16) + "...).\nOnly 2.0.1a is supported.");
        p->done = true;
        return;
    }
    if (!dir_has_entries(romfs)) {
        fail("The game's romfs/ folder was not found or is empty (" + romfs + ").\nCopy romfs/ from your 2.0.1a dump into " + game + ".");
        p->done = true;
        return;
    }
    p->progress = 0.05f;

    // 2. the shader cache (5 % .. 95 %): every record of results.tsv has its <hash>.spv
    p->set_step("Checking the shaders");
    const std::string sdir = nxrt::config_get_str("nvn", "shader_cache", (game + "/shader_cache/spirv").c_str());
    std::string warning;
    std::vector<std::string> hashes;
    if (std::FILE* f = std::fopen((sdir + "/results.tsv").c_str(), "r")) {   // 5 % .. 20 %: reading the list
        struct stat st;
        const double size = stat((sdir + "/results.tsv").c_str(), &st) == 0 && st.st_size > 0 ? static_cast<double>(st.st_size) : 1.0;
        double got = 0;
        char line[4096];
        bool header = true;
        while (std::fgets(line, sizeof line, f)) {
            got += static_cast<double>(std::strlen(line));
            if ((hashes.size() & 1023) == 0) p->progress = 0.05f + 0.15f * static_cast<float>(std::min(1.0, got / size));
            if (header) { header = false; continue; }
            const size_t n = std::strcspn(line, "\t\r\n");
            if (n) hashes.emplace_back(line, n);
        }
        std::fclose(f);
    }
    if (hashes.empty()) {
        warning = "No shader cache was found, so the game will run without graphics for most things.\nUpload it to " + sdir + ".";
    } else {
        size_t missing = 0;
        for (size_t i = 0; i < hashes.size(); ++i) {
            if (!exists(sdir + "/" + hashes[i] + ".spv")) ++missing;
            if ((i & 255) == 0) p->progress = 0.20f + 0.75f * static_cast<float>(i) / static_cast<float>(hashes.size());
        }
        NXLOG_I(kTag, "prepare: %zu shaders listed, %zu missing", hashes.size(), missing);
        if (missing)
            warning = std::to_string(missing) + " of " + std::to_string(hashes.size()) +
                      " shaders are missing, so parts of the game won't be drawn.\nThe shader upload may have stopped early: upload it again.";
    }
    p->progress = 0.95f;

    // 3. the folders the game writes, then the marker
    p->set_step("Setting up");
    mkdir(nxrt::config_get_str("fs", "save_dir", (game + "/save").c_str()).c_str(), 0777);
    if (std::FILE* f = std::fopen(marker_path(game).c_str(), "w")) {
        std::fprintf(f, "%s\n", marker_line(build).c_str());
        if (!warning.empty()) {
            std::string one = warning;
            std::replace(one.begin(), one.end(), '\n', ' ');
            std::fprintf(f, "warning %s\n", one.c_str());
        }
        std::fclose(f);
    }
    {
        std::lock_guard lk(p->mu);
        p->warning = warning;
    }
    p->progress = 1.f;
    p->done = true;
}

// ---- the screen ------------------------------------------------------------------------------------------------------------------

enum class State { NeedPrepare, Preparing, Ready, Failed, Leaving };

struct Screen {
    nxrt::gfx::Device* dev = nullptr;
    nxrt::gfx::Renderer* ren = nullptr;
    std::string game;
    ImGuiContext* imgui = nullptr;
    bool backend = false;
    VkFormat format = VK_FORMAT_UNDEFINED;

    struct Picture {
        ImTextureData* tex = nullptr;
        int w = 0, h = 0;
    };
    Picture bg;                                  // title.png, when there is one
    Picture logo;                                // logo.png, when there is one
    ImFont* f_menu = nullptr;                    // Montserrat: the menu and the name
    ImFont* f_text = nullptr;                    // Inter: messages
    ImFont* f_serif = nullptr;                   // KaTeX italic: RECOMP

    State state = State::NeedPrepare;
    int sel = 0;                                 // 0 = the first item (Prepare / Play / Try again), 1 = Exit
    bool play = false;                           // the choice when Leaving
    std::unique_ptr<Prepare> prep;
    std::string warning, error;
    float shown_progress = 0.f;                  // eased towards the worker's value
    Clock::time_point start = Clock::now(), leave_at{}, last = Clock::now();
    float sel_anim = 0.f;                        // the highlight bar's position (eased towards sel)
    int stick_dir = 0;                           // the left stick's last menu step (-1 up, 1 down, 0 centred)

    bool init();
    void shutdown();
    Picture load_picture(const char* file);
    void free_picture(Picture& p);
    void start_prepare();
    void confirm();
    void move(int d);
    void handle_input();
    void update();
    void build(const nxrt::gfx::FrameInfo& f);
    void draw(const nxrt::gfx::FrameInfo& f);
};

bool Screen::init() {
    IMGUI_CHECKVERSION();
    imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefaultVector();
    auto add = [&](const unsigned char* b, const unsigned char* e) {
        ImFontConfig fc;
        fc.FontDataOwnedByAtlas = false;   // the bytes are in the title's .rodata
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(b), static_cast<int>(e - b), 32.f, &fc);
    };
    f_menu = add(pkp_font_menu, pkp_font_menu_end);
    f_text = add(pkp_font_text, pkp_font_text_end);
    f_serif = add(pkp_font_serif, pkp_font_serif_end);
    format = ren->format();
    VkPipelineRenderingCreateInfoKHR rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &format;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = dev->instance();
    ii.PhysicalDevice = dev->physical();
    ii.Device = dev->device();
    ii.QueueFamily = dev->queue_family();
    ii.Queue = dev->queue();
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = std::max(2u, ren->image_count());
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = rci;
    ii.CheckVkResultFn = [](VkResult r) { if (r != VK_SUCCESS) NXLOG_E(kTag, "ImGui Vulkan call failed (%d)", static_cast<int>(r)); };
    if (!ImGui_ImplVulkan_Init(&ii)) {
        NXLOG_E(kTag, "ImGui_ImplVulkan_Init failed: no start screen");
        ImGui::DestroyContext(imgui);
        imgui = nullptr;
        return false;
    }
    backend = true;
    NXLOG_I(kTag, "start screen on a %ux%u swapchain (format %d, %s)", ren->extent().width, ren->extent().height, static_cast<int>(format),
            nxrt::extras::output_kind_name(ren->output_kind()));
    bg = load_picture("title.png");
    logo = load_picture("logo.png");
    return true;
}

Screen::Picture Screen::load_picture(const char* file) {
    Picture out;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    const std::string path = game + "/" + file;
    if (!exists(path)) return out;
    if (!nxrt::gfx::load_png_rgba(path, &rgba, &w, &h) || !w || !h) {
        NXLOG_W(kTag, "%s could not be read as a PNG: not shown", path.c_str());
        return out;
    }
    ImTextureData* t = IM_NEW(ImTextureData)();
    t->Create(ImTextureFormat_RGBA32, static_cast<int>(w), static_cast<int>(h));
    std::memcpy(t->GetPixels(), rgba.data(), rgba.size());
    t->UseColors = true;
    t->Status = ImTextureStatus_WantCreate;
    {
        std::lock_guard lk(dev->queue_mutex());   // the upload submits on the shared queue
        ImGui_ImplVulkan_UpdateTexture(t);
    }
    if (t->Status != ImTextureStatus_OK) {
        NXLOG_W(kTag, "%s could not be uploaded", path.c_str());
        IM_DELETE(t);
        return out;
    }
    t->DestroyPixels();
    out.tex = t;
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    NXLOG_I(kTag, "%s: %ux%u", path.c_str(), w, h);
    return out;
}

void Screen::free_picture(Picture& p) {
    if (!p.tex) return;
    if (backend) {
        p.tex->Status = ImTextureStatus_WantDestroy;
        p.tex->UnusedFrames = 1 << 20;
        ImGui_ImplVulkan_UpdateTexture(p.tex);
    }
    IM_DELETE(p.tex);
    p = {};
}

void Screen::shutdown() {
    if (prep && prep->worker.joinable()) prep->worker.join();
    dev->wait_idle();
    if (imgui) ImGui::SetCurrentContext(imgui);
    free_picture(bg);
    free_picture(logo);
    if (backend) ImGui_ImplVulkan_Shutdown();
    if (imgui) ImGui::DestroyContext(imgui);
    imgui = nullptr;
    backend = false;
}

void Screen::start_prepare() {
    prep = std::make_unique<Prepare>();
    state = State::Preparing;
    shown_progress = 0.f;
    error.clear();
    warning.clear();
    Prepare* p = prep.get();
    p->worker = std::thread(prepare_run, p, game);
    NXLOG_I(kTag, "prepare: started");
}

void Screen::confirm() {
    if (state == State::Preparing || state == State::Leaving) return;
    if (sel == 1) {
        state = State::Leaving;
        play = false;
        leave_at = Clock::now();
        return;
    }
    if (state == State::Ready) {
        state = State::Leaving;
        play = true;
        leave_at = Clock::now();
    } else {
        start_prepare();
    }
}

void Screen::move(int d) {
    if (state == State::Preparing || state == State::Leaving) return;
    sel = std::clamp(sel + d, 0, 1);
}

void Screen::handle_input() {
    nxrt::platform::pump_events(false, 0);
    nxrt::input::Event ev[64];
    size_t n;
    while ((n = nxrt::input::queue_drain(ev, 64)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            const nxrt::input::Event& e = ev[i];
            using T = nxrt::input::EventType;
            if (e.type == T::GamepadButtonDown) {
                switch (e.u.gpbutton.button) {
                    case 0: confirm(); break;     // SOUTH: cross
                    case 11: move(-1); break;     // d-pad up
                    case 12: move(1); break;      // d-pad down
                    default: break;
                }
            } else if (e.type == T::GamepadAxis && e.u.gpaxis.axis == 1) {   // left stick Y: one step per push
                const int v = e.u.gpaxis.value;
                const int dir = v < -20000 ? -1 : v > 20000 ? 1 : (std::abs(v) < 8000 ? 0 : stick_dir);
                if (dir && dir != stick_dir) move(dir);
                stick_dir = dir;
            } else if (e.type == T::KeyDown) {
                if (e.u.key.keycode == '\r' || e.u.key.keycode == ' ') confirm();
                else if (e.u.key.scancode == 82) move(-1);   // SDL_SCANCODE_UP
                else if (e.u.key.scancode == 81) move(1);    // SDL_SCANCODE_DOWN
            }
        }
    }
}

void Screen::update() {
    const auto now = Clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.f, 0.1f);
    last = now;
    if (state == State::Preparing && prep) {
        const float target = prep->progress.load();
        shown_progress += (target - shown_progress) * std::min(1.f, dt * 8.f);
        if (prep->done && shown_progress > target - 0.01f) {
            prep->worker.join();
            std::lock_guard lk(prep->mu);
            if (!prep->error.empty()) {
                error = prep->error;
                state = State::Failed;
            } else {
                warning = prep->warning;
                state = State::Ready;
                NXLOG_I(kTag, "prepare: done%s", warning.empty() ? "" : " (with a warning)");
            }
            sel = 0;
        }
    }
    sel_anim += (static_cast<float>(sel) - sel_anim) * std::min(1.f, dt * 14.f);
}

// ---- drawing ----------------------------------------------------------------------------------------------------------------------
// Laid out on a 1920x1080 page scaled to the swapchain (k): the name top left, the menu under it on a gold band that runs out of the
// left edge, messages under the menu.

ImU32 rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.f, 1.f) * 255.f)); }

// Text with extra space between the letters; returns its width.
float spaced_width(ImFont* font, float size, const char* text, float spacing) {
    float w = 0;
    for (const char* c = text; *c; ++c) w += font->CalcTextSizeA(size, FLT_MAX, 0, c, c + 1).x + (c[1] ? spacing : 0);
    return w;
}
void spaced_text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text, float spacing) {
    for (const char* c = text; *c; ++c) {
        dl->AddText(font, size, pos, col, c, c + 1);
        pos.x += font->CalcTextSizeA(size, FLT_MAX, 0, c, c + 1).x + spacing;
    }
}
// Spaced text over a soft dark shadow, so it reads on a bright picture.
void shadowed(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text, float spacing, float k, float alpha) {
    for (int i = 0; i < 4; ++i)
        spaced_text(dl, font, size, ImVec2(pos.x + (1.f + i) * k, pos.y + (1.5f + i) * k), rgba(0, 0, 0, 0.16f * alpha), text, spacing);
    spaced_text(dl, font, size, pos, col, text, spacing);
}
void multiline(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const std::string& text, float wrap, float k) {
    dl->AddText(font, size, ImVec2(pos.x + 1.5f * k, pos.y + 2 * k), rgba(0, 0, 0, 0.5f), text.c_str(), text.c_str() + text.size(), wrap);
    dl->AddText(font, size, pos, col, text.c_str(), text.c_str() + text.size(), wrap);
}
// A soft round glow: rings of triangles whose vertex colours fall off smoothly from the centre (the GPU blends between them, so
// there are no visible steps, however big the glow).
void glow(ImDrawList* dl, ImVec2 c, float r, int cr, int cg, int cb, float a) {
    constexpr int kSeg = 48, kRings = 6;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    auto col = [&](float u) { const float v = 1.f - u; return rgba(cr, cg, cb, a * 0.5f * v * v); };   // u: 0 centre .. 1 rim
    dl->PrimReserve(kSeg * 3 + (kRings - 1) * kSeg * 6, 1 + kRings * kSeg);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(c, uv, col(0.f));
    for (int ring = 1; ring <= kRings; ++ring) {
        const float u = static_cast<float>(ring) / kRings;
        for (int i = 0; i < kSeg; ++i) {
            const float ang = 6.2831853f * i / kSeg;
            dl->PrimWriteVtx(ImVec2(c.x + std::cos(ang) * r * u, c.y + std::sin(ang) * r * u), uv, col(u));
        }
    }
    auto at = [&](int ring, int i) { return static_cast<ImDrawIdx>(base + 1 + (ring - 1) * kSeg + (i % kSeg)); };
    for (int i = 0; i < kSeg; ++i) dl->PrimWriteIdx(base), dl->PrimWriteIdx(at(1, i)), dl->PrimWriteIdx(at(1, i + 1));
    for (int ring = 1; ring < kRings; ++ring)
        for (int i = 0; i < kSeg; ++i) {
            dl->PrimWriteIdx(at(ring, i)), dl->PrimWriteIdx(at(ring + 1, i)), dl->PrimWriteIdx(at(ring + 1, i + 1));
            dl->PrimWriteIdx(at(ring, i)), dl->PrimWriteIdx(at(ring + 1, i + 1)), dl->PrimWriteIdx(at(ring, i + 1));
        }
}
// A horizontal band that fades to nothing at its right end (and at the left, from fade_left).
void band(ImDrawList* dl, float x0, float x1, float y0, float y1, ImU32 col) {
    const ImU32 clear = col & ~IM_COL32_A_MASK;
    dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), col, clear, clear, col);
}

void Screen::build(const nxrt::gfx::FrameInfo& f) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float W = static_cast<float>(f.extent.width), H = static_cast<float>(f.extent.height);
    const float k = H / 1080.f;
    const float t = std::chrono::duration<float>(Clock::now() - start).count();
    const float in = std::clamp(t / 0.6f, 0.f, 1.f);   // fade in

    // background: the player's title.png (cover), or a drawn evening forest: a green gradient with warm drifting lights
    if (bg.tex) {
        const float s = std::max(W / bg.w, H / bg.h);
        const float iw = bg.w * s, ih = bg.h * s;
        dl->AddImage(ImTextureRef(bg.tex->GetTexID()), ImVec2((W - iw) / 2, (H - ih) / 2), ImVec2((W + iw) / 2, (H + ih) / 2));
    } else {
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), rgba(26, 52, 36, 1), rgba(60, 88, 46, 1), rgba(30, 50, 34, 1), rgba(10, 22, 16, 1));
        glow(dl, ImVec2(W * 0.78f, H * 0.18f), 700 * k, 255, 200, 120, 0.35f);   // the evening light, top right
        for (int i = 0; i < 26; ++i) {
            const float fx = std::fabs(std::fmod(std::sin(i * 12.9898f) * 43758.5453f, 1.f));
            const float fy = std::fabs(std::fmod(std::sin(i * 22.0369f) * 24634.6345f, 1.f));
            const float x = (0.32f + 0.68f * fx) * W + std::sin(t * 0.21f + i) * 30 * k;
            const float y = fy * H + std::cos(t * 0.17f + i * 0.5f) * 24 * k;
            const float r = (22 + (i % 5) * 14) * k;
            const float a = 0.55f + 0.35f * std::sin(t * 0.8f + i);
            glow(dl, ImVec2(x, y), r, 255, 214, 130, a);
            dl->AddCircleFilled(ImVec2(x, y), r * 0.07f, rgba(255, 236, 190, 0.5f * a), 12);
        }
    }
    // the left side darkened so the name and the menu read on any picture (lighter over the player's picture, which has its own)
    band(dl, 0, W * 0.62f, 0, H, rgba(3, 8, 6, bg.tex ? 0.35f : 0.72f));

    // the name: logo.png fitted in a 560x170 box, or POKOPIA in letters, with RECOMP under it. Not drawn over a title.png without
    // a logo.png: the picture is taken to carry its own name (a start-screen picture usually has the logo painted in).
    if (logo.tex || !bg.tex) {
        const float box_x = 112 * k, box_y = 70 * k, box_w = 560 * k, box_h = 170 * k;
        float name_cx = box_x + box_w / 2, name_bottom = box_y + box_h;
        if (logo.tex) {
            const float s = std::min(box_w / logo.w, box_h / logo.h);
            const float lw = logo.w * s, lh = logo.h * s;
            const ImVec2 p0(box_x, box_y + (box_h - lh) / 2);
            dl->AddImage(ImTextureRef(logo.tex->GetTexID()), p0, ImVec2(p0.x + lw, p0.y + lh), ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, in));
            name_cx = p0.x + lw / 2;
            name_bottom = p0.y + lh;
        } else {
            const char* name = "POKOPIA";
            const float size = 118 * k, sp = 4 * k;
            const float w = spaced_width(f_menu, size, name, sp);
            const ImVec2 p0(box_x, box_y + (box_h - size) / 2);
            for (int i = 0; i < 3; ++i)   // a little heavier than Medium
                shadowed(dl, f_menu, size, ImVec2(p0.x + i * 0.8f * k, p0.y), rgba(255, 250, 238, in), name, sp, k, in);
            name_cx = p0.x + w / 2;
            name_bottom = p0.y + size * 0.92f;
        }
        // RECOMP: spaced serif italic between two thin lines, centred under the name
        const char* sub = "RECOMP";
        const float sub_size = 34 * k, sub_sp = 13 * k;
        const float sub_w = spaced_width(f_serif, sub_size, sub, sub_sp);
        const float sx = name_cx - sub_w / 2, sy = name_bottom + 18 * k;
        shadowed(dl, f_serif, sub_size, ImVec2(sx, sy), rgba(240, 232, 214, in), sub, sub_sp, k, in);
        const float ly = sy + sub_size * 0.55f;
        dl->AddLine(ImVec2(sx - 92 * k, ly), ImVec2(sx - 24 * k, ly), rgba(240, 232, 214, 0.8f * in), 1.4f * k);
        dl->AddLine(ImVec2(sx + sub_w + 24 * k, ly), ImVec2(sx + sub_w + 92 * k, ly), rgba(240, 232, 214, 0.8f * in), 1.4f * k);
    }

    // the menu
    const char* first = state == State::Ready || (state == State::Leaving && play) ? "PLAY"
                        : state == State::Preparing                               ? "PREPARING"
                        : state == State::Failed                                  ? "TRY AGAIN"
                                                                                  : "PREPARE GAME";
    const char* items[2] = {first, "EXIT"};
    const float my = 528 * k, step = 78 * k, item_size = 36 * k, item_sp = 6.5f * k, tx = 162 * k;
    const float bar_y = my + sel_anim * step, bar_h = 70 * k, bar_w = 780 * k;
    // the band: a dark warm fill, gold hairlines top and bottom with a soft glow under them, all fading to the right
    band(dl, 0, bar_w, bar_y - bar_h / 2, bar_y + bar_h / 2, rgba(120, 88, 40, 0.62f * in));
    band(dl, 0, bar_w * 0.75f, bar_y - bar_h / 2, bar_y + bar_h / 2, rgba(30, 20, 8, 0.25f * in));
    for (const float edge : {bar_y - bar_h / 2, bar_y + bar_h / 2}) {
        band(dl, 0, bar_w * 0.9f, edge - 3 * k, edge + 3 * k, rgba(255, 196, 96, 0.22f * in));
        band(dl, 0, bar_w, edge - 0.8f * k, edge + 0.8f * k, rgba(255, 214, 140, 0.95f * in));
    }
    // chevron
    const float cx = 108 * k, ch = 11 * k;
    const ImVec2 chev[3] = {ImVec2(cx, bar_y - ch), ImVec2(cx + ch * 0.75f, bar_y), ImVec2(cx, bar_y + ch)};
    dl->AddPolyline(chev, 3, rgba(255, 248, 230, in), 0, 3.f * k);
    for (int i = 0; i < 2; ++i) {
        const float y = my + i * step - item_size * 0.62f;
        const bool on = i == sel;
        shadowed(dl, f_menu, item_size, ImVec2(tx, y), on ? rgba(255, 250, 238, in) : rgba(236, 232, 222, 0.85f * in), items[i], item_sp, k, in);
    }

    // progress, result and messages, under the menu
    const float px = tx, py = my + 2 * step - 4 * k, pw = 560 * k, wrap = 860 * k, small = 25 * k;
    if (state == State::Preparing && prep) {
        std::string what;
        {
            std::lock_guard lk(prep->mu);
            what = prep->step;
        }
        const float ph = 8 * k;
        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), rgba(255, 255, 255, 0.18f * in), ph / 2);
        const float fill = std::max(ph, pw * shown_progress);
        dl->AddRectFilled(ImVec2(px, py - 2 * k), ImVec2(px + fill, py + ph + 2 * k), rgba(255, 200, 110, 0.18f * in), ph);
        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + fill, py + ph), rgba(255, 210, 130, in), ph / 2);
        char pct[16];
        std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(shown_progress * 100.f + 0.5f));
        dl->AddText(f_text, small, ImVec2(px + pw + 18 * k, py + ph / 2 - small * 0.6f), rgba(255, 246, 225, in), pct);
        what += std::string(static_cast<size_t>(static_cast<int>(t * 2.5f) % 4), '.');
        multiline(dl, f_text, small, ImVec2(px, py + 22 * k), rgba(232, 226, 214, 0.92f * in), what, wrap, k);
    } else if (state == State::Failed) {
        multiline(dl, f_text, small, ImVec2(px, py), rgba(255, 160, 140, in), error, wrap, k);
    } else if ((state == State::Ready || state == State::Leaving) && !warning.empty()) {
        multiline(dl, f_text, small, ImVec2(px, py), rgba(255, 214, 140, in), warning, wrap, k);
    } else if (state == State::NeedPrepare) {
        multiline(dl, f_text, small, ImVec2(px, py), rgba(232, 226, 214, 0.85f * in),
                  "The first start checks your game files and shaders. It takes a minute or two.", wrap, k);
    }

    // the button hint, bottom right: a cross in a ring, then what it does
    if (state != State::Preparing && state != State::Leaving) {
        const char* hint = "Select";
        const float hs = 26 * k;
        const float tw = f_text->CalcTextSizeA(hs, FLT_MAX, 0, hint).x;
        const float hx = W - 80 * k - tw, hy = H - 74 * k;
        const ImVec2 c(hx - 30 * k, hy + hs * 0.55f);
        const float r = 17 * k, d = 6.5f * k;
        dl->AddCircleFilled(c, r + 2 * k, rgba(0, 0, 0, 0.35f * in), 32);
        dl->AddCircle(c, r, rgba(255, 246, 225, 0.9f * in), 32, 2.f * k);
        dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), rgba(160, 190, 255, in), 2.5f * k);
        dl->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), rgba(160, 190, 255, in), 2.5f * k);
        multiline(dl, f_text, hs, ImVec2(hx, hy), rgba(255, 246, 225, 0.9f * in), hint, 0, k);
    }

    // fade to black when leaving, from black when starting
    if (state == State::Leaving) {
        const float a = std::clamp(std::chrono::duration<float>(Clock::now() - leave_at).count() / 0.5f, 0.f, 1.f);
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), rgba(0, 0, 0, a));
    } else if (in < 1.f) {
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), rgba(0, 0, 0, 1.f - in));
    }
}

void Screen::draw(const nxrt::gfx::FrameInfo& f) {
    ImGui::SetCurrentContext(imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(f.extent.width), static_cast<float>(f.extent.height));
    io.DeltaTime = 1.f / 60.f;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    build(f);
    ImGui::Render();
    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = f.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, f.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(f.cmd, &ri);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmd);
    vkCmdEndRendering(f.cmd);
}

}  // namespace

bool title_screen_run(nxrt::gfx::Device& device, nxrt::gfx::Renderer& renderer, const char* game_dir) {
    Screen s;
    s.dev = &device;
    s.ren = &renderer;
    s.game = game_dir;
    if (!s.init()) return true;   // no screen: straight into the game, as before
    if (already_prepared(s.game, &s.warning)) s.state = State::Ready;
    NXLOG_I(kTag, "start screen: %s", s.state == State::Ready ? "prepared before, Play" : "Prepare game");
    renderer.set_overlay_hook([&s](const nxrt::gfx::FrameInfo& f) { s.draw(f); });
    nxrt::input::queue_clear();
    auto next = Clock::now();
    const bool shots = getenv("PKP_TITLE_SHOT") != nullptr;
    State shot_state = State::Leaving, seen_state = s.state;
    auto shot_since = Clock::now();
    for (;;) {
        s.handle_input();
        s.update();
        if (s.state == State::Leaving && Clock::now() - s.leave_at > std::chrono::milliseconds(550)) break;
        // PKP_TITLE_SHOT (pkp-env.txt, for checking the screen without a camera): a screenshot in pkp-results/ two seconds into
        // each state.
        if (shots && s.state != shot_state && Clock::now() - shot_since > std::chrono::seconds(2)) {
            shot_state = s.state;
            renderer.request_screenshot(s.game + "/pkp-results/title-" + std::to_string(static_cast<int>(s.state)) + ".png");
        }
        if (s.state != seen_state) {
            seen_state = s.state;
            shot_since = Clock::now();
        }
        nxrt::gfx::FrameInfo fi;
        if (renderer.begin_frame(&fi)) renderer.end_frame();
        // about 60 a second whatever the present mode
        next += std::chrono::microseconds(16667);
        const auto now = Clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else next = now;
    }
    renderer.set_overlay_hook(nullptr);
    s.shutdown();
    nxrt::input::queue_clear();   // nothing pressed here reaches the game
    NXLOG_I(kTag, "start screen: %s", s.play ? "Play" : "Exit");
    return s.play;
}

}  // namespace pkp_ps5

// SPDX-License-Identifier: MIT
// The DualSense as an SDL gamepad. The console's SDL3 build has no joystick driver of its own, so the first P3 runs had no
// controller at all: on PC the log says "gamepad added: Xbox One Controller ... controller 2 connected", on the console
// nothing, and the game left its main loop about a second after it set up hid (no player 1 controller). This reads the
// user's pad with scePad and feeds an SDL virtual joystick, so the runtime's platform layer (runtime/src/gfx/platform.cpp)
// sees an ordinary gamepad: added event, buttons, axes, rumble and light bar.
//
// Adapted from KongaTime's KH 358/2 Days port (ps5/input_native.c, MIT), whose pad sample layout and service calls come
// from KongaTime/PS5_ReXGlueTemplate src/platform.c (Copyright 2026 Mihawk, MIT).
#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>

extern "C" {
int sceUserServiceInitialize(const void*);
int sceUserServiceGetInitialUser(int32_t*);
int scePadInit(void);
int scePadOpen(int32_t user, int32_t type, int32_t index, const void*);
int scePadGetHandle(int32_t user, int32_t type, int32_t index);
int scePadRead(int32_t handle, void* samples, int32_t capacity);
int scePadClose(int32_t handle);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int scePadSetVibration(int32_t handle, const void* motors);
int scePadSetLightBar(int32_t handle, const void* color);
int sceKernelUsleep(unsigned microseconds);
}

namespace {

struct PadSample {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
    uint8_t reserved[66];
    int32_t connected;
    uint64_t timestamp;
    uint8_t extension[16], connected_count, remaining[15];
};
static_assert(sizeof(PadSample) == 120, "native pad sample size");
static_assert(offsetof(PadSample, connected) == 0x4c, "native connection offset");
static_assert(offsetof(PadSample, timestamp) == 0x50, "native timestamp offset");
static_assert(SDL_GAMEPAD_BUTTON_COUNT < 32, "review the native button mask after SDL updates");

struct Pad {
    int32_t handle = -1;
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    PadSample last{};
} g_pad;

// scePad button bits per SDL gamepad button (0: the pad has none, e.g. GUIDE: Options is START, the touchpad is BACK, the PS
// button belongs to the system).
uint32_t button_bit(int b) {
    switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return 0x4000;            // cross
        case SDL_GAMEPAD_BUTTON_EAST: return 0x2000;             // circle
        case SDL_GAMEPAD_BUTTON_WEST: return 0x8000;             // square
        case SDL_GAMEPAD_BUTTON_NORTH: return 0x1000;            // triangle
        case SDL_GAMEPAD_BUTTON_START: return 0x0008;            // options
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return 0x0002;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 0x0004;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 0x0400;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 0x0800;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return 0x0010;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return 0x0020;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return 0x0040;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return 0x0080;
        // The touchpad click is the Switch's - (Select): the DualSense has no Back/Select button, and Pokopia uses - (camera).
        case SDL_GAMEPAD_BUTTON_BACK: return 0x100000;
        default: return 0;
    }
}

Sint16 stick(unsigned v) { return static_cast<Sint16>(v < 128 ? (static_cast<int>(v) - 128) * 256 : (static_cast<int>(v) - 128) * 32767 / 127); }

void update(void*) {
    if (!g_pad.joystick || g_pad.handle < 0) return;
    PadSample samples[64];
    const int count = scePadRead(g_pad.handle, samples, 64);
    if (count > 0 && count <= 64) g_pad.last = samples[count - 1];
    const bool active = g_pad.last.connected && !(g_pad.last.buttons & 0x80000000u);
    // SDL's virtual driver packs only the button_mask entries, in order: no gaps for the buttons the pad lacks.
    int slot = 0;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
        if (const uint32_t bit = button_bit(i)) SDL_SetJoystickVirtualButton(g_pad.joystick, slot++, active && (g_pad.last.buttons & bit) != 0);
    const unsigned raw[4] = {g_pad.last.lx, g_pad.last.ly, g_pad.last.rx, g_pad.last.ry};
    for (int i = 0; i < 4; ++i) SDL_SetJoystickVirtualAxis(g_pad.joystick, i, active ? stick(raw[i]) : 0);
    SDL_SetJoystickVirtualAxis(g_pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, active ? static_cast<Sint16>(g_pad.last.l2 * 257 - 32768) : -32768);
    SDL_SetJoystickVirtualAxis(g_pad.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, active ? static_cast<Sint16>(g_pad.last.r2 * 257 - 32768) : -32768);
}

bool rumble(void*, Uint16 low, Uint16 high) {
    const uint8_t motors[2] = {static_cast<uint8_t>(low >> 8), static_cast<uint8_t>(high >> 8)};
    return scePadSetVibration(g_pad.handle, motors) >= 0;
}

bool led(void*, Uint8 r, Uint8 g, Uint8 b) {
    const uint8_t color[4] = {r, g, b, 0};
    return scePadSetLightBar(g_pad.handle, color) >= 0;
}

void cleanup(void*) {
    if (g_pad.handle >= 0) scePadClose(g_pad.handle);
    g_pad.handle = -1;
    g_pad.id = 0;
    g_pad.joystick = nullptr;
}

}  // namespace

// After SDL_Init with the joystick and gamepad subsystems (nxrt::platform::init). False with SDL_GetError set otherwise.
bool pkp_ps5_pad_init() {
    if (g_pad.id) return true;
    (void)sceUserServiceInitialize(nullptr);
    int32_t user = 0;
    if (sceUserServiceGetInitialUser(&user) < 0 || scePadInit() < 0) return SDL_SetError("the PS5 controller service is unavailable");
    for (int i = 0; i < 10 && g_pad.handle < 0; ++i) {
        g_pad.handle = scePadOpen(user, 0, 0, nullptr);
        if (g_pad.handle < 0) sceKernelUsleep(100000);
    }
    if (g_pad.handle < 0) g_pad.handle = scePadGetHandle(user, 0, 0);
    if (g_pad.handle < 0) return SDL_SetError("no controller for the PS5 user");
    (void)scePadSetVibrationMode(g_pad.handle, 2);
    g_pad.last = PadSample{};
    g_pad.last.lx = g_pad.last.ly = g_pad.last.rx = g_pad.last.ry = 128;

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x054c;    // Sony
    desc.product_id = 0x0ce6;   // DualSense
    desc.name = "DualSense (PS5 native)";
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
        if (button_bit(i)) {
            ++desc.nbuttons;
            desc.button_mask |= 1u << i;
        }
    desc.Update = update;
    desc.Rumble = rumble;
    desc.SetLED = led;
    desc.Cleanup = cleanup;
    g_pad.id = SDL_AttachVirtualJoystick(&desc);
    if (!g_pad.id) {
        cleanup(nullptr);
        return false;
    }
    g_pad.joystick = SDL_OpenJoystick(g_pad.id);
    if (!g_pad.joystick) {
        SDL_DetachVirtualJoystick(g_pad.id);
        cleanup(nullptr);
        return false;
    }
    update(nullptr);
    return true;
}

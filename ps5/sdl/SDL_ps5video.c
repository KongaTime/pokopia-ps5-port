/*
  SDL3 video driver "ps5" for the Pokopia PS5 port (zlib licence, as SDL). Built into SDL by ps5/tools/build-sdl3.sh.

  The console has one video output and no window system. Vulkan is Mesa's RADV, linked into the title (ps5/radv.cmake), and it
  presents through VK_KHR_display (PS5_Mesa's VideoOut WSI), so this driver:
    - reports one display at the video output's size (3840x2160 at 60 Hz) and makes every window cover it;
    - hands SDL's Vulkan layer RADV's own entry, vk_icdGetInstanceProcAddr (no loader library to open);
    - creates the surface on the display's first plane with SDL's VK_KHR_display helper.
  Input does not come through here: the title feeds the DualSense into SDL as a virtual joystick (ps5/game/pad_ps5.cpp).
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_PS5

#include "../SDL_sysvideo.h"
#include "../SDL_vulkan_internal.h"
#include "../../events/SDL_events_c.h"

#define PS5VID_DRIVER_NAME "ps5"
#define PS5_OUTPUT_W 3840
#define PS5_OUTPUT_H 2160

#ifdef SDL_VIDEO_VULKAN
// RADV's entry point (Mesa's ICD interface), linked into the title.
extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) __attribute__((weak));
#endif

static bool PS5_VideoInit(SDL_VideoDevice *_this)
{
    SDL_DisplayMode mode;
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_XRGB8888;
    mode.w = PS5_OUTPUT_W;
    mode.h = PS5_OUTPUT_H;
    mode.refresh_rate = 60.0f;
    if (SDL_AddBasicVideoDisplay(&mode) == 0) {
        return false;
    }
    return true;
}

static void PS5_VideoQuit(SDL_VideoDevice *_this)
{
}

static void PS5_PumpEvents(SDL_VideoDevice *_this)
{
    // no window system: nothing to pump (joysticks are updated by SDL's own joystick code)
}

static bool PS5_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props)
{
    // every window is the whole video output
    window->x = 0;
    window->y = 0;
    window->w = PS5_OUTPUT_W;
    window->h = PS5_OUTPUT_H;
    window->flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
    SDL_SetKeyboardFocus(window);
    return true;
}

static void PS5_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
}

static void PS5_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
    // the output size does not change: report it back
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, PS5_OUTPUT_W, PS5_OUTPUT_H);
}

static SDL_FullscreenResult PS5_SetWindowFullscreen(SDL_VideoDevice *_this, SDL_Window *window, SDL_VideoDisplay *display, SDL_FullscreenOp fullscreen)
{
    return SDL_FULLSCREEN_SUCCEEDED;
}

#ifdef SDL_VIDEO_VULKAN
static bool PS5_Vulkan_LoadLibrary(SDL_VideoDevice *_this, const char *path)
{
    if (!vk_icdGetInstanceProcAddr) {
        return SDL_SetError("ps5: no Vulkan driver linked into the title (vk_icdGetInstanceProcAddr)");
    }
    _this->vulkan_config.vkGetInstanceProcAddr = (SDL_FunctionPointer)vk_icdGetInstanceProcAddr;
    _this->vulkan_config.vkEnumerateInstanceExtensionProperties =
        (SDL_FunctionPointer)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    SDL_strlcpy(_this->vulkan_config.loader_path, "RADV (linked)", SDL_arraysize(_this->vulkan_config.loader_path));
    return true;
}

static void PS5_Vulkan_UnloadLibrary(SDL_VideoDevice *_this)
{
    _this->vulkan_config.vkGetInstanceProcAddr = NULL;
    _this->vulkan_config.vkEnumerateInstanceExtensionProperties = NULL;
}

static char const * const *PS5_Vulkan_GetInstanceExtensions(SDL_VideoDevice *_this, Uint32 *count)
{
    static const char *const extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
    if (count) {
        *count = SDL_arraysize(extensions);
    }
    return extensions;
}

static bool PS5_Vulkan_CreateSurface(SDL_VideoDevice *_this, SDL_Window *window, VkInstance instance,
                                     const struct VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    if (!_this->vulkan_config.vkGetInstanceProcAddr) {
        return SDL_SetError("ps5: Vulkan is not loaded");
    }
    return SDL_Vulkan_Display_CreateSurface(_this->vulkan_config.vkGetInstanceProcAddr, instance, allocator, surface);
}

static void PS5_Vulkan_DestroySurface(SDL_VideoDevice *_this, VkInstance instance, VkSurfaceKHR surface,
                                      const struct VkAllocationCallbacks *allocator)
{
    if (_this->vulkan_config.vkGetInstanceProcAddr) {
        SDL_Vulkan_DestroySurface_Internal(_this->vulkan_config.vkGetInstanceProcAddr, instance, surface, allocator);
    }
}
#endif

static void PS5_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static SDL_VideoDevice *PS5_CreateDevice(void)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }
    device->VideoInit = PS5_VideoInit;
    device->VideoQuit = PS5_VideoQuit;
    device->PumpEvents = PS5_PumpEvents;
    device->CreateSDLWindow = PS5_CreateWindow;
    device->DestroyWindow = PS5_DestroyWindow;
    device->SetWindowSize = PS5_SetWindowSize;
    device->SetWindowFullscreen = PS5_SetWindowFullscreen;
#ifdef SDL_VIDEO_VULKAN
    device->Vulkan_LoadLibrary = PS5_Vulkan_LoadLibrary;
    device->Vulkan_UnloadLibrary = PS5_Vulkan_UnloadLibrary;
    device->Vulkan_GetInstanceExtensions = PS5_Vulkan_GetInstanceExtensions;
    device->Vulkan_CreateSurface = PS5_Vulkan_CreateSurface;
    device->Vulkan_DestroySurface = PS5_Vulkan_DestroySurface;
#endif
    device->free = PS5_DeleteDevice;
    return device;
}

VideoBootStrap PS5_bootstrap = {
    PS5VID_DRIVER_NAME, "SDL PS5 video driver (VK_KHR_display on the video output)",
    PS5_CreateDevice,
    NULL, // no ShowMessageBox implementation
    false
};

#endif // SDL_VIDEO_DRIVER_PS5

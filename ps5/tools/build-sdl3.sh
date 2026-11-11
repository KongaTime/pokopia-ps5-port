#!/usr/bin/env bash
# SDL 3.4.12 (zlib) built with the PS5 toolchain for the title's platform layer, with this port's video driver "ps5"
# (ps5/sdl/SDL_ps5video.c: one display, Vulkan surfaces through VK_KHR_display on RADV). Static, video + events + joystick /
# gamepad (the DualSense comes in as a virtual joystick) only.
#   ps5/tools/build-sdl3.sh [prefix]     default $PKP_DEPS/sdl3 (ps5/local.env); include/SDL3/ and lib/libSDL3.a there
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ps5=$(dirname "$here")
source "$ps5/env.sh"
pkp_need PS5_SDK
deps=${PKP_DEPS:-${PKP_BUILD:?set PKP_BUILD in ps5/local.env}/deps}
prefix=${1:-$deps/sdl3}
ver=3.4.12
sum=f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7

src=$deps/sdl3-src
mkdir -p "$src"
tar=$src/SDL3-$ver.tar.gz
[[ -f $tar ]] || curl -sSL --max-time 300 -o "$tar" "https://github.com/libsdl-org/SDL/releases/download/release-$ver/SDL3-$ver.tar.gz"
echo "$sum  $tar" | sha256sum -c --quiet - || { echo "checksum mismatch: $tar" >&2; exit 1; }
rm -rf "$src/SDL3-$ver" && tar xzf "$tar" -C "$src"
sdl=$src/SDL3-$ver

# the driver, registered where SDL registers its own (CMake option block, build config, bootstrap list)
mkdir -p "$sdl/src/video/ps5"
cp "$ps5/sdl/SDL_ps5video.c" "$sdl/src/video/ps5/"
python3 - "$sdl" <<'PY'
import sys
root = sys.argv[1]
def edit(rel, old, new):
    p = f'{root}/{rel}'
    s = open(p, encoding='utf-8').read()
    if s.count(old) != 1:
        sys.exit(f'{rel}: anchor not found once: {old!r}')
    open(p, 'w', encoding='utf-8').write(s.replace(old, new))
edit('CMakeLists.txt', 'if(SDL_VIDEO)\n  if(SDL_OFFSCREEN)\n',
     'if(SDL_VIDEO)\n  if(SDL_PS5_VIDEO)\n    set(SDL_VIDEO_DRIVER_PS5 1)\n'
     '    sdl_glob_sources("${SDL3_SOURCE_DIR}/src/video/ps5/*.c")\n    set(HAVE_SDL_VIDEO TRUE)\n  endif()\n  if(SDL_OFFSCREEN)\n')
# the virtual joystick (how the title hands SDL the DualSense) is tied to HIDAPI, which the console build leaves out
edit('CMakeLists.txt', 'dep_option(SDL_VIRTUAL_JOYSTICK    "Enable the virtual-joystick driver" ON SDL_HIDAPI OFF)',
     'dep_option(SDL_VIRTUAL_JOYSTICK    "Enable the virtual-joystick driver" ON SDL_JOYSTICK OFF)')
edit('include/build_config/SDL_build_config.h.cmake', '#cmakedefine SDL_VIDEO_DRIVER_OFFSCREEN 1\n',
     '#cmakedefine SDL_VIDEO_DRIVER_OFFSCREEN 1\n#cmakedefine SDL_VIDEO_DRIVER_PS5 1\n')
edit('src/video/SDL_sysvideo.h', 'extern VideoBootStrap OFFSCREEN_bootstrap;\n',
     'extern VideoBootStrap OFFSCREEN_bootstrap;\nextern VideoBootStrap PS5_bootstrap;\n')
edit('src/video/SDL_video.c', '#ifdef SDL_VIDEO_DRIVER_OFFSCREEN\n    &OFFSCREEN_bootstrap,\n',
     '#ifdef SDL_VIDEO_DRIVER_PS5\n    &PS5_bootstrap,\n#endif\n#ifdef SDL_VIDEO_DRIVER_OFFSCREEN\n    &OFFSCREEN_bootstrap,\n')
PY

build=$deps/sdl3-build
rm -rf "$build"
cmake -S "$sdl" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PS5_SDK/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_PS5_VIDEO=ON -DSDL_VULKAN=ON -DSDL_OFFSCREEN=OFF -DSDL_DUMMYVIDEO=OFF \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VIVANTE=OFF \
    -DSDL_AUDIO=OFF -DSDL_RENDER=OFF -DSDL_GPU=OFF -DSDL_CAMERA=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF -DSDL_POWER=OFF \
    -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBICONV=OFF -DSDL_SYSTEM_ICONV=OFF \
    -DSDL_DISKAUDIO=OFF -DSDL_DUMMYAUDIO=OFF -DSDL_DUMMYCAMERA=OFF -DSDL_VIRTUAL_JOYSTICK=ON -DSDL_JOYSTICK=ON \
    > "$build.configure.log" 2>&1 || { tail -30 "$build.configure.log" >&2; exit 1; }
cmake --build "$build" -j "${JOBS:-6}" > "$build.log" 2>&1 || { grep -E 'error' "$build.log" | head -30 >&2; exit 1; }
cmake --install "$build" > /dev/null
echo "SDL $ver with the ps5 video driver: $prefix/lib/libSDL3.a, $prefix/include"

# SPDX-License-Identifier: GPL-3.0-or-later
# The PS5 build (ps5/PLAN.md). Configure with the PS5 SDK's toolchain:
#   cmake -S ps5 -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=<sdk>/toolchain/prospero.cmake -DP1_CASES=<make_cases.py output>
# ps5/p1/build.sh does this, then turns the link into a signed title.
cmake_minimum_required(VERSION 3.25)
project(pokopia_ps5 C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# ---- ps5/compat: the Win32 subset ----
file(GLOB PS5_COMPAT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/compat/*.cpp")
add_library(ps5compat STATIC ${PS5_COMPAT_SOURCES})
target_include_directories(ps5compat PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_compile_options(ps5compat PUBLIC "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/prelude.h" -fno-strict-aliasing)
target_link_libraries(ps5compat PUBLIC ps5platform)   # the SDK's platform layer: ps5_vrange_*, ps5_memory_query

# ---- the runtime (nxrt) ----
# Every runtime source outside the graphics shell, as runtime/CMakeLists.txt takes them, compiled against ps5/compat, less
# the files ps5/runtime/services_ps5.cpp stands in for (XAudio2, MMDevice, WIC, the swkbd window, Media Foundation) and
# the test tooling (its game probes read guest memory under SEH; ps5/runtime/testtools_off.cpp answers as it does when off).
file(GLOB_RECURSE NXRT_SOURCES CONFIGURE_DEPENDS "${ROOT}/runtime/src/*.cpp")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/(gfx|testtools)/")   # testtools: runtime/testtools_off.cpp
set(PS5_REPLACED
    services/audio_xaudio2.cpp services/audio_devices.cpp services/album.cpp services/swkbd_prompt.cpp
    media/movie_mf.cpp)
foreach(f ${PS5_REPLACED})
    list(REMOVE_ITEM NXRT_SOURCES "${ROOT}/runtime/src/${f}")
endforeach()
file(GLOB PS5_RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp")
add_library(nxrt STATIC ${NXRT_SOURCES} ${PS5_RUNTIME_SOURCES})
target_include_directories(nxrt PUBLIC "${ROOT}/runtime/include" "${ROOT}/recomp/include"
                           PRIVATE "${ROOT}/runtime/src" "${ROOT}/runtime/third_party")
target_compile_options(nxrt PRIVATE -O2 -g1 -w -fno-omit-frame-pointer -mcx16 -ffp-contract=off)
target_link_libraries(nxrt PUBLIC ps5compat)
# libvpx for the games' VP8/VP9 movies (src/media/video_codec.cpp; the PC build takes vcpkg's): ps5/tools/build-libvpx.sh
# builds it with the PS5 toolchain into this prefix. Without it the runtime builds without the decoders and movies are skipped.
set(PKP_LIBVPX "" CACHE PATH "ps5/tools/build-libvpx.sh install prefix (include/vpx, lib/libvpx.a)")
if(PKP_LIBVPX AND EXISTS "${PKP_LIBVPX}/lib/libvpx.a")
    target_include_directories(nxrt PRIVATE "${PKP_LIBVPX}/include")
    target_compile_definitions(nxrt PRIVATE NXRT_HAVE_LIBVPX=1)
    target_link_libraries(nxrt PUBLIC "${PKP_LIBVPX}/lib/libvpx.a")
    message(STATUS "pokopia-ps5: libvpx ${PKP_LIBVPX} (movies)")
else()
    target_compile_definitions(nxrt PRIVATE NXRT_HAVE_LIBVPX=0)
    message(STATUS "pokopia-ps5: no libvpx (run ps5/tools/build-libvpx.sh): movies are left out")
endif()

# ---- lifted modules: one static library per folder of a make_cases.py (or game) output ----
function(ps5_lifted_module target dir)
    file(GLOB srcs "${dir}/*.cpp")
    add_library(${target} STATIC ${srcs})
    target_include_directories(${target} PUBLIC "${ROOT}/recomp/include" "${dir}")
    # recomp/cmake/NxRecomp.cmake's flags for lifted code on a non-MSVC compiler
    target_compile_options(${target} PRIVATE -O1 -w -fno-exceptions -fno-rtti -fno-strict-aliasing -ffp-contract=off -mcx16)
endfunction()

# ---- the title link: what every PS5 title of this port takes (The-Darkness-Recomp / Monado-PS5) ----
function(ps5_title target)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/title/app_crt.cpp"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/title/posix_compat.cpp"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/title/fp_ieee.cpp"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/title/libc_ps5.cpp")
    target_link_options(${target} PRIVATE
        -lkernel -Wl,--as-needed     # libkernel, not the driver's libkernel_web (PRX_SCE_MODULE_LOAD_ERROR otherwise)
        -nostartfiles                # app_crt.cpp's _start takes the console's process parameters
        -Wl,--wrap=realpath -Wl,--wrap=printf -Wl,--wrap=vprintf -Wl,--wrap=puts -Wl,--wrap=putchar
        -Wl,--wrap=pthread_create -Wl,--wrap=pthread_exit   # thread_local destructors (compat/thread_atexit.cpp)
        # The heap in direct memory (ps5platform/heap.h): the console's libc heap is about 400 MiB of flexible memory, and the
        # game's loader alone needs more (the second P2 run ended reading main's 29 MB NSO into a vector, no error logged).
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -Wl,--wrap=posix_memalign
        -Wl,--wrap=aligned_alloc -Wl,--wrap=memalign -Wl,--wrap=malloc_usable_size -Wl,--wrap=reallocf
        -Wl,--wrap=reallocarray -Wl,--wrap=getline -Wl,--wrap=getdelim
        # The libc calls a title is refused or lacks, on ps5platform's replacements (title/libc_ps5.cpp, ps5platform/libc.h):
        # std::filesystem's directory streams and *at calls above all (the sixth P2 run ended in a file-system request).
        -Wl,--wrap=opendir -Wl,--wrap=fdopendir -Wl,--wrap=readdir -Wl,--wrap=rewinddir -Wl,--wrap=dirfd -Wl,--wrap=closedir
        -Wl,--wrap=openat -Wl,--wrap=unlinkat -Wl,--wrap=fchmodat -Wl,--wrap=fstatat -Wl,--wrap=mkdirat -Wl,--wrap=renameat
        -Wl,--wrap=statvfs -Wl,--wrap=fstatvfs -Wl,--wrap=statfs -Wl,--wrap=fstatfs -Wl,--wrap=access -Wl,--wrap=getcwd
        -Wl,--wrap=utimensat -Wl,--wrap=futimens -Wl,--wrap=sysconf -Wl,--wrap=pthread_getaffinity_np
        -Wl,--wrap=pthread_setaffinity_np -Wl,--wrap=mkstemp -Wl,--wrap=tmpfile -Wl,--wrap=readlink -Wl,--wrap=link
        -Wl,--wrap=symlink -Wl,--wrap=umask -Wl,--wrap=lstat
        -Wl,--wrap=clock_nanosleep -Wl,--wrap=pathconf -Wl,--wrap=fork   # raw syscall stubs in the SDK libc
        -Wl,--wrap=read -Wl,--wrap=pread -Wl,--wrap=write -Wl,--wrap=pwrite -Wl,--wrap=fread -Wl,--wrap=fwrite   # touch()
        -Wl,--exclude-libs=ALL -Wl,--eh-frame-hdr -Wl,--build-id=none
        -Wl,--no-dependent-libraries # the runtime's #pragma comment(lib, "avrt.lib") and the like name Windows libraries
        "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/title/ps5-pie.ld")
    target_link_libraries(${target} PRIVATE SceSystemService SceUserService)
endfunction()

# ---- P1: the lifted test programs on the runtime ----
set(P1_CASES "" CACHE PATH "make_cases.py output folder (elf/, expected/, gen/, cases.inc)")
if(P1_CASES)
    file(GLOB P1_GEN LIST_DIRECTORIES true "${P1_CASES}/gen/*")
    set(P1_LIBS "")
    foreach(dir ${P1_GEN})
        get_filename_component(name "${dir}" NAME)
        ps5_lifted_module(lift_${name} "${dir}")
        list(APPEND P1_LIBS lift_${name})
    endforeach()
    add_executable(pokopia_p1 "${CMAKE_CURRENT_SOURCE_DIR}/p1/p1_main.cpp")
    target_include_directories(pokopia_p1 PRIVATE "${P1_CASES}")
    target_compile_options(pokopia_p1 PRIVATE -O2 -g1 -fno-omit-frame-pointer)
    target_link_libraries(pokopia_p1 PRIVATE ${P1_LIBS} nxrt)
    ps5_title(pokopia_p1)
endif()

# ---- the game: the four modules ps5/game/lift.sh lifts from the player's own dump (never in the repository) ----
set(PKP_LIFT "" CACHE PATH "ps5/game/lift.sh output folder (main/, sdk/, subsdk0/, rtld/)")
set(PKP_BUILD_DIR "" CACHE PATH "the build work folder (game/build.sh PKP_BUILD)")
set(PKP_DEPS "" CACHE PATH "ps5/tools/fetch-deps.sh output folder (vulkan_headers/, volk/, vma/, imgui/)")
# game/build.sh passes these from ps5/local.env
set(PKP_SDL3_INCLUDE "" CACHE PATH "the PS5 SDL3's headers")
set(PKP_SDL3_LIB "" CACHE FILEPATH "the PS5 SDL3 static library (KongaTime's KH358/2 Days port builds it)")
if(PKP_LIFT)
    foreach(m main sdk subsdk0 rtld)
        ps5_lifted_module(pkp_${m} "${PKP_LIFT}/${m}")
    endforeach()
    add_custom_target(pkp_modules DEPENDS pkp_main pkp_sdk pkp_subsdk0 pkp_rtld)
endif()

if(PKP_LIFT AND PKP_DEPS)
    # The graphics shell (runtime/cmake/gfx.cmake's nxrt_gfx and its pinned dependencies) for the console. Left out: DLSS
    # (temporal_dlss.cpp, NVIDIA's Windows SDK) and the Windows display-mode code (win_display.cpp); the ASTC transcoder is
    # ps5/gfx/astc2bc7_off.cpp until P3. P2 runs with [boot] gfx = none, so none of the Vulkan path runs yet.
    add_library(ps5_volk STATIC "${PKP_DEPS}/volk/volk.c")
    target_include_directories(ps5_volk SYSTEM PUBLIC "${PKP_DEPS}/volk" "${PKP_DEPS}/vulkan_headers/include")
    target_compile_definitions(ps5_volk PUBLIC VK_NO_PROTOTYPES)
    add_library(ps5_imgui STATIC "${PKP_DEPS}/imgui/imgui.cpp" "${PKP_DEPS}/imgui/imgui_draw.cpp" "${PKP_DEPS}/imgui/imgui_tables.cpp"
                                 "${PKP_DEPS}/imgui/imgui_widgets.cpp" "${PKP_DEPS}/imgui/backends/imgui_impl_vulkan.cpp")
    target_include_directories(ps5_imgui SYSTEM PUBLIC "${PKP_DEPS}/imgui" "${PKP_DEPS}/imgui/backends")
    target_compile_definitions(ps5_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
    target_link_libraries(ps5_imgui PUBLIC ps5_volk)
    add_library(ps5_tiling STATIC "${ROOT}/tools/tiling/src/tegra_tiling.cpp")
    target_include_directories(ps5_tiling PUBLIC "${ROOT}/tools/tiling/include")
    add_library(ps5_sdl3 STATIC IMPORTED)
    set_target_properties(ps5_sdl3 PROPERTIES IMPORTED_LOCATION "${PKP_SDL3_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${PKP_SDL3_INCLUDE}")

    file(GLOB_RECURSE PS5_GFX_SOURCES CONFIGURE_DEPENDS "${ROOT}/runtime/src/gfx/*.cpp")
    list(FILTER PS5_GFX_SOURCES EXCLUDE REGEX "(nvn_gpu_stub|temporal_dlss|win_display|nvn_shader_worker)[.]cpp$")   # shader worker: gfx/shader_worker_off.cpp
    add_library(nxrt_gfx STATIC ${PS5_GFX_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/gfx/astc2bc7_off.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/gfx/gfx_ps5.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/gfx/shader_worker_off.cpp")
    target_include_directories(nxrt_gfx PUBLIC "${ROOT}/runtime/include" "${ROOT}/runtime/src/gfx/nvn/third_party" "${ROOT}/tools/textures/include"
                               "${PKP_DEPS}/vma/include"
                               PRIVATE "${ROOT}/runtime/src" "${ROOT}/runtime/src/gfx" "${ROOT}/runtime/src/gfx/nvn")
    target_compile_definitions(nxrt_gfx PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
    target_compile_options(nxrt_gfx PRIVATE -O2 -g1 -w -fno-omit-frame-pointer -mcx16 -ffp-contract=off
                           "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/endian_fix.h")
    target_link_libraries(nxrt_gfx PUBLIC nxrt ps5_volk ps5_imgui ps5_tiling ps5_sdl3)

    # The game module (runtime/games/pkp/CMakeLists.txt: data, core, play hooks), as one library.
    set(_pkp "${ROOT}/runtime/games/pkp")
    add_library(nxrt_game_pkp STATIC "${_pkp}/pkp_data.cpp" "${_pkp}/pkp_core.cpp" "${_pkp}/pkp_tt_probe.cpp" "${_pkp}/pkp_game.cpp"
                "${_pkp}/pkp_play.cpp" "${_pkp}/pkp_kbm.cpp" "${_pkp}/pkp_menu.cpp" "${_pkp}/pkp_movie_test.cpp" "${_pkp}/pkp_quality.cpp"
                "${_pkp}/pkp_taa.cpp")
    target_include_directories(nxrt_game_pkp PUBLIC "${ROOT}/runtime/games" "${_pkp}"
                               PRIVATE "${ROOT}/runtime/src" "${ROOT}/runtime/src/gfx" "${ROOT}/runtime/src/gfx/play" "${ROOT}/runtime/src/testtools")
    target_compile_options(nxrt_game_pkp PRIVATE -O2 -g1 -w -fno-omit-frame-pointer "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/endian_fix.h")
    target_link_libraries(nxrt_game_pkp PUBLIC nxrt_gfx)
    # Identity and file names from the game manifest (recomp/games/pkp/game.toml), as runtime/games/pkp/CMakeLists.txt makes them.
    include("${ROOT}/recomp/cmake/GameManifest.cmake")
    game_manifest_load("${ROOT}/recomp/games/pkp/game.toml")
    game_manifest_header("${CMAKE_BINARY_DIR}/pkp_gen/game_manifest.h")
    target_include_directories(nxrt_game_pkp PRIVATE "${CMAKE_BINARY_DIR}/pkp_gen")

    # The title (P2: ps5/game/pkp_ps5_main.cpp)
    add_executable(pokopia_ps5 "${CMAKE_CURRENT_SOURCE_DIR}/game/pkp_ps5_main.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/game/pad_ps5.cpp")
    target_compile_options(pokopia_ps5 PRIVATE -O2 -g1 -fno-omit-frame-pointer)
    target_link_libraries(pokopia_ps5 PRIVATE pkp_main pkp_sdk pkp_subsdk0 pkp_rtld nxrt_game_pkp nxrt_gfx nxrt
                          ScePad SceAudioOut SceUserService SceSystemService SceNet SceImeDialog
                          "${PKP_BUILD_DIR}/stubs/libSceCommonDialog.so")   # ps5/stubs, built by game/build.sh
    ps5_title(pokopia_ps5)
    # Bring-up traces of the main loop (game/loop_trace.cpp, game/write_trap.cpp, the job trace in pkp_ps5_main.cpp): link-time
    # wraps of game functions, so they cost on every call. -DPKP_PS5_BRINGUP=ON turns them on.
    option(PKP_PS5_BRINGUP "PS5: link the bring-up traces of the main loop" OFF)
    if(PKP_PS5_BRINGUP)
        target_compile_definitions(pokopia_ps5 PRIVATE PKP_PS5_BRINGUP=1)
        target_link_options(pokopia_ps5 PRIVATE -Wl,--wrap=f_7101fdcde0 -Wl,--wrap=f_7101fe00a0)   # bring-up: main-loop job trace
        target_sources(pokopia_ps5 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/game/loop_trace.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/game/write_trap.cpp")   # bring-up: main-loop callee trace
        target_link_options(pokopia_ps5 PRIVATE -Wl,--wrap=f_7101fdb040 -Wl,--wrap=f_71009fc100 -Wl,--wrap=f_7101fd9448 -Wl,--wrap=f_7101fd9440 -Wl,--wrap=f_7101fdd700 -Wl,--wrap=f_7101ffaca0 -Wl,--wrap=f_710069ed3c -Wl,--wrap=f_7100234140 -Wl,--wrap=f_7102045420 -Wl,--wrap=f_7101ff1c44 -Wl,--wrap=f_7101fdd6b4 -Wl,--wrap=f_7101fdcf20 -Wl,--wrap=f_7101ff1d60 -Wl,--wrap=f_7101ff1fe0 -Wl,--wrap=f_7101fdd6bc -Wl,--wrap=f_7102045a60 -Wl,--wrap=f_7101ff1f7c -Wl,--wrap=f_7101fdd6c0)
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/radv.cmake")   # P3: Vulkan on the console (RADV)
    ps5_radv(pokopia_ps5)
endif()

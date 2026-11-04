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
target_compile_options(ps5compat PUBLIC -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/prelude.h" -fno-strict-aliasing)
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
                                     "${CMAKE_CURRENT_SOURCE_DIR}/title/fp_ieee.cpp")
    target_link_options(${target} PRIVATE
        -lkernel -Wl,--as-needed     # libkernel, not the driver's libkernel_web (PRX_SCE_MODULE_LOAD_ERROR otherwise)
        -nostartfiles                # app_crt.cpp's _start takes the console's process parameters
        -Wl,--wrap=realpath -Wl,--wrap=printf -Wl,--wrap=vprintf -Wl,--wrap=puts -Wl,--wrap=putchar
        -Wl,--wrap=pthread_create -Wl,--wrap=pthread_exit   # thread_local destructors (compat/thread_atexit.cpp)
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

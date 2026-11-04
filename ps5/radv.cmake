# RADV, linked into a title (P3): the console has no system Vulkan, so the driver is PS5_Vulkan's static RADV archive,
# presenting through PS5_Mesa's VideoOut WSI (VK_KHR_display), and the console's SDL3 video driver (KongaTime's KH Days
# build, ps5/sdl/video_ps5.c) names its entry, vk_icdGetInstanceProcAddr. The recipe is Monado-PS5's ps5/radv.cmake
# (and PS5_Vulkan/tools/radv-link.sh), adapted to this port's title link (ps5_title), which already wraps the heap,
# pthread_create and the refused libc calls:
#
#   * libc names RADV calls that no system module exports are bound to the platform layer's ps5_ functions (--defsym),
#     only for names the layer defines and the title link does not already wrap;
#   * fclose / fflush go through the platform's stdio handling, as Monado's link does;
#   * Clang's builtins archive for RADV's emulated TLS (__emutls_get_address);
#   * the AGC link stubs (libSceAgc, libSceAgcDriver), which the public SDK has none of: built by The-Darkness-Recomp's
#     tools; ps5/game/build.sh hands the same files to ps5-native-tool link.

set(PKP_RADV_ARCHIVE "/mnt/c/kb/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
    CACHE FILEPATH "RADV's release archive for the console (whole-archive)")
set(PKP_AGC_STUBS "/mnt/c/kb/The-Darkness-Recomp/app/ps5/build/stubs" CACHE PATH "libSceAgc.so and libSceAgcDriver.so link stubs")
set(PKP_PLATFORM_LIBRARY "${CMAKE_SYSROOT}/lib/libps5platform.a" CACHE FILEPATH "the SDK's platform layer")

function(ps5_radv target)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/title/c11_threads.cpp")
    foreach(f "${PKP_RADV_ARCHIVE}" "${PKP_AGC_STUBS}/libSceAgc.so" "${PKP_AGC_STUBS}/libSceAgcDriver.so" "${PKP_PLATFORM_LIBRARY}")
        if(NOT EXISTS "${f}")
            message(FATAL_ERROR "RADV link: ${f} is missing")
        endif()
    endforeach()

    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --print-resource-dir OUTPUT_VARIABLE res OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(builtins "${res}/lib/linux/libclang_rt.builtins-x86_64.a")
    if(NOT EXISTS "${builtins}")
        set(builtins "${CMAKE_SYSROOT}/lib/libclang_rt.builtins-x86_64.a")
    endif()
    if(NOT EXISTS "${builtins}")
        message(FATAL_ERROR "RADV link: no clang builtins archive (tried ${res} and the sysroot)")
    endif()

    execute_process(COMMAND "${CMAKE_SYSROOT}/../bin/llvm-nm" --defined-only "${PKP_PLATFORM_LIBRARY}" OUTPUT_VARIABLE syms ERROR_QUIET)
    string(REGEX MATCHALL " T ps5_[A-Za-z0-9_]+" exports "${syms}")
    list(TRANSFORM exports REPLACE " T ps5_" "")

    # Already wrapped by ps5_title (title/libc_ps5.cpp) or defined by the compat layer (__cxa_thread_atexit_impl).
    set(skip opendir fdopendir readdir rewinddir dirfd closedir openat unlinkat fchmodat fstatat mkdirat renameat statvfs
        fstatvfs access utimensat futimens clock_nanosleep mkstemp readlink link symlink __cxa_thread_atexit_impl)
    set(flags)
    set(bound)
    foreach(name qsort_r mkstemps openlog popen pclose __xuname __assert __memset_chk regcomp regexec regfree regerror
            localtime_r newlocale freelocale strtod_l strtof_l dladdr localeconv_l strtoll_l strtoull_l strtold_l snprintf_l
            sscanf_l asprintf_l strcoll_l strxfrm_l strftime_l wcscoll_l wcsxfrm_l btowc_l wctob_l iswctype_l mbrlen_l
            mbrtowc_l mbsrtowcs_l mbsnrtowcs_l wcrtomb_l wcsnrtombs_l mbtowc_l ___mb_cur_max_l ___runetype_l ___tolower_l
            ___toupper_l __runes_for_locale catopen catgets catclose backtrace backtrace_symbols_fd arc4random arc4random_buf
            arc4random_uniform gmtime_r getaddrinfo freeaddrinfo if_nameindex if_freenameindex nl_langinfo nl_langinfo_l
            getpwuid_r posix_fallocate memfd_create localeconv strcasestr open_memstream)
        if(name IN_LIST exports AND NOT name IN_LIST skip)
            list(APPEND flags "-Wl,--defsym=${name}=ps5_${name}")
            list(APPEND bound "${name}")
        endif()
    endforeach()

    set(map "${CMAKE_BINARY_DIR}/radv-platform-local.map")
    file(WRITE "${map}.tmp" "{\n    local:\n")
    foreach(name ${bound})
        file(APPEND "${map}.tmp" "        ${name};\n")
    endforeach()
    file(APPEND "${map}.tmp" "};\n")
    file(RENAME "${map}.tmp" "${map}")
    message(STATUS "RADV: ${bound} bound to the platform layer")

    # The builtins and the platform layer come after the title's own libraries (link libraries, not options): RADV's
    # references to __wrap_pthread_create and the like must find compat/thread_atexit.cpp's wrapper (FTZ off, thread_local
    # destructors) first, not the platform layer's, which --allow-multiple-definition would otherwise keep.
    target_link_libraries(${target} PRIVATE ps5compat SceVideoOut "${PKP_AGC_STUBS}/libSceAgc.so" "${PKP_AGC_STUBS}/libSceAgcDriver.so"
                          "-Wl,--start-group" "${builtins}" "${PKP_PLATFORM_LIBRARY}" "-Wl,--end-group")
    target_link_options(${target} PRIVATE
        "-Wl,--whole-archive" "${PKP_RADV_ARCHIVE}" "-Wl,--no-whole-archive"
        ${flags} -Wl,--wrap=fclose -Wl,--wrap=fflush
        # RADV's C11 thread calls on libc's layer, whose return codes title/c11_threads.cpp translates to Mesa's
        -Wl,--wrap=mtx_init -Wl,--wrap=mtx_lock -Wl,--wrap=mtx_timedlock -Wl,--wrap=mtx_trylock -Wl,--wrap=mtx_unlock
        -Wl,--wrap=cnd_init -Wl,--wrap=cnd_wait -Wl,--wrap=cnd_timedwait -Wl,--wrap=cnd_signal -Wl,--wrap=cnd_broadcast
        -Wl,--wrap=thrd_create -Wl,--wrap=thrd_join -Wl,--wrap=thrd_detach
        "-Wl,--allow-multiple-definition"
        "-Wl,--version-script=${map}")
endfunction()

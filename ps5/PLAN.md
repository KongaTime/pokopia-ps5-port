# Pokopia Recomp on PS5 — porting plan

Branch `ps5` of [pokopia-recomp](https://github.com/drakolordx7/pokopia-recomp). Goal: the recompiled game running as a native PS5
homebrew title, built the way KongaTime's Xenoblade port (Monado-PS5, title `PPSA99426`) is built. Everything for the console lives
under `ps5/`; changes to `runtime/` stay small so the branch can follow upstream.

## What the upstream project is

- `recomp/` (nxrecomp, Python): lifts the game's AArch64 NSOs (`main`, `rtld`, `sdk`, `subsdk0`) to C++. The generated-code ABI
  (`recomp/include/nxrecomp/ctx.h`, `cxx*.h`) is portable: GCC builtins and `__int128`, MSVC paths behind `_MSC_VER`. The generated
  code is made from the player's own dump and is never committed.
- `runtime/` (nxrt, C++20): kernel/SVC, IPC and services HLE, memory, C++ exception unwinding, and the NVN2-on-Vulkan renderer
  (38k lines in `src/gfx`). Windows-only today: Win32 in 40 of the 72 non-graphics files, XAudio2, Media Foundation, WIC, COM.
- `game-build/`, `launcher/` (Tauri), `tools/shaders` (offline Ampere SASS → SPIR-V cache, 1–2 h on a PC).

## Measured baseline (2026-11-03)

`ps5/tools/census.sh` compiles each runtime file with the PS5 SDK's `prospero-clang++` (clang 23.1.1, `x86_64-sie-ps5`), syntax
only, in the archlinux WSL distro:

| Runtime dirs | Files | Compile now | Need `windows.h` | Other |
|---|---|---|---|---|
| core kernel mem os ipc loader eh diag services timestep | 72 | 30 | 40 | 2 (`_wfopen`; `std::atomic_ref` in hid.cpp) |

Not yet measured: `src/gfx` (needs SDL3/Vulkan/VMA/ImGui headers), `src/media`, `src/testtools`.

The Win32 surface in those 40 files is small and repetitive: SRW locks and condition variables, `GetLastError`, handles,
`Virtual{Alloc,Free,Protect,Query}`, file mappings (`CreateFileMapping`/`MapViewOfFileEx`: the heap section and its alias views),
`QueryPerformanceCounter`, `CreateThread`/priority/affinity/`Suspend`/`ResumeThread`, vectored exception handlers with `CONTEXT`,
`CreateFileW`/`ReadFile`/`WriteFile`, UTF-8/UTF-16 conversion.

## Compat layer status (2026-11-03)

`ps5/compat/` is written and builds for the PS5 (`ps5/tools/build-compat.sh` → `libps5compat.a`, `-Wall -Wextra` clean); every
function its headers declare is defined. **Not yet run on hardware**: P1 is what exercises it.

| File | What it does |
|---|---|
| `windows.h`, `avrt.h`, `bcrypt.h`, `mmsystem.h`, `psapi.h`, `tlhelp32.h`, `dbghelp.h`, `intrin.h` | the Win32 subset the runtime uses, LLP64 sizes |
| `prelude.h` (force-included) | MS CRT names (`_wfopen`, `_strtoui64`), `std::atomic_ref` (the SDK's libc++ lacks it) |
| `win32_core.cpp` | handles; threads (stacks reserved in direct memory, 1 MiB committed, grown by the fault handler); events, waitable timers, mutexes; SRW locks with real shared mode and their condition variables; WaitOnAddress; TLS; time (QPC at 10 MHz); UTF-8/UTF-32; topology |
| `win32_memory.cpp` | VirtualAlloc/Free/Protect/Query on ps5platform ranges; page-file sections backed in 64 KiB direct-memory units, mapped into every view; per-4 KiB state table; console pages take the union of their 4 KiB protections |
| `win32_fault.cpp` | vectored handlers and the unhandled filter on sigaction, CONTEXT ⇄ mcontext both ways, RaiseException |
| `win32_files.cpp` | files on POSIX fds, attributes, rename, directories, module path `/app0/eboot.bin`, env, full paths, ini reads |

Census with it: **61 of 72** non-graphics runtime files compile (was 30). The 11 left get PS5 back ends rather than compat:
`audio_xaudio2`, `audio_devices` (XAudio2/MMDevice → sceAudioOut), `album` (WIC → stb_image_write), `swkbd_prompt` (a Win32
window → the console's IME dialog), and the diagnostics that read other threads' registers or PE headers (`crash`, `sampler`,
`watchdog`, `symbols`, `stackwalk`, `gauge`, `memguard`, `kernel/starvation_guard`) → signal-based crash record, frame-pointer
stack walk, no sampling.

Behaviour that differs from Windows, on purpose:
- `SuspendThread`/`GetThreadContext` on another thread fail (`ERROR_NOT_SUPPORTED`); only the diagnostics above used them.
- A 4 KiB page made read-only beside a writable one in the same 16 KiB console page stays writable. `mem/write_watch.cpp`
  tracks 16 KiB pages on the console (`kPage` under `__PROSPERO__`), so its armed pages are never weakened this way.
- `WaitForMultipleObjects` polls at 1 ms; `WakeConditionVariable` and `WakeByAddressSingle` wake every waiter.
- `CreateFileMapping` supports page-file sections only (the runtime makes no file-backed ones).

## Approach

1. **`ps5/compat/` — the Win32 subset the core uses, implemented on the console.** A `windows.h` stand-in on the include path for
   the PS5 build only, so upstream files compile unchanged:
   - locks/condvars on pthreads (the SDK's `pthread_*_t` are pointers with NULL initialisers, so `SRWLOCK_INIT`/zero-init works);
   - memory on direct memory, using the libre layer from PS5_RPCS3Title (`ps5/libre/memory/vm.c`, MIT): reservations with
     `libre_vrange_reserve_at`, commits with `libre_vrange_commit` (execute granted after mapping, as the console requires), and the
     heap section with its alias views as one `libre_shm` block mapped at `kHeapBase` and at each alias;
   - faults: `AddVectoredExceptionHandler` on `sigaction(SIGSEGV/SIGBUS/SIGILL)` with `SA_ONSTACK`, as Monado-PS5's `platform.c`
     does, translating `ucontext_t` to the `CONTEXT`/`EXCEPTION_RECORD` fields the runtime reads (write watch, guest faults);
   - time on `clock_gettime(CLOCK_MONOTONIC)`, threads on pthreads, files on POSIX fds under `/app0` and `/download0`.
2. **Whole subsystems with PS5 back ends,** each a new file selected in CMake, not `#ifdef`s through upstream code:
   - audio sink: `sceAudioOut` (or SDL3 audio), replacing `audio_xaudio2.cpp`;
   - movies: ffmpeg (Monado-PS5 builds it: `/home/kai/monado-ffmpeg-install`), replacing `movie_mf.cpp`;
   - window/input: SDL3 PS5 build from the KH Days port, Vulkan through RADV linked statically
     (`PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a`, `radv.cmake` as in Monado-PS5), DualSense via `ScePad`;
   - crash reports and stack walks: the signal handler's record, not dbghelp.
3. **Title packaging** exactly as Monado-PS5's `build.sh`: `prospero.cmake` toolchain, `ps5-pie.ld`, `ps5-native-tool link` with the
   AGC link stubs, `self --sign`, `libc.prx` in `sce_module/`, `/data/homebrew/PPSA9xxxx/`.
4. **Generated code and shaders are built on the PC** from the player's dump (Python + clang are not on the console). The shader
   cache's SPIR-V is portable, so the PC tool's output can be shipped beside the title, as Monado's `cache/spirv.pack` is.

## Constraints to keep in view

- **16 KiB console pages, 4 KiB guest pages.** `kPage` is 0x1000. Commits and protections round out to 16 KiB; write watching then
  over-reports (performance, not correctness); guest permission changes inside one 16 KiB page must take the union.
- **Address layout fits.** `main` 0x71_0000_0000, `sdk` 0x75_0000_0000, heap 0x78_0000_0000 (+12 GiB), aliases 0x60_0000_0000
  (+8 GiB) are all inside what the console grants a title (0x10_0000_0000 up to 1 TiB, reserved exactly at the address).
- **Memory budget.** The PC build asks for 16 GB of system RAM; the console has one shared pool for CPU and GPU. Measure on hardware
  before tuning (`[memory] heap_gb`, pool mirrors off: unified memory makes `pool_mirror` pointless).
- **Supported game build.** The runtime's hooks are tied to `main` build id `76A489F3…` (2.0.1). The dump on this PC is base
  `D4E672B8…` and update 327680 `816C5358…`; neither is 2.0.1, so the game itself needs the 2.0.1a update.

## Milestones

| | Pass condition |
|---|---|
| P0 | Census of all of `runtime/src` (gfx included) recorded, compat layer compiles the 72 core files |
| P1 | Title runs nxrt's lifted test programs (`runtime/tests/lift/progs`) on the console and writes their results to `/data` — recompiled AArch64 executing on PS5 through nxrt, no game needed. **Built and signed** (`ps5/p1/`, `dist/PPSA90002`); not yet run on a console |
| P2 | Title boots Pokopia 2.0.1 to its first NVN present (log line) with audio/movies stubbed |
| P3 | First frame on screen through RADV; DualSense input |
| P4 | Audio, movies, saves; PS5 launcher (Prepare/Play/Settings) like Monado-PS5 |

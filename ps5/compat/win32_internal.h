// SPDX-License-Identifier: GPL-3.0-or-later
// Shared between the ps5/compat implementation files: the kernel objects behind HANDLEs.
#pragma once

#include <windows.h>

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace ps5compat {

// Every HANDLE the compat layer gives out points at one of these. A handle is a reference: CloseHandle drops it, and
// the object goes when the last reference does (a thread holds one on itself while it runs, a view on its section).
struct Object {
    enum class Kind { Thread, Event, Timer, Mutex, File, Section, Process };
    explicit Object(Kind k) : kind(k) {}
    virtual ~Object() = default;
    const Kind kind;
    std::atomic<int> refs{1};
};

void retain(Object *o);
void release(Object *o);
// The object behind a handle of the given kind, or null (and ERROR_INVALID_HANDLE) for a pseudo handle, a null or
// invalid handle, or one of another kind.
Object *object_of(HANDLE h);
template <class T>
T *as(HANDLE h, Object::Kind k) {
    Object *o = object_of(h);
    if (!o || o->kind != k) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    return static_cast<T *>(o);
}

// The pseudo handles of GetCurrentProcess / GetCurrentThread (the Windows values).
inline HANDLE pseudo_process() { return reinterpret_cast<HANDLE>(intptr_t{-1}); }
inline HANDLE pseudo_thread() { return reinterpret_cast<HANDLE>(intptr_t{-2}); }

// Monotonic nanoseconds.
inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
// The absolute CLOCK_MONOTONIC time `ms` from now, for pthread_cond_timedwait on a monotonic condition variable.
timespec deadline_after_ms(DWORD ms);
// A condition variable that times out against CLOCK_MONOTONIC.
void cond_init_monotonic(pthread_cond_t *cv);

std::string utf8_of(const wchar_t *s);
std::string native_path(const char *path);   // backslashes to slashes

// Called by the fault layer (win32_fault.cpp) at the start of every thread the compat layer runs: the alternate signal
// stack the fault handler runs on.
void fault_thread_init();
// Installs the fault handler (once): done by the first vectored handler, and by the first growable thread stack.
void fault_install();

// Thread stacks (win32_core.cpp) are reserved whole and committed as they grow, as Windows does: the fault handler calls
// this first for an access fault, and resumes the thread when it returns true (the page was in a stack's reserve and
// is committed now).
bool stack_grow(uint64_t address);

}  // namespace ps5compat

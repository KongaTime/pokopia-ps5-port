// SPDX-License-Identifier: GPL-3.0-or-later
// MMCSS (avrt.dll) for the PS5 build: there is no multimedia class scheduler on the console. The audio threads ask for
// "Pro Audio"; the call succeeds with a dummy handle and the thread keeps the priority the runtime gave it.
#pragma once
#include <windows.h>

static inline HANDLE AvSetMmThreadCharacteristicsA(LPCSTR, LPDWORD index) {
    if (index) *index = 0;
    return reinterpret_cast<HANDLE>(uintptr_t{1});
}
static inline HANDLE AvSetMmThreadCharacteristicsW(LPCWSTR, LPDWORD index) {
    if (index) *index = 0;
    return reinterpret_cast<HANDLE>(uintptr_t{1});
}
static inline BOOL AvRevertMmThreadCharacteristics(HANDLE) { return TRUE; }
typedef enum _AVRT_PRIORITY { AVRT_PRIORITY_LOW = -1, AVRT_PRIORITY_NORMAL, AVRT_PRIORITY_HIGH, AVRT_PRIORITY_CRITICAL } AVRT_PRIORITY;
static inline BOOL AvSetMmThreadPriority(HANDLE, AVRT_PRIORITY) { return TRUE; }

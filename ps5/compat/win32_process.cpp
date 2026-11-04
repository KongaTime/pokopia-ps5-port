// SPDX-License-Identifier: GPL-3.0-or-later
// ps5/compat: process counters (psapi.h) and Toolhelp snapshots (tlhelp32.h).
#include "win32_internal.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <ps5platform/memory.h>

#include <algorithm>
#include <atomic>

using namespace ps5compat;

BOOL GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS counters, DWORD size) {
    if (!counters || size < sizeof(PROCESS_MEMORY_COUNTERS)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    ps5_memory_stats s{};
    if (!ps5_memory_query(&s)) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    const SIZE_T used = s.total_bytes > s.free_bytes ? s.total_bytes - s.free_bytes : 0;
    static std::atomic<SIZE_T> peak{0};
    SIZE_T p = peak.load();
    while (used > p && !peak.compare_exchange_weak(p, used)) {}
    p = std::max(p, used);
    auto *c = counters;
    const DWORD cb = size >= sizeof(PROCESS_MEMORY_COUNTERS_EX) ? DWORD(sizeof(PROCESS_MEMORY_COUNTERS_EX)) : DWORD(sizeof(PROCESS_MEMORY_COUNTERS));
    memset(c, 0, cb);
    c->cb = cb;
    c->WorkingSetSize = c->PagefileUsage = used;
    c->PeakWorkingSetSize = c->PeakPagefileUsage = p;
    if (cb == sizeof(PROCESS_MEMORY_COUNTERS_EX)) reinterpret_cast<PROCESS_MEMORY_COUNTERS_EX *>(c)->PrivateUsage = used;
    return TRUE;
}

BOOL GetProcessHandleCount(HANDLE, PDWORD count) {
    if (count) *count = 0;   // not counted: diagnostics only
    return TRUE;
}

HANDLE CreateToolhelp32Snapshot(DWORD, DWORD) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE_VALUE;
}
BOOL Thread32First(HANDLE, LPTHREADENTRY32) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
BOOL Thread32Next(HANDLE, LPTHREADENTRY32) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

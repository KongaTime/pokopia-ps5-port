// SPDX-License-Identifier: GPL-3.0-or-later
// Toolhelp snapshots for the PS5 build: diag/gauge.cpp counts the process's threads with one. The console has no
// system-wide thread list to take; the snapshot fails (INVALID_HANDLE_VALUE), which the gauge reads as "unknown".
#pragma once
#include <windows.h>

#define TH32CS_SNAPTHREAD 0x4u
typedef struct tagTHREADENTRY32 {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ThreadID;
    DWORD th32OwnerProcessID;
    LONG tpBasePri;
    LONG tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32, *LPTHREADENTRY32;

HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD process_id);
BOOL Thread32First(HANDLE snapshot, LPTHREADENTRY32 entry);
BOOL Thread32Next(HANDLE snapshot, LPTHREADENTRY32 entry);

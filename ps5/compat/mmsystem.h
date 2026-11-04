// SPDX-License-Identifier: GPL-3.0-or-later
// winmm's timer resolution calls for the PS5 build. The console's sleeps are not rounded to a 15.6 ms system tick, so
// asking for a 1 ms period changes nothing there; the calls succeed and do nothing.
#pragma once
#include <windows.h>

typedef UINT MMRESULT;
#define TIMERR_NOERROR 0u
static inline MMRESULT timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
static inline MMRESULT timeEndPeriod(UINT) { return TIMERR_NOERROR; }
DWORD timeGetTime();

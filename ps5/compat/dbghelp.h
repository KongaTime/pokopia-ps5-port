// SPDX-License-Identifier: GPL-3.0-or-later
// dbghelp's name undecoration for the PS5 build: diag/symbols.cpp undecorates the names of a linker map. The console
// build's names are Itanium-mangled, so they are demangled with the C++ ABI's demangler; a name it does not take is
// copied as it is.
#pragma once
#include <windows.h>

#define UNDNAME_COMPLETE 0x0000u
#define UNDNAME_NO_ALLOCATION_LANGUAGE 0x0010u
#define UNDNAME_NO_ACCESS_SPECIFIERS 0x0080u
#define UNDNAME_NO_MEMBER_TYPE 0x0200u
DWORD UnDecorateSymbolName(LPCSTR name, LPSTR out, DWORD size, DWORD flags);

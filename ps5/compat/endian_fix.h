// SPDX-License-Identifier: GPL-3.0-or-later
// Force-included into the graphics shell of the PS5 build, after prelude.h.
//
// FreeBSD's <sys/endian.h>, which the console's SDL3 and libc headers bring in, defines bswap16/bswap32/bswap64 as
// function-like macros. recomp/include/nxrecomp/ctx.h declares functions of those names, and a file that includes both
// (src/gfx/overlay.cpp, play/play.cpp) fails to compile. The header is read here first and the macros dropped; its include
// guard keeps the later includes from defining them again. The graphics shell has no use for them.
#pragma once
#include <sys/endian.h>
#undef bswap16
#undef bswap32
#undef bswap64

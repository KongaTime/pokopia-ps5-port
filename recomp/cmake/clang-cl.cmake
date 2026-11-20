# SPDX-License-Identifier: GPL-3.0-or-later
# Toolchain file: LLVM clang-cl (C:\Program Files\LLVM\bin) with the MSVC x64 environment (run through scripts/nx_env.cmd,
# or from a shell where vcvars64.bat has run), Ninja, optional sccache.
# NX_LLVM_BIN (environment): an extracted LLVM release's bin folder instead of the installed one (scripts/nx_env.cmd).
set(CMAKE_SYSTEM_NAME Windows)
if(DEFINED ENV{NX_LLVM_BIN})
  file(TO_CMAKE_PATH "$ENV{NX_LLVM_BIN}" _nx_llvm)
else()
  set(_nx_llvm "C:/Program Files/LLVM/bin")
endif()
set(CMAKE_C_COMPILER "${_nx_llvm}/clang-cl.exe")
set(CMAKE_CXX_COMPILER "${_nx_llvm}/clang-cl.exe")
set(CMAKE_LINKER "${_nx_llvm}/lld-link.exe")
set(CMAKE_RC_COMPILER "${_nx_llvm}/llvm-rc.exe")
set(CMAKE_MT "${_nx_llvm}/llvm-mt.exe")
# static MSVC runtime (/MT): the lifted libraries and the runtime that links them must agree, and players then need no
# Visual C++ Redistributable (CMake needs policy CMP0091 NEW, i.e. cmake_minimum_required 3.15 or later, for this to apply)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# the debug info format of an exe built here is not useful to the oracles: keep objects small
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "")
if(NOT DEFINED ENV{NX_NO_SCCACHE})
  find_program(NX_SCCACHE sccache HINTS "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Links")
  if(NX_SCCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER "${NX_SCCACHE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${NX_SCCACHE}")
  endif()
endif()

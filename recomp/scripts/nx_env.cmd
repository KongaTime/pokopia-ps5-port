@echo off
rem Sets up the build environment: MSVC x64 (vcvars64), LLVM clang-cl 23, CMake, Ninja, sccache.
rem Usage from cmd:  scripts\nx_env.cmd cmake --build build\x
rem                  scripts\nx_env.cmd            (no arguments: just prints the tool versions)
rem Visual Studio: the 18 Build Tools, else the 2022 ones. LLVM: %NX_LLVM_BIN% if set (an extracted LLVM release's bin\),
rem else C:\Program Files\LLVM\bin.
set "NX_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%NX_VCVARS%" set "NX_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%NX_VCVARS%" >nul
if not defined NX_LLVM_BIN set "NX_LLVM_BIN=C:\Program Files\LLVM\bin"
set "PATH=%NX_LLVM_BIN%;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%~dp0..\..\tools\.venv\Scripts;%PATH%"
if "%~1"=="" (
  clang-cl --version & cmake --version & ninja --version & sccache --version
  exit /b 0
)
%*

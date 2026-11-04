// SPDX-License-Identifier: GPL-3.0-or-later
// ps5/compat: the calls the runtime's diagnostics make (crash reports, stack walks, symbol names, the sampler).
#include "win32_internal.h"

#include <dbghelp.h>

#include <cxxabi.h>

#include <cstdlib>
#include <cstring>

using namespace ps5compat;

BOOL SetThreadStackGuarantee(PULONG size) {
    // The fault handler runs on an alternate signal stack of its own (fault_thread_init), which is what the guarantee buys
    // a handler on Windows; the previous guarantee reported is none.
    if (size) *size = 0;
    return TRUE;
}

BOOL GetThreadIdealProcessorEx(HANDLE, PPROCESSOR_NUMBER ideal) {
    if (ideal) *ideal = PROCESSOR_NUMBER{0, 0, 0};
    return TRUE;
}

BOOL ReadProcessMemory(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T size, SIZE_T *read) {
    // This process only: readable memory is copied, as Windows does; anything else fails without touching it.
    if (read) *read = 0;
    if (process != GetCurrentProcess() && GetProcessId(process) != GetCurrentProcessId()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (IsBadReadPtr(address, size)) {
        SetLastError(299);   // ERROR_PARTIAL_COPY
        return FALSE;
    }
    memcpy(buffer, address, size);
    if (read) *read = size;
    return TRUE;
}

HMODULE LoadLibraryA(LPCSTR) {
    SetLastError(126);   // ERROR_MOD_NOT_FOUND
    return nullptr;
}
HMODULE LoadLibraryW(LPCWSTR) { return LoadLibraryA(nullptr); }
BOOL FreeLibrary(HMODULE) { return TRUE; }

// ---- frame-pointer unwinding ----

namespace {
bool readable_word(uint64_t a) { return a >= 0x10000 && !(a & 7) && !IsBadReadPtr(reinterpret_cast<void *>(a), 16); }
RUNTIME_FUNCTION g_frame_record{};
}  // namespace

PRUNTIME_FUNCTION RtlLookupFunctionEntry(DWORD64 pc, PDWORD64 image_base, void *) {
    // A frame to step through: the walker's context must carry an rbp that points at a readable frame record. Without one
    // the function is treated as a leaf (null), which is what the stack walker does at the first frame.
    if (image_base) *image_base = 0;
    (void)pc;
    return &g_frame_record;
}

void *RtlVirtualUnwind(DWORD, DWORD64, DWORD64, PRUNTIME_FUNCTION, PCONTEXT c, PVOID *handler_data, PDWORD64 establisher,
                       void *) {
    if (handler_data) *handler_data = nullptr;
    if (establisher) *establisher = c->Rbp;
    // The frame record at rbp: [rbp] the caller's rbp, [rbp + 8] the return address. A chain that does not go up the
    // stack, or a record that cannot be read, ends the walk (Rip 0).
    const uint64_t fp = c->Rbp;
    if (!readable_word(fp) || fp < c->Rsp) {
        c->Rip = 0;
        return nullptr;
    }
    const uint64_t caller_fp = *reinterpret_cast<const uint64_t *>(fp);
    c->Rip = *reinterpret_cast<const uint64_t *>(fp + 8);
    c->Rsp = fp + 16;
    c->Rbp = caller_fp > fp ? caller_fp : 0;
    return nullptr;
}

DWORD UnDecorateSymbolName(LPCSTR name, LPSTR out, DWORD size, DWORD) {
    if (!name || !out || !size) return 0;
    int status = 0;
    char *d = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    const char *text = status == 0 && d ? d : name;
    const size_t n = std::min<size_t>(strlen(text), size - 1);
    memcpy(out, text, n);
    out[n] = 0;
    free(d);
    return static_cast<DWORD>(n);
}

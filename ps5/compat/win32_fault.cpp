// SPDX-License-Identifier: GPL-3.0-or-later
// ps5/compat: structured exceptions (windows.h) on the console's signals.
//
// The runtime's fault users are vectored handlers: the write tracker (mem/write_watch.cpp) unprotects a page and asks
// for EXCEPTION_CONTINUE_EXECUTION, the crash reporter records and gives up. Here one sigaction handler (SIGSEGV, SIGBUS,
// SIGILL, SIGFPE, SIGTRAP) builds the EXCEPTION_RECORD and CONTEXT a Windows handler is given, calls the vectored
// handlers in order and then the unhandled-exception filter, and on EXCEPTION_CONTINUE_EXECUTION writes the CONTEXT back
// into the signal context and returns, which resumes the thread there. Nothing handled it: the previous disposition is
// restored and the instruction faults again into it (the console's default ends the title), as Monado-PS5's handler does.
//
// The handler list is read without locks (the handler runs inside a signal): adding or removing a handler publishes a
// new array, and old arrays are never freed (there are a handful in a run).
#include "win32_internal.h"

#include <ps5platform/context.h>

#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace ps5compat {
namespace {

struct Handlers {
    size_t n = 0;
    PVECTORED_EXCEPTION_HANDLER fn[32] = {};
};

std::atomic<Handlers *> g_handlers{new Handlers};
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_filter{nullptr};
std::mutex g_install_mu;
bool g_installed = false;
constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
struct sigaction g_previous[sizeof kSignals / sizeof kSignals[0]];

thread_local sigjmp_buf *t_guard = nullptr;   // the innermost seh_guard of this thread

// fxsave image in mc_fpstate: MXCSR at byte 24, XMM0..15 at byte 160 (16 bytes each).
constexpr size_t kMxcsr = 24, kXmm = 160;

void to_context(const mcontext_t &m, CONTEXT &c) {
    c = CONTEXT{};
    c.ContextFlags = CONTEXT_FULL;
    c.Rax = m.mc_rax; c.Rcx = m.mc_rcx; c.Rdx = m.mc_rdx; c.Rbx = m.mc_rbx;
    c.Rsp = m.mc_rsp; c.Rbp = m.mc_rbp; c.Rsi = m.mc_rsi; c.Rdi = m.mc_rdi;
    c.R8 = m.mc_r8; c.R9 = m.mc_r9; c.R10 = m.mc_r10; c.R11 = m.mc_r11;
    c.R12 = m.mc_r12; c.R13 = m.mc_r13; c.R14 = m.mc_r14; c.R15 = m.mc_r15;
    c.Rip = m.mc_rip;
    c.EFlags = static_cast<DWORD>(m.mc_rflags);
    c.SegCs = static_cast<WORD>(m.mc_cs);
    c.SegSs = static_cast<WORD>(m.mc_ss);
    const auto *fx = reinterpret_cast<const uint8_t *>(m.mc_fpstate);
    memcpy(&c.MxCsr, fx + kMxcsr, 4);
    memcpy(c.Xmm, fx + kXmm, sizeof c.Xmm);
}

void from_context(const CONTEXT &c, mcontext_t &m) {
    m.mc_rax = c.Rax; m.mc_rcx = c.Rcx; m.mc_rdx = c.Rdx; m.mc_rbx = c.Rbx;
    m.mc_rsp = c.Rsp; m.mc_rbp = c.Rbp; m.mc_rsi = c.Rsi; m.mc_rdi = c.Rdi;
    m.mc_r8 = c.R8; m.mc_r9 = c.R9; m.mc_r10 = c.R10; m.mc_r11 = c.R11;
    m.mc_r12 = c.R12; m.mc_r13 = c.R13; m.mc_r14 = c.R14; m.mc_r15 = c.R15;
    m.mc_rip = c.Rip;
    m.mc_rflags = c.EFlags;
    auto *fx = reinterpret_cast<uint8_t *>(m.mc_fpstate);
    memcpy(fx + kMxcsr, &c.MxCsr, 4);
    memcpy(fx + kXmm, c.Xmm, sizeof c.Xmm);
}

// Vectored handlers, then the filter. EXCEPTION_CONTINUE_EXECUTION or EXCEPTION_CONTINUE_SEARCH.
LONG dispatch(EXCEPTION_POINTERS &ep) {
    const Handlers *h = g_handlers.load(std::memory_order_acquire);
    for (size_t i = 0; i < h->n; ++i)
        if (h->fn[i] && h->fn[i](&ep) == EXCEPTION_CONTINUE_EXECUTION) return EXCEPTION_CONTINUE_EXECUTION;
    if (LPTOP_LEVEL_EXCEPTION_FILTER f = g_filter.load(std::memory_order_acquire)) {
        const LONG r = f(&ep);
        if (r == EXCEPTION_CONTINUE_EXECUTION) return r;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_signal(int sig, siginfo_t *info, void *uctx) {
    auto *uc = static_cast<ucontext_t *>(uctx);
    // A thread stack growing into its reserve: committed, and the access is retried.
    if ((sig == SIGSEGV || sig == SIGBUS) && info && stack_grow(reinterpret_cast<uint64_t>(info->si_addr))) return;
    EXCEPTION_RECORD rec{};
    CONTEXT ctx;
    to_context(uc->uc_mcontext, ctx);
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
    switch (sig) {
        case SIGSEGV:
        case SIGBUS: {
            rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
            rec.NumberParameters = 2;
            // The page-fault error code: bit 1 a write, bit 4 an instruction fetch (Windows: 1 write, 8 execute, 0 read).
            const uint64_t err = uc->uc_mcontext.mc_err;
            rec.ExceptionInformation[0] = (err & 0x10) ? 8 : (err & 0x2) ? 1 : 0;
            rec.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(info ? info->si_addr : nullptr);
            break;
        }
        case SIGILL: rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION; break;
        case SIGFPE: rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO; break;
        default: rec.ExceptionCode = EXCEPTION_BREAKPOINT; break;
    }
    EXCEPTION_POINTERS ep{&rec, &ctx};
    if (dispatch(ep) == EXCEPTION_CONTINUE_EXECUTION) {
        from_context(ctx, uc->uc_mcontext);
        return;
    }
    // An access fault inside seh_guard: back out of the guarded call, as __except (EXCEPTION_EXECUTE_HANDLER) does.
    if ((sig == SIGSEGV || sig == SIGBUS) && t_guard) siglongjmp(*t_guard, 1);
    // Nobody handled it: the disposition from before ours, and the fault happens again into it.
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i)
        if (kSignals[i] == sig) sigaction(sig, &g_previous[i], nullptr);
}

void install() {
    std::lock_guard lk(g_install_mu);
    if (g_installed) return;
    fault_thread_init();
    struct sigaction sa{};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;   // NODEFER: a handler that faults reaches the next one
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i) sigaction(kSignals[i], &sa, &g_previous[i]);
    g_installed = true;
}

thread_local void *t_altstack = nullptr;

}  // namespace

void fault_install() { install(); }

bool seh_guard_call(void (*fn)(void *), void *arg) {
    install();
    sigjmp_buf jb;
    sigjmp_buf *outer = t_guard;
    // The signal mask is saved and restored (the handler runs with the fault's signal blocked unless SA_NODEFER; it is set).
    if (sigsetjmp(jb, 1) != 0) {
        t_guard = outer;
        return false;
    }
    t_guard = &jb;
    fn(arg);
    t_guard = outer;
    return true;
}

void fault_thread_init() {
    if (t_altstack) return;
    // 64 KiB: the handlers build a CONTEXT and may log (the runtime's crash reporter formats a report on it).
    constexpr size_t kSize = 64 * 1024;
    t_altstack = malloc(kSize);
    if (!t_altstack) return;
    stack_t ss{};
    ss.ss_sp = t_altstack;
    ss.ss_size = kSize;
    sigaltstack(&ss, nullptr);
}

}  // namespace ps5compat

using namespace ps5compat;

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler) {
    if (!handler) return nullptr;
    install();
    std::lock_guard lk(g_install_mu);
    const Handlers *old = g_handlers.load(std::memory_order_acquire);
    if (old->n == 32) return nullptr;
    auto *next = new Handlers;
    size_t k = 0;
    if (first) next->fn[k++] = handler;
    for (size_t i = 0; i < old->n; ++i) next->fn[k++] = old->fn[i];
    if (!first) next->fn[k++] = handler;
    next->n = k;
    g_handlers.store(next, std::memory_order_release);   // the old array stays: a signal may still be reading it
    return reinterpret_cast<PVOID>(handler);
}

ULONG RemoveVectoredExceptionHandler(PVOID handle) {
    std::lock_guard lk(g_install_mu);
    const Handlers *old = g_handlers.load(std::memory_order_acquire);
    auto *next = new Handlers;
    bool found = false;
    for (size_t i = 0; i < old->n; ++i) {
        if (!found && reinterpret_cast<PVOID>(old->fn[i]) == handle) {
            found = true;
            continue;
        }
        next->fn[next->n++] = old->fn[i];
    }
    if (!found) {
        delete next;
        return 0;
    }
    g_handlers.store(next, std::memory_order_release);
    return 1;
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    install();
    return g_filter.exchange(filter, std::memory_order_acq_rel);
}

void RtlCaptureContext(PCONTEXT context) {
    // The caller's state as far as this function sees it: where it returns to, its stack and frame pointer. The crash
    // reporter uses it to start a stack walk; the other registers are not captured.
    *context = CONTEXT{};
    context->ContextFlags = CONTEXT_CONTROL;
    void *frame = __builtin_frame_address(0);
    context->Rip = reinterpret_cast<DWORD64>(__builtin_return_address(0));
    context->Rbp = *static_cast<DWORD64 *>(frame);
    context->Rsp = reinterpret_cast<DWORD64>(frame) + 16;
}

BOOL GetThreadContext(HANDLE thread, PCONTEXT context) {
    // Another thread's registers cannot be read on the console (there is no SuspendThread to stop it first).
    if (thread != pseudo_thread() || !context) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    RtlCaptureContext(context);
    return TRUE;
}

void RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments) {
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags;
    rec.NumberParameters = std::min<DWORD>(count, EXCEPTION_MAXIMUM_PARAMETERS);
    for (DWORD i = 0; i < rec.NumberParameters && arguments; ++i) rec.ExceptionInformation[i] = arguments[i];
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
    EXCEPTION_POINTERS ep{&rec, &ctx};
    // The handlers see it (the crash reporter records it). The runtime raises only non-continuable exceptions, so this
    // never returns, whatever they answer.
    dispatch(ep);
    fprintf(stderr, "ps5compat: unhandled exception 0x%08x\n", code);
    fflush(nullptr);
    abort();
}

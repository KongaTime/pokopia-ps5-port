// SPDX-License-Identifier: GPL-3.0-or-later
// ps5/compat: errors, handles, threads, synchronisation, time, strings and the small calls (windows.h).
#include "win32_internal.h"

#include <bcrypt.h>
#include <mmsystem.h>
#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>

#include <errno.h>
#include <pthread_np.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
// libkernel's thread priority (the PS4 SDK's range: 256 highest .. 767 lowest).
int scePthreadSetprio(pthread_t thread, int priority);
}

namespace ps5compat {
namespace {

thread_local DWORD t_last_error = 0;

// ---- objects ----

struct Waitable : Object {
    using Object::Object;
    pthread_mutex_t mu{};
    pthread_cond_t cv{};
    bool init_sync() {
        pthread_mutex_init(&mu, nullptr);
        cond_init_monotonic(&cv);
        return true;
    }
    ~Waitable() override {
        pthread_cond_destroy(&cv);
        pthread_mutex_destroy(&mu);
    }
    // With mu held: is the object signalled for this waiter, and take it if waiting does that (auto-reset events, mutexes).
    virtual bool try_take() = 0;
    // Waits until try_take succeeds or the time runs out. WAIT_OBJECT_0 or WAIT_TIMEOUT.
    virtual DWORD wait(DWORD ms) {
        const timespec until = deadline_after_ms(ms);
        pthread_mutex_lock(&mu);
        DWORD r = WAIT_OBJECT_0;
        while (!try_take()) {
            if (ms == 0) { r = WAIT_TIMEOUT; break; }
            const int e = ms == INFINITE ? pthread_cond_wait(&cv, &mu) : pthread_cond_timedwait(&cv, &mu, &until);
            if (e == ETIMEDOUT && !try_take()) { r = WAIT_TIMEOUT; break; }
            if (e == ETIMEDOUT) break;
        }
        pthread_mutex_unlock(&mu);
        return r;
    }
};

// ---- thread stacks ----
//
// A stack is a direct-memory reservation (ps5_vrange) with its top committed; the rest is committed by stack_grow when
// the thread first touches it, from the fault handler, which runs on the thread's alternate signal stack. pthread
// stacks would otherwise be backed whole at creation out of the title's small flexible budget: the runtime asks 64 MiB
// for every guest thread.
constexpr uint64_t kStackInitial = 1u << 20;   // committed at creation
constexpr uint64_t kStackStep = 256u << 10;    // committed per growth fault
constexpr int kStackSlots = 512;

struct Stack {
    std::atomic<uint64_t> lo{0}, hi{0};          // the reservation; 0 when the slot is free
    std::atomic<uint64_t> committed{0};          // lowest committed address
    std::atomic<uint64_t> dead_since{0};         // its thread ended at this time (now_ns); 0 while it runs
};
Stack g_stacks[kStackSlots];
std::mutex g_stacks_mu;   // claiming and releasing slots (stack_grow reads them without it)

// Releases the stacks of threads that ended more than a second ago (a detached thread still runs its last instructions
// on its stack after its exit is signalled), then claims a slot for a new stack.
Stack *stack_alloc(uint64_t size) {
    std::lock_guard lk(g_stacks_mu);
    const uint64_t t = now_ns();
    Stack *free_slot = nullptr;
    for (Stack &s : g_stacks) {
        const uint64_t dead = s.dead_since.load(std::memory_order_acquire);
        if (s.hi.load() && dead && t - dead > 1000000000ull) {
            ps5_vrange_release(reinterpret_cast<void *>(s.lo.load()), s.hi.load() - s.lo.load());
            s.hi.store(0);
            s.lo.store(0);
            s.dead_since.store(0);
        }
        if (!free_slot && !s.hi.load()) free_slot = &s;
    }
    if (!free_slot) return nullptr;
    size = (size + 0xFFFF) & ~uint64_t{0xFFFF};
    void *base = nullptr;
    if (ps5_vrange_reserve(size, nullptr, 0x10000, &base) != 0) return nullptr;
    const uint64_t lo = reinterpret_cast<uint64_t>(base), hi = lo + size;
    const uint64_t first = hi - std::min<uint64_t>(kStackInitial, size);
    if (ps5_vrange_commit(reinterpret_cast<void *>(first), hi - first, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE) != 0) {
        ps5_vrange_release(base, size);
        return nullptr;
    }
    free_slot->committed.store(first);
    free_slot->lo.store(lo);
    free_slot->dead_since.store(0);
    free_slot->hi.store(hi, std::memory_order_release);
    return free_slot;
}

struct Thread : Waitable {
    Thread() : Waitable(Kind::Thread) { init_sync(); }
    Stack *stack = nullptr;      // a stack of ours, released after the thread ends
    pthread_t pt{};
    DWORD id = 0;
    bool adopted = false;        // a thread the compat layer did not start (main thread, std::thread)
    LPTHREAD_START_ROUTINE start = nullptr;
    LPVOID param = nullptr;
    int suspend = 0;             // CreateThread(CREATE_SUSPENDED) holds the start; ResumeThread releases it
    bool exited = false;
    DWORD exit_code = 259;       // STILL_ACTIVE
    int priority = THREAD_PRIORITY_NORMAL;
    bool try_take() override { return exited; }
};

struct Event : Waitable {
    Event(bool manual, bool initial) : Waitable(Kind::Event), manual(manual), set(initial) { init_sync(); }
    bool manual, set;
    bool try_take() override {
        if (!set) return false;
        if (!manual) set = false;
        return true;
    }
};

struct Timer : Waitable {
    explicit Timer(bool manual) : Waitable(Kind::Timer), manual(manual) { init_sync(); }
    bool manual;
    bool armed = false, fired = false;
    uint64_t due_ns = 0;
    uint64_t period_ns = 0;
    bool try_take() override {
        if (armed && !fired && now_ns() >= due_ns) {
            if (period_ns) due_ns += period_ns;
            else fired = true;
            if (!manual) return true;
        }
        if (fired && manual) return true;
        return false;
    }
    // A timer's wait sleeps to the due time itself: nothing signals the condition variable when it comes.
    DWORD wait(DWORD ms) override {
        const uint64_t limit = ms == INFINITE ? UINT64_MAX : now_ns() + uint64_t(ms) * 1000000ull;
        for (;;) {
            pthread_mutex_lock(&mu);
            const bool took = try_take();
            const bool live = armed;
            const uint64_t due = due_ns;
            pthread_mutex_unlock(&mu);
            if (took) return WAIT_OBJECT_0;
            const uint64_t t = now_ns();
            if (t >= limit) return WAIT_TIMEOUT;
            // Unarmed: wait for SetWaitableTimer (or the timeout); armed: sleep to whichever comes first.
            const uint64_t until = live ? std::min<uint64_t>(due, limit) : std::min<uint64_t>(limit, t + 1000000ull);
            timespec ts{static_cast<time_t>(until / 1000000000ull), static_cast<long>(until % 1000000000ull)};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        }
    }
};

struct Mutex : Waitable {
    Mutex() : Waitable(Kind::Mutex) { init_sync(); }
    DWORD owner = 0;
    int count = 0;
    bool try_take() override;
};

struct Process : Object {
    Process() : Object(Kind::Process) {}
};

// ---- thread registry ----

std::mutex g_threads_mu;
std::map<DWORD, Thread *> g_threads;   // live thread objects by id (not retained: the object removes itself)
std::atomic<DWORD> g_next_tid{0x1000};
thread_local DWORD t_tid = 0;
thread_local Thread *t_self = nullptr;

DWORD current_tid() {
    if (!t_tid) t_tid = g_next_tid.fetch_add(4);   // Windows thread ids are multiples of 4
    return t_tid;
}

bool Mutex::try_take() {
    const DWORD me = current_tid();
    if (count == 0 || owner == me) {
        owner = me;
        count++;
        return true;
    }
    return false;
}

// The thread object of the calling thread, made on first use for a thread the compat layer did not start; it is
// signalled when the thread ends (its thread_local guard below runs then).
struct SelfGuard {
    Thread *t = nullptr;
    ~SelfGuard() {
        if (!t) return;
        pthread_mutex_lock(&t->mu);
        t->exited = true;
        if (t->exit_code == 259) t->exit_code = 0;
        pthread_cond_broadcast(&t->cv);
        pthread_mutex_unlock(&t->mu);
        if (t->stack) t->stack->dead_since.store(now_ns(), std::memory_order_release);
        {
            std::lock_guard lk(g_threads_mu);
            g_threads.erase(t->id);
        }
        release(t);
    }
};
thread_local SelfGuard t_guard;

Thread *self_thread() {
    if (t_self) return t_self;
    auto *t = new Thread;
    t->pt = pthread_self();
    t->id = current_tid();
    t->adopted = true;
    t_self = t;
    t_guard.t = t;   // the guard's reference
    std::lock_guard lk(g_threads_mu);
    g_threads[t->id] = t;
    return t;
}

void *thread_main(void *arg) {
    auto *t = static_cast<Thread *>(arg);
    t_tid = t->id;
    t_self = t;
    t_guard.t = t;   // the reference CreateThread gave the thread itself
    fault_thread_init();
    pthread_mutex_lock(&t->mu);
    while (t->suspend > 0) pthread_cond_wait(&t->cv, &t->mu);
    pthread_mutex_unlock(&t->mu);
    const DWORD code = t->start(t->param);
    pthread_mutex_lock(&t->mu);
    t->exit_code = code;
    pthread_mutex_unlock(&t->mu);
    return nullptr;   // the guard signals and drops the reference
}

int console_priority(int win) {
    if (win <= THREAD_PRIORITY_IDLE) return 767;
    if (win >= THREAD_PRIORITY_TIME_CRITICAL) return 600;
    switch (win) {
        case THREAD_PRIORITY_LOWEST: return 760;
        case THREAD_PRIORITY_BELOW_NORMAL: return 720;
        case THREAD_PRIORITY_NORMAL: return 700;
        case THREAD_PRIORITY_ABOVE_NORMAL: return 680;
        case THREAD_PRIORITY_HIGHEST: return 640;
    }
    return win < 0 ? 740 : 660;
}

Thread *thread_of(HANDLE h) {
    if (h == pseudo_thread()) return self_thread();
    return as<Thread>(h, Object::Kind::Thread);
}

uint64_t g_process_mask = 0;   // the CPUs the title may run on, read once from the first thread that asks

uint64_t process_mask() {
    static std::once_flag once;
    std::call_once(once, [] {
        uint64_t m = 0;
        if (scePthreadGetaffinity(pthread_self(), &m) != 0 || m == 0) {
            const long n = sysconf(_SC_NPROCESSORS_ONLN);
            m = n > 0 && n < 64 ? (1ull << n) - 1 : 0xff;
        }
        g_process_mask = m;
    });
    return g_process_mask;
}

// ---- parking: hashed buckets of waiters, for SRW locks, condition variables and WaitOnAddress ----
//
// A waiter counts itself in, then checks its condition and sleeps, all under the bucket's mutex; a waker changes the
// state first and only takes the mutex (to broadcast) when the bucket has waiters. Both sides are sequentially
// consistent, so the waker either sees the waiter or the waiter sees the new state: no wake is lost.

struct Bucket {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv{};
    std::atomic<int> waiters{0};
    std::once_flag once;
};
Bucket g_buckets[256];
Bucket &bucket_of(const volatile void *a) {
    Bucket &b = g_buckets[(reinterpret_cast<uintptr_t>(a) >> 3) % 256];
    std::call_once(b.once, [&] { cond_init_monotonic(&b.cv); });
    return b;
}

// Sleeps until ready() is true or the deadline passes (null: no deadline). False on timeout.
template <class Ready>
bool park(const volatile void *key, Ready ready, const timespec *until) {
    Bucket &b = bucket_of(key);
    pthread_mutex_lock(&b.mu);
    b.waiters.fetch_add(1, std::memory_order_seq_cst);
    bool ok = true;
    while (!ready()) {
        const int e = until ? pthread_cond_timedwait(&b.cv, &b.mu, until) : pthread_cond_wait(&b.cv, &b.mu);
        if (e == ETIMEDOUT) {
            ok = ready();
            break;
        }
    }
    b.waiters.fetch_sub(1, std::memory_order_seq_cst);
    pthread_mutex_unlock(&b.mu);
    return ok;
}

void unpark_all(const volatile void *key) {
    Bucket &b = bucket_of(key);
    if (b.waiters.load(std::memory_order_seq_cst) == 0) return;
    pthread_mutex_lock(&b.mu);
    pthread_cond_broadcast(&b.cv);
    pthread_mutex_unlock(&b.mu);
}

// SRW lock word (SRWLOCK::Ptr): bit 0 a writer holds it, the rest twice the number of readers. Zero is unlocked, as
// SRWLOCK_INIT. Readers may enter while other readers hold it, so a thread that takes it shared twice does not deadlock
// itself (which Windows allows in practice while no writer waits).
std::atomic<uintptr_t> &srw_word(PSRWLOCK lock) { return *reinterpret_cast<std::atomic<uintptr_t> *>(&lock->Ptr); }
static_assert(sizeof(std::atomic<uintptr_t>) == sizeof(void *), "SRWLOCK holds one atomic word");

bool srw_try_exclusive(std::atomic<uintptr_t> &w) {
    uintptr_t expected = 0;
    return w.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
}
bool srw_try_shared(std::atomic<uintptr_t> &w) {
    uintptr_t s = w.load(std::memory_order_relaxed);
    while (!(s & 1))
        if (w.compare_exchange_weak(s, s + 2, std::memory_order_acquire, std::memory_order_relaxed)) return true;
    return false;
}
void srw_lock_exclusive(PSRWLOCK lock) {
    auto &w = srw_word(lock);
    for (int spin = 0; spin < 64; ++spin) {
        if (srw_try_exclusive(w)) return;
        __builtin_ia32_pause();
    }
    while (!srw_try_exclusive(w)) park(lock, [&] { return w.load(std::memory_order_seq_cst) == 0; }, nullptr);
}
void srw_lock_shared(PSRWLOCK lock) {
    auto &w = srw_word(lock);
    for (int spin = 0; spin < 64; ++spin) {
        if (srw_try_shared(w)) return;
        __builtin_ia32_pause();
    }
    while (!srw_try_shared(w)) park(lock, [&] { return !(w.load(std::memory_order_seq_cst) & 1); }, nullptr);
}
void srw_unlock_exclusive(PSRWLOCK lock) {
    srw_word(lock).store(0, std::memory_order_seq_cst);
    unpark_all(lock);
}
void srw_unlock_shared(PSRWLOCK lock) {
    if (srw_word(lock).fetch_sub(2, std::memory_order_seq_cst) == 2) unpark_all(lock);
}

// ---- named mutexes (this process only) ----
std::mutex g_names_mu;
std::map<std::string, Mutex *> g_named;

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// shared helpers (win32_internal.h)

bool stack_grow(uint64_t address) {
    for (Stack &s : g_stacks) {
        const uint64_t hi = s.hi.load(std::memory_order_acquire);
        if (!hi) continue;
        const uint64_t lo = s.lo.load(), committed = s.committed.load();
        if (address < lo || address >= committed) continue;
        // Commit from a step below the fault up to what is committed; the lowest page stays a guard: a stack that runs
        // through its whole reserve faults there, as on Windows.
        uint64_t from = (address & ~(kStackStep - 1));
        if (from <= lo) from = lo + 0x4000;
        if (address < from) return false;
        if (ps5_vrange_commit(reinterpret_cast<void *>(from), committed - from,
                              PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE) != 0)
            return false;
        s.committed.store(from, std::memory_order_release);
        return true;
    }
    return false;
}

void retain(Object *o) { o->refs.fetch_add(1, std::memory_order_relaxed); }

void release(Object *o) {
    if (o && o->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete o;
}

Object *object_of(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || h == pseudo_process() || h == pseudo_thread()) return nullptr;
    return static_cast<Object *>(h);
}

timespec deadline_after_ms(DWORD ms) {
    const uint64_t t = now_ns() + (ms == INFINITE ? 0 : uint64_t(ms) * 1000000ull);
    return timespec{static_cast<time_t>(t / 1000000000ull), static_cast<long>(t % 1000000000ull)};
}

void cond_init_monotonic(pthread_cond_t *cv) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &a);
    pthread_condattr_destroy(&a);
}

std::string utf8_of(const wchar_t *s) {
    std::string out;
    if (!s) return out;
    for (; *s; ++s) {
        const uint32_t c = static_cast<uint32_t>(*s);
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | c >> 12); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | c >> 18); out += char(0x80 | (c >> 12 & 0x3F)); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::string native_path(const char *path) {
    std::string p = path ? path : "";
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

}  // namespace ps5compat

using namespace ps5compat;

// ---------------------------------------------------------------------------------------------------------------------
// errors

DWORD GetLastError() { return t_last_error; }
void SetLastError(DWORD error) { t_last_error = error; }

// ---------------------------------------------------------------------------------------------------------------------
// handles

BOOL CloseHandle(HANDLE h) {
    if (h == pseudo_process() || h == pseudo_thread()) return TRUE;
    Object *o = object_of(h);
    if (!o) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (o->kind == Object::Kind::Mutex) {
        std::lock_guard lk(g_names_mu);
        for (auto it = g_named.begin(); it != g_named.end(); ++it)
            if (it->second == o) {
                if (o->refs.load() == 1) g_named.erase(it);
                break;
            }
    }
    release(o);
    return TRUE;
}

BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, LPHANDLE dst, DWORD, BOOL, DWORD) {
    if (!dst) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    Object *o = src == pseudo_thread() ? self_thread() : object_of(src);
    if (src == pseudo_process()) {
        *dst = new Process;
        return TRUE;
    }
    if (!o) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    retain(o);
    *dst = o;
    return TRUE;
}

// ---------------------------------------------------------------------------------------------------------------------
// processes and threads

HANDLE GetCurrentProcess() { return pseudo_process(); }
HANDLE GetCurrentThread() { return pseudo_thread(); }
DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
DWORD GetCurrentThreadId() { return current_tid(); }

DWORD GetThreadId(HANDLE thread) {
    Thread *t = thread_of(thread);
    return t ? t->id : 0;
}

DWORD GetProcessId(HANDLE process) {
    return process == pseudo_process() || (object_of(process) && object_of(process)->kind == Object::Kind::Process)
               ? GetCurrentProcessId()
               : 0;
}

HANDLE OpenProcess(DWORD, BOOL, DWORD process_id) {
    // One process runs on the console: only this one can be opened.
    if (process_id != GetCurrentProcessId()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return new Process;
}

HANDLE OpenThread(DWORD, BOOL, DWORD thread_id) {
    if (thread_id == current_tid()) self_thread();
    std::lock_guard lk(g_threads_mu);
    auto it = g_threads.find(thread_id);
    if (it == g_threads.end()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    retain(it->second);
    return it->second;
}

HANDLE CreateThread(void *, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags, LPDWORD thread_id) {
    if (!start) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto *t = new Thread;
    t->start = start;
    t->param = param;
    t->suspend = (flags & CREATE_SUSPENDED) ? 1 : 0;
    t->id = g_next_tid.fetch_add(4);
    retain(t);   // the thread's own reference, dropped when it ends
    {
        std::lock_guard lk(g_threads_mu);
        g_threads[t->id] = t;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Windows reserves the stack and commits it as it grows, and so does this (stack_alloc): the runtime asks for 64 MiB
    // per guest thread ([threads] host_stack_mb). Without a size the thread gets the console's default stack.
    if (stack) {
        fault_install();   // growth comes through the fault handler
        t->stack = stack_alloc(std::max<size_t>(stack, 256 * 1024));
        if (t->stack) {
            const uint64_t lo = t->stack->lo.load(), hi = t->stack->hi.load();
            pthread_attr_setstack(&attr, reinterpret_cast<void *>(lo), hi - lo);
        } else {
            pthread_attr_setstacksize(&attr, std::max<size_t>(stack, 256 * 1024));
        }
    }
    const int e = pthread_create(&t->pt, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (e != 0) {
        if (t->stack) t->stack->dead_since.store(1, std::memory_order_release);   // long dead: the next claim frees it
        {
            std::lock_guard lk(g_threads_mu);
            g_threads.erase(t->id);
        }
        release(t);
        release(t);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    pthread_detach(t->pt);
    if (thread_id) *thread_id = t->id;
    return t;
}

DWORD ResumeThread(HANDLE thread) {
    Thread *t = thread_of(thread);
    if (!t) return DWORD(-1);
    pthread_mutex_lock(&t->mu);
    const DWORD was = static_cast<DWORD>(t->suspend);
    if (t->suspend > 0 && --t->suspend == 0) pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    return was;
}

DWORD SuspendThread(HANDLE) {
    // Stopping a running thread from another one has no equivalent on the console; only diagnostics asked for it.
    SetLastError(ERROR_NOT_SUPPORTED);
    return DWORD(-1);
}

void ExitThread(DWORD code) {
    Thread *t = self_thread();
    pthread_mutex_lock(&t->mu);
    t->exit_code = code;
    pthread_mutex_unlock(&t->mu);
    pthread_exit(nullptr);
}

void ExitProcess(UINT code) { exit(static_cast<int>(code)); }

BOOL TerminateProcess(HANDLE process, UINT code) {
    if (process != pseudo_process() && GetProcessId(process) != GetCurrentProcessId()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    fflush(nullptr);
    _exit(static_cast<int>(code));
}

BOOL GetExitCodeThread(HANDLE thread, LPDWORD code) {
    Thread *t = thread_of(thread);
    if (!t || !code) return FALSE;
    pthread_mutex_lock(&t->mu);
    *code = t->exit_code;
    pthread_mutex_unlock(&t->mu);
    return TRUE;
}

BOOL SetThreadPriority(HANDLE thread, int priority) {
    Thread *t = thread_of(thread);
    if (!t) return FALSE;
    t->priority = priority;
    (void)scePthreadSetprio(t->pt, console_priority(priority));   // best effort, as on Windows without the privilege
    return TRUE;
}

int GetThreadPriority(HANDLE thread) {
    Thread *t = thread_of(thread);
    return t ? t->priority : THREAD_PRIORITY_ERROR_RETURN;
}

BOOL SetThreadPriorityBoost(HANDLE, BOOL) { return TRUE; }   // the console has no dynamic boost to turn off
BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }

DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask) {
    Thread *t = thread_of(thread);
    if (!t) return 0;
    const uint64_t usable = mask & process_mask();
    if (!usable) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint64_t old = 0;
    if (scePthreadGetaffinity(t->pt, &old) != 0) old = process_mask();
    if (scePthreadSetaffinity(t->pt, usable) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return old ? old : process_mask();
}

DWORD SetThreadIdealProcessorEx(HANDLE, PPROCESSOR_NUMBER, PPROCESSOR_NUMBER previous) {
    // A hint the console's scheduler has no call for; the affinity mask is what places threads there.
    if (previous) *previous = PROCESSOR_NUMBER{0, 0, 0};
    return 0;
}

BOOL GetProcessAffinityMask(HANDLE, PDWORD_PTR process, PDWORD_PTR system) {
    if (process) *process = process_mask();
    if (system) *system = process_mask();
    return TRUE;
}

namespace {
void ns_to_filetime(uint64_t ns, LPFILETIME ft) {
    const uint64_t v = ns / 100;
    ft->dwLowDateTime = static_cast<DWORD>(v);
    ft->dwHighDateTime = static_cast<DWORD>(v >> 32);
}
uint64_t thread_cpu_ns(Thread *t) {
    clockid_t id;
    timespec ts{};
    if (t == t_self || (t && t->pt == pthread_self())) {
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
        return 0;
    }
    if (t && pthread_getcpuclockid(t->pt, &id) == 0 && clock_gettime(id, &ts) == 0)
        return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
    return 0;
}
}  // namespace

BOOL GetThreadTimes(HANDLE thread, LPFILETIME creation, LPFILETIME exit_time, LPFILETIME kernel, LPFILETIME user) {
    Thread *t = thread_of(thread);
    if (!t) return FALSE;
    if (creation) ns_to_filetime(0, creation);
    if (exit_time) ns_to_filetime(0, exit_time);
    if (kernel) ns_to_filetime(0, kernel);   // the console reports one CPU time per thread: it is counted as user time
    if (user) ns_to_filetime(thread_cpu_ns(t), user);
    return TRUE;
}

BOOL GetProcessTimes(HANDLE, LPFILETIME creation, LPFILETIME exit_time, LPFILETIME kernel, LPFILETIME user) {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    if (creation) ns_to_filetime(0, creation);
    if (exit_time) ns_to_filetime(0, exit_time);
    if (kernel) ns_to_filetime(0, kernel);
    if (user) ns_to_filetime(uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec), user);
    return TRUE;
}

BOOL QueryThreadCycleTime(HANDLE thread, PULONG64 cycles) {
    Thread *t = thread_of(thread);
    if (!t || !cycles) return FALSE;
    // CPU time in TSC ticks: the unit Windows reports cycle time in.
    const uint64_t ns = thread_cpu_ns(t);
    *cycles = static_cast<ULONG64>(static_cast<unsigned __int128>(ns) * sceKernelGetTscFrequency() / 1000000000ull);
    return TRUE;
}

DWORD GetCurrentProcessorNumber() {
    const int32_t c = sceKernelGetCurrentCpu();
    return c < 0 ? 0 : static_cast<DWORD>(c);
}

void GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER number) {
    if (number) *number = PROCESSOR_NUMBER{0, static_cast<BYTE>(GetCurrentProcessorNumber()), 0};
}

HRESULT SetThreadDescription(HANDLE thread, LPCWSTR name) {
    Thread *t = thread_of(thread);
    if (t && name) pthread_set_name_np(t->pt, utf8_of(name).substr(0, 31).c_str());
    return S_OK;
}

void Sleep(DWORD ms) {
    if (ms == 0) {
        sched_yield();
        return;
    }
    if (ms == INFINITE) for (;;) pause();
    timespec ts{static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

DWORD SleepEx(DWORD ms, BOOL) {
    Sleep(ms);
    return 0;
}

BOOL SwitchToThread() {
    sched_yield();
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (h == pseudo_thread() || h == pseudo_process()) {
        // Waiting for oneself never ends: sleep out the time.
        if (ms != INFINITE) Sleep(ms);
        else for (;;) pause();
        return WAIT_TIMEOUT;
    }
    Object *o = object_of(h);
    if (!o) {
        SetLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    if (o->kind == Object::Kind::Process) {
        // The only process that can be opened is this one, which is running.
        if (ms != INFINITE) Sleep(ms);
        return WAIT_TIMEOUT;
    }
    if (o->kind == Object::Kind::File || o->kind == Object::Kind::Section) {
        SetLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    return static_cast<Waitable *>(o)->wait(ms);
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD ms) {
    // Polled: nothing in the runtime waits on several objects on a hot path.
    const uint64_t limit = ms == INFINITE ? UINT64_MAX : now_ns() + uint64_t(ms) * 1000000ull;
    std::vector<bool> done(count, false);
    for (;;) {
        DWORD ready = 0;
        for (DWORD i = 0; i < count; ++i) {
            if (done[i]) { ready++; continue; }
            const DWORD r = WaitForSingleObject(handles[i], 0);
            if (r == WAIT_FAILED) return WAIT_FAILED;
            if (r == WAIT_OBJECT_0) {
                if (!all) return WAIT_OBJECT_0 + i;
                done[i] = true;
                ready++;
            }
        }
        if (all && ready == count) return WAIT_OBJECT_0;
        if (now_ns() >= limit) return WAIT_TIMEOUT;
        Sleep(1);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// events, timers, mutexes

HANDLE CreateEventW(void *, BOOL manual_reset, BOOL initial, LPCWSTR) { return new Event(manual_reset, initial); }
HANDLE CreateEventA(void *, BOOL manual_reset, BOOL initial, LPCSTR) { return new Event(manual_reset, initial); }

BOOL SetEvent(HANDLE event) {
    auto *e = as<Event>(event, Object::Kind::Event);
    if (!e) return FALSE;
    pthread_mutex_lock(&e->mu);
    e->set = true;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->mu);
    return TRUE;
}

BOOL ResetEvent(HANDLE event) {
    auto *e = as<Event>(event, Object::Kind::Event);
    if (!e) return FALSE;
    pthread_mutex_lock(&e->mu);
    e->set = false;
    pthread_mutex_unlock(&e->mu);
    return TRUE;
}

HANDLE CreateWaitableTimerExW(void *, LPCWSTR, DWORD flags, DWORD) {
    return new Timer((flags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0);
}

HANDLE CreateWaitableTimerW(void *, BOOL manual_reset, LPCWSTR) { return new Timer(manual_reset); }

BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER *due, LONG period, PTIMERAPCROUTINE, LPVOID, BOOL) {
    auto *t = as<Timer>(timer, Object::Kind::Timer);
    if (!t || !due) return FALSE;
    uint64_t at;
    if (due->QuadPart < 0) {
        at = now_ns() + uint64_t(-due->QuadPart) * 100;   // relative, in 100 ns units
    } else {
        // Absolute UTC (FILETIME): its distance from the wall clock now, added to the monotonic clock.
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        const int64_t wall = int64_t((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
        const int64_t delta = (due->QuadPart - wall) * 100;
        at = delta > 0 ? now_ns() + uint64_t(delta) : now_ns();
    }
    pthread_mutex_lock(&t->mu);
    t->due_ns = at;
    t->period_ns = period > 0 ? uint64_t(period) * 1000000ull : 0;
    t->armed = true;
    t->fired = false;
    pthread_mutex_unlock(&t->mu);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE timer) {
    auto *t = as<Timer>(timer, Object::Kind::Timer);
    if (!t) return FALSE;
    pthread_mutex_lock(&t->mu);
    t->armed = false;
    pthread_mutex_unlock(&t->mu);
    return TRUE;
}

HANDLE CreateMutexA(void *, BOOL initial_owner, LPCSTR name) {
    Mutex *m = nullptr;
    if (name && *name) {
        std::lock_guard lk(g_names_mu);
        auto it = g_named.find(name);
        if (it != g_named.end()) {
            retain(it->second);
            SetLastError(ERROR_ALREADY_EXISTS);
            return it->second;
        }
        m = new Mutex;
        g_named[name] = m;
    } else {
        m = new Mutex;
    }
    if (initial_owner) {
        pthread_mutex_lock(&m->mu);
        m->try_take();
        pthread_mutex_unlock(&m->mu);
    }
    SetLastError(ERROR_SUCCESS);
    return m;
}

HANDLE CreateMutexW(void *security, BOOL initial_owner, LPCWSTR name) {
    const std::string n = utf8_of(name);
    return CreateMutexA(security, initial_owner, name ? n.c_str() : nullptr);
}

HANDLE CreateMutexW(void *security, BOOL initial_owner, const char *name) {
    return CreateMutexA(security, initial_owner, name);
}

BOOL ReleaseMutex(HANDLE mutex) {
    auto *m = as<Mutex>(mutex, Object::Kind::Mutex);
    if (!m) return FALSE;
    pthread_mutex_lock(&m->mu);
    const bool mine = m->count > 0 && m->owner == current_tid();
    if (mine && --m->count == 0) {
        m->owner = 0;
        pthread_cond_broadcast(&m->cv);
    }
    pthread_mutex_unlock(&m->mu);
    if (!mine) SetLastError(ERROR_ACCESS_DENIED);   // ERROR_NOT_OWNER on Windows
    return mine;
}

// ---------------------------------------------------------------------------------------------------------------------
// thread-local storage

DWORD TlsAlloc() {
    pthread_key_t k;
    if (pthread_key_create(&k, nullptr) != 0) return TLS_OUT_OF_INDEXES;
    return static_cast<DWORD>(k);
}
BOOL TlsFree(DWORD index) { return pthread_key_delete(static_cast<pthread_key_t>(index)) == 0; }
LPVOID TlsGetValue(DWORD index) {
    SetLastError(ERROR_SUCCESS);
    return pthread_getspecific(static_cast<pthread_key_t>(index));
}
BOOL TlsSetValue(DWORD index, LPVOID value) { return pthread_setspecific(static_cast<pthread_key_t>(index), value) == 0; }

// ---------------------------------------------------------------------------------------------------------------------
// SRW locks, condition variables, critical sections, address waits

void InitializeSRWLock(PSRWLOCK lock) { lock->Ptr = nullptr; }
void AcquireSRWLockExclusive(PSRWLOCK lock) { srw_lock_exclusive(lock); }
void ReleaseSRWLockExclusive(PSRWLOCK lock) { srw_unlock_exclusive(lock); }
void AcquireSRWLockShared(PSRWLOCK lock) { srw_lock_shared(lock); }
void ReleaseSRWLockShared(PSRWLOCK lock) { srw_unlock_shared(lock); }
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock) { return srw_try_exclusive(srw_word(lock)); }
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock) { return srw_try_shared(srw_word(lock)); }

// Condition variable word (CONDITION_VARIABLE::Ptr): a wake sequence number, zero at CONDITION_VARIABLE_INIT. A
// sleeper notes it, releases the lock, and waits for it to change; a wake advances it. WakeConditionVariable wakes every
// sleeper (Windows allows spurious wakes, and every caller re-checks its predicate in a loop).
void InitializeConditionVariable(PCONDITION_VARIABLE cv) { cv->Ptr = nullptr; }

BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags) {
    auto &seq = *reinterpret_cast<std::atomic<uintptr_t> *>(&cv->Ptr);
    const uintptr_t seen = seq.load(std::memory_order_seq_cst);
    const bool shared = (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) != 0;
    shared ? srw_unlock_shared(lock) : srw_unlock_exclusive(lock);
    const timespec until = deadline_after_ms(ms);
    const bool woke = park(cv, [&] { return seq.load(std::memory_order_seq_cst) != seen; },
                           ms == INFINITE ? nullptr : &until);
    shared ? srw_lock_shared(lock) : srw_lock_exclusive(lock);
    if (!woke) {
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

void WakeConditionVariable(PCONDITION_VARIABLE cv) { WakeAllConditionVariable(cv); }
void WakeAllConditionVariable(PCONDITION_VARIABLE cv) {
    reinterpret_cast<std::atomic<uintptr_t> *>(&cv->Ptr)->fetch_add(1, std::memory_order_seq_cst);
    unpark_all(cv);
}

// Critical sections are recursive: the mutex is made with that type on first use.
static pthread_mutex_t *cs_mutex(LPCRITICAL_SECTION cs) {
    auto *m = static_cast<pthread_mutex_t *>(__atomic_load_n(&cs->Ptr, __ATOMIC_ACQUIRE));
    if (m) return m;
    auto *fresh = new pthread_mutex_t;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(fresh, &a);
    pthread_mutexattr_destroy(&a);
    void *expected = nullptr;
    if (__atomic_compare_exchange_n(&cs->Ptr, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return fresh;
    pthread_mutex_destroy(fresh);
    delete fresh;
    return static_cast<pthread_mutex_t *>(expected);
}
void InitializeCriticalSection(LPCRITICAL_SECTION cs) {
    cs->Ptr = nullptr;
    cs_mutex(cs);
}
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD) {
    InitializeCriticalSection(cs);
    return TRUE;
}
void DeleteCriticalSection(LPCRITICAL_SECTION cs) {
    if (auto *m = static_cast<pthread_mutex_t *>(cs->Ptr)) {
        pthread_mutex_destroy(m);
        delete m;
    }
    cs->Ptr = nullptr;
}
void EnterCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_lock(cs_mutex(cs)); }
void LeaveCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_unlock(cs_mutex(cs)); }
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) { return pthread_mutex_trylock(cs_mutex(cs)) == 0; }

BOOL WaitOnAddress(volatile void *address, void *compare, SIZE_T size, DWORD ms) {
    // Returns when the value differs from *compare (or the time runs out); the caller re-checks, as on Windows, where a
    // wake may also be spurious. Writers call WakeByAddress* after changing the value, which ends the park.
    const timespec until = deadline_after_ms(ms);
    const bool changed = park(address, [&] {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return memcmp(const_cast<void *>(address), compare, size) != 0;
    }, ms == INFINITE ? nullptr : &until);
    if (!changed) {
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

void WakeByAddressSingle(void *address) { WakeByAddressAll(address); }   // every waiter of the bucket re-checks
void WakeByAddressAll(void *address) { unpark_all(address); }

// ---------------------------------------------------------------------------------------------------------------------
// time

// QueryPerformanceCounter ticks at 10 MHz, the frequency Windows reports on current hardware, so arithmetic the runtime
// does with it has the headroom it has on Windows.
BOOL QueryPerformanceCounter(LARGE_INTEGER *count) {
    count->QuadPart = static_cast<LONGLONG>(now_ns() / 100);
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency) {
    frequency->QuadPart = 10000000;
    return TRUE;
}
ULONGLONG GetTickCount64() { return now_ns() / 1000000ull; }
DWORD GetTickCount() { return static_cast<DWORD>(GetTickCount64()); }
DWORD timeGetTime() { return static_cast<DWORD>(GetTickCount64()); }

static constexpr uint64_t kEpochDelta = 11644473600ull;   // seconds from 1601-01-01 to 1970-01-01

void GetSystemTimePreciseAsFileTime(LPFILETIME time) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t v = (uint64_t(ts.tv_sec) + kEpochDelta) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
    time->dwLowDateTime = static_cast<DWORD>(v);
    time->dwHighDateTime = static_cast<DWORD>(v >> 32);
}
void GetSystemTimeAsFileTime(LPFILETIME time) { GetSystemTimePreciseAsFileTime(time); }

static void tm_to_systemtime(const struct tm &t, WORD ms, LPSYSTEMTIME st) {
    st->wYear = static_cast<WORD>(t.tm_year + 1900);
    st->wMonth = static_cast<WORD>(t.tm_mon + 1);
    st->wDayOfWeek = static_cast<WORD>(t.tm_wday);
    st->wDay = static_cast<WORD>(t.tm_mday);
    st->wHour = static_cast<WORD>(t.tm_hour);
    st->wMinute = static_cast<WORD>(t.tm_min);
    st->wSecond = static_cast<WORD>(t.tm_sec);
    st->wMilliseconds = ms;
}

BOOL FileTimeToSystemTime(const FILETIME *file_time, LPSYSTEMTIME system_time) {
    const uint64_t v = (uint64_t(file_time->dwHighDateTime) << 32) | file_time->dwLowDateTime;
    const uint64_t secs = v / 10000000ull;
    if (secs < kEpochDelta) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const time_t unix_secs = static_cast<time_t>(secs - kEpochDelta);
    struct tm t;
    gmtime_r(&unix_secs, &t);
    tm_to_systemtime(t, static_cast<WORD>((v / 10000ull) % 1000), system_time);
    return TRUE;
}

BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME file_time) {
    struct tm t{};
    t.tm_year = st->wYear - 1900;
    t.tm_mon = st->wMonth - 1;
    t.tm_mday = st->wDay;
    t.tm_hour = st->wHour;
    t.tm_min = st->wMinute;
    t.tm_sec = st->wSecond;
    const time_t secs = timegm(&t);
    const uint64_t v = (uint64_t(secs) + kEpochDelta) * 10000000ull + uint64_t(st->wMilliseconds) * 10000ull;
    file_time->dwLowDateTime = static_cast<DWORD>(v);
    file_time->dwHighDateTime = static_cast<DWORD>(v >> 32);
    return TRUE;
}

void GetSystemTime(LPSYSTEMTIME time) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    gmtime_r(&ts.tv_sec, &t);
    tm_to_systemtime(t, static_cast<WORD>(ts.tv_nsec / 1000000), time);
}

void GetLocalTime(LPSYSTEMTIME time) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    tm_to_systemtime(t, static_cast<WORD>(ts.tv_nsec / 1000000), time);
}

// ---------------------------------------------------------------------------------------------------------------------
// strings

int MultiByteToWideChar(UINT, DWORD flags, LPCSTR in, int in_len, LPWSTR out, int out_len) {
    if (!in || in_len == 0 || out_len < 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const size_t n = in_len < 0 ? strlen(in) + 1 : static_cast<size_t>(in_len);   // -1 counts the terminator
    const auto *s = reinterpret_cast<const unsigned char *>(in);
    std::vector<wchar_t> tmp;
    tmp.reserve(n);
    for (size_t i = 0; i < n;) {
        uint32_t c = s[i];
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        bool bad = extra < 0 || i + size_t(extra) >= n + (extra == 0 ? 1 : 0);
        if (!bad && extra > 0) {
            if (i + size_t(extra) >= n + 1) bad = true;
            c &= 0x3Fu >> extra;
            for (int k = 1; k <= extra && !bad; ++k) {
                if (i + size_t(k) >= n || (s[i + k] & 0xC0) != 0x80) bad = true;
                else c = c << 6 | (s[i + k] & 0x3F);
            }
            if (!bad && ((extra == 1 && c < 0x80) || (extra == 2 && c < 0x800) || (extra == 3 && c < 0x10000) ||
                         c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)))
                bad = true;
        }
        if (bad) {
            if (flags & MB_ERR_INVALID_CHARS) {
                SetLastError(ERROR_NO_UNICODE_TRANSLATION);
                return 0;
            }
            tmp.push_back(L'\xFFFD');
            ++i;
            continue;
        }
        tmp.push_back(static_cast<wchar_t>(c));
        i += size_t(extra) + 1;
    }
    if (out_len == 0) return static_cast<int>(tmp.size());
    if (tmp.size() > size_t(out_len)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::copy(tmp.begin(), tmp.end(), out);
    return static_cast<int>(tmp.size());
}

int WideCharToMultiByte(UINT, DWORD flags, LPCWSTR in, int in_len, LPSTR out, int out_len, LPCSTR, BOOL *used_def) {
    if (!in || in_len == 0 || out_len < 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (used_def) *used_def = FALSE;
    const size_t n = in_len < 0 ? wcslen(in) + 1 : static_cast<size_t>(in_len);
    std::string tmp;
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = static_cast<uint32_t>(in[i]);
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
            if (flags & WC_ERR_INVALID_CHARS) {
                SetLastError(ERROR_NO_UNICODE_TRANSLATION);
                return 0;
            }
            c = 0xFFFD;
        }
        const wchar_t one[2] = {static_cast<wchar_t>(c), 0};
        if (c == 0) tmp.push_back('\0');
        else tmp += utf8_of(one);
    }
    if (out_len == 0) return static_cast<int>(tmp.size());
    if (tmp.size() > size_t(out_len)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(out, tmp.data(), tmp.size());
    return static_cast<int>(tmp.size());
}

FILE *_wfopen(const char *path, const wchar_t *mode) { return fopen(native_path(path).c_str(), utf8_of(mode).c_str()); }
FILE *_wfopen(const wchar_t *path, const wchar_t *mode) { return fopen(native_path(utf8_of(path).c_str()).c_str(), utf8_of(mode).c_str()); }

LANGID GetUserDefaultUILanguage() { return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US); }
LANGID GetUserDefaultLangID() { return GetUserDefaultUILanguage(); }
DWORD GetGuiResources(HANDLE, DWORD) { return 0; }

NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE, uint8_t *buffer, ULONG size, ULONG) {
    arc4random_buf(buffer, size);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// processor topology: the title's CPUs, paired into cores (Zen 2: logical CPUs 2k and 2k+1 share a core)

DWORD GetActiveProcessorCount(WORD) { return static_cast<DWORD>(__builtin_popcountll(process_mask())); }

BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP rel, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buf,
                                      PDWORD length) {
    if (!length) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::vector<uint64_t> cores;
    if (rel == RelationProcessorCore || rel == RelationAll) {
        const uint64_t m = process_mask();
        for (int k = 0; k < 64; k += 2)
            if (const uint64_t pair = m & (3ull << k)) cores.push_back(pair);
    }
    const DWORD rec = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
    const DWORD need = rec * static_cast<DWORD>(cores.size());
    if (!buf || *length < need) {
        *length = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    auto *p = reinterpret_cast<uint8_t *>(buf);
    for (uint64_t pair : cores) {
        auto *e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(p);
        memset(e, 0, rec);
        e->Relationship = RelationProcessorCore;
        e->Size = rec;
        e->Processor.Flags = __builtin_popcountll(pair) > 1 ? 1 : 0;   // LTP_PC_SMT
        e->Processor.GroupCount = 1;
        e->Processor.GroupMask[0].Mask = pair;
        p += rec;
    }
    *length = need;
    return TRUE;
}

// ---------------------------------------------------------------------------------------------------------------------
// debugging

void OutputDebugStringA(LPCSTR text) {
    if (text) fputs(text, stderr);
}
void OutputDebugStringW(LPCWSTR text) { OutputDebugStringA(utf8_of(text).c_str()); }
BOOL IsDebuggerPresent() { return FALSE; }
void DebugBreak() { __builtin_debugtrap(); }

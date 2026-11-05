// SPDX-License-Identifier: GPL-3.0-or-later
// Every thread the title starts: the IEEE floating-point state, and C++ thread_local destructors on the console.
//
// The C++ runtime registers them through __cxa_thread_atexit_impl, which the console's C library does not export: the
// module converter refuses a title that imports it ("no public SDK stub exports required symbol"). This defines it.
// The destructors must run when the thread's function returns, before libkernel's own key destructors free the thread's
// storage (the console's compiler emulates TLS, so that storage holds the thread_local objects): KongaTime's PS5_RPCS3
// libre layer measured that run after them they used freed memory (ps5/libre/threads/threads.c, which this follows).
// So the title's link wraps pthread_create and pthread_exit (ps5/CMakeLists.txt, ps5_title): a thread runs its function
// through a trampoline that runs the destructors after it, and pthread_exit runs them first. A thread that ends some
// other way still runs them, from a pthread key destructor (the fallback, in no fixed order with libkernel's).
#include <ps5platform/heap.h>

#include <atomic>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <stdlib.h>
#include <vector>

namespace {

struct Destructor {
    void (*run)(void *);
    void *object;
    Destructor *next;
};

pthread_key_t g_key;
pthread_once_t g_once = PTHREAD_ONCE_INIT;

void run_list(void *head);

void make_key() { pthread_key_create(&g_key, run_list); }

// This thread's destructors, newest first; one may register more, which run too.
void run_destructors() {
    pthread_once(&g_once, make_key);
    for (Destructor *d; (d = static_cast<Destructor *>(pthread_getspecific(g_key)));) {
        pthread_setspecific(g_key, d->next);
        d->run(d->object);
        free(d);
    }
}

void run_list(void *head) {
    pthread_setspecific(g_key, head);   // the key is cleared before its destructor runs: put the list back to walk it
    run_destructors();
}

struct Start {
    void *(*fn)(void *);
    void *arg;
    void *stack;   // ours (direct memory), retired when the thread ends
};

// Thread stacks in direct memory. pthread's own come from flexible memory, of which the console gives a title a few
// hundred MiB in all: with 8 MiB stacks (RADV's shader compiler needs them) the sixth P3 run had 34 MiB of it left at
// 5 s and ended in abort() at 7.5 s. A stack cannot be freed by the thread running on it, so an ended thread's stack is
// retired with a time, and freed by a later pthread_create once a second has passed (the thread is gone by then).
struct Retired {
    void *stack;
    time_t at;
};
std::mutex g_retired_mu;
std::vector<Retired> *g_retired;

void retire(void *stack) {
    if (!stack) return;
    std::lock_guard lk(g_retired_mu);
    if (!g_retired) g_retired = new std::vector<Retired>;
    g_retired->push_back({stack, time(nullptr)});
}
void free_retired() {
    std::lock_guard lk(g_retired_mu);
    if (!g_retired) return;
    const time_t now = time(nullptr);
    for (size_t i = 0; i < g_retired->size();) {
        if (now - (*g_retired)[i].at >= 2) {
            ps5_heap_free((*g_retired)[i].stack);
            (*g_retired)[i] = g_retired->back();
            g_retired->pop_back();
        } else {
            ++i;
        }
    }
}
thread_local void *t_stack = nullptr;

// The console starts threads with flush-to-zero and denormals-are-zero set in MXCSR (bits 15 and 6), as it starts a title;
// _start clears them for the main thread only (title/fp_ieee.cpp). Code built here, the lifted guest code above all,
// expects IEEE denormals: the second console run's 03_fp and 05_neon_fp_mem got -0.0 where AArch64 gives a denormal.
void fp_ieee() {
    unsigned int control;
    __asm__ volatile("stmxcsr %0" : "=m"(control));
    control &= ~0x8040u;
    __asm__ volatile("ldmxcsr %0" : : "m"(control));
}

void *trampoline(void *p) {
    const Start s = *static_cast<Start *>(p);
    free(p);
    fp_ieee();
    t_stack = s.stack;
    void *result = s.fn(s.arg);
    run_destructors();
    retire(t_stack);
    return result;
}

}  // namespace

extern "C" {

int __cxa_thread_atexit_impl(void (*destructor)(void *), void *object, void *) {
    pthread_once(&g_once, make_key);
    auto *d = static_cast<Destructor *>(malloc(sizeof(Destructor)));
    if (!d) return -1;
    *d = Destructor{destructor, object, static_cast<Destructor *>(pthread_getspecific(g_key))};
    if (pthread_setspecific(g_key, d) != 0) {
        free(d);
        return -1;
    }
    return 0;
}

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    auto *s = static_cast<Start *>(malloc(sizeof(Start)));
    if (!s) return 12;   // ENOMEM
    *s = Start{fn, arg, nullptr};
    // A thread that does not choose its stack (std::thread passes no attributes) gets 8 MiB, Linux's default: the
    // console's default is far smaller, and the first P3 run's render thread overflowed it inside RADV's shader
    // compiler (nir_opt_varyings, from vkCreateGraphicsPipelines). Threads with their own stack (the compat layer's,
    // ps5_vrange) or an explicit size of 8 MiB or more keep theirs.
    constexpr size_t kStack = size_t(8) << 20;
    free_retired();
    pthread_attr_t local;
    pthread_attr_t *use = const_cast<pthread_attr_t *>(attr);
    bool own = false;
    if (!attr) {
        if (pthread_attr_init(&local) == 0) {
            use = &local;
            own = true;
        }
    }
    s->stack = nullptr;
    if (use) {
        void *addr = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(use, &addr, &size) == 0 && !addr) {   // no stack of its own: one in direct memory
            const size_t want = size > kStack ? size : kStack;
            if (void *st = ps5_heap_memalign(16384, want)) {
                if (pthread_attr_setstack(use, st, want) == 0) s->stack = st;
                else ps5_heap_free(st);
            }
            if (!s->stack && size < kStack) pthread_attr_setstacksize(use, kStack);
        }
    }
    const int e = __real_pthread_create(thread, use, trampoline, s);
    if (own) pthread_attr_destroy(&local);
    if (e != 0) {
        if (s->stack) ps5_heap_free(s->stack);
        free(s);
    }
    return e;
}

[[noreturn]] void __real_pthread_exit(void *value);
[[noreturn]] void __wrap_pthread_exit(void *value) {
    run_destructors();
    retire(t_stack);
    __real_pthread_exit(value);
}

}  // extern "C"

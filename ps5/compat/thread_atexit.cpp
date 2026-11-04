// SPDX-License-Identifier: GPL-3.0-or-later
// C++ thread_local destructors on the console.
//
// The C++ runtime registers them through __cxa_thread_atexit_impl, which the console's C library does not export: the
// module converter refuses a title that imports it ("no public SDK stub exports required symbol"). This defines it.
// The destructors must run when the thread's function returns, before libkernel's own key destructors free the thread's
// storage (the console's compiler emulates TLS, so that storage holds the thread_local objects): KongaTime's PS5_RPCS3
// libre layer measured that run after them they used freed memory (ps5/libre/threads/threads.c, which this follows).
// So the title's link wraps pthread_create and pthread_exit (ps5/CMakeLists.txt, ps5_title): a thread runs its function
// through a trampoline that runs the destructors after it, and pthread_exit runs them first. A thread that ends some
// other way still runs them, from a pthread key destructor (the fallback, in no fixed order with libkernel's).
#include <pthread.h>
#include <stdlib.h>

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
};

void *trampoline(void *p) {
    const Start s = *static_cast<Start *>(p);
    free(p);
    void *result = s.fn(s.arg);
    run_destructors();
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
    *s = Start{fn, arg};
    const int e = __real_pthread_create(thread, attr, trampoline, s);
    if (e != 0) free(s);
    return e;
}

[[noreturn]] void __real_pthread_exit(void *value);
[[noreturn]] void __wrap_pthread_exit(void *value) {
    run_destructors();
    __real_pthread_exit(value);
}

}  // extern "C"

// SPDX-License-Identifier: GPL-3.0-or-later
// RADV's C11 thread calls (mtx_*, cnd_*, thrd_*) resolve to the console libc's <threads.h> layer: the driver puts -lc
// ahead of RADV's archive, so libc's members are taken before Mesa's own threads_posix.c.o. The two disagree about the
// return codes:
//
//   console <threads.h> (FreeBSD):  thrd_busy 1, thrd_error 2, thrd_nomem 3, thrd_success 4, thrd_timedout 5
//   Mesa's c11/threads.h:           thrd_success 0, thrd_timedout 1, thrd_error 2, thrd_busy 3, thrd_nomem 4
//
// so every "this failed" check in Mesa fires on success. The first P3 run ended there: vk_instance.c:187 tests
// mtx_init(...) != 0 and RADV returned VK_ERROR_INITIALIZATION_FAILED. The title link wraps the calls that answer with
// one of those codes (ps5/radv.cmake) and translates the answer, as KongaTime's The-Darkness-Recomp and Monado-PS5 do
// (their posix_compat.cpp). mtx_plain / mtx_recursive / mtx_timed have the same values in both headers.
#include <time.h>

namespace {
int mesa_code(int console) {
    switch (console) {
        case 4: return 0;   // thrd_success
        case 5: return 1;   // thrd_timedout
        case 2: return 2;   // thrd_error
        case 1: return 3;   // thrd_busy
        case 3: return 4;   // thrd_nomem
        default: return console;
    }
}
}  // namespace

extern "C" {
int __real_mtx_init(void* m, int type);
int __real_mtx_lock(void* m);
int __real_mtx_timedlock(void* m, const struct timespec* until);
int __real_mtx_trylock(void* m);
int __real_mtx_unlock(void* m);
int __real_cnd_init(void* c);
int __real_cnd_wait(void* c, void* m);
int __real_cnd_timedwait(void* c, void* m, const struct timespec* until);
int __real_cnd_signal(void* c);
int __real_cnd_broadcast(void* c);
int __real_thrd_create(void* t, int (*start)(void*), void* arg);
int __real_thrd_join(void* t, int* result);
int __real_thrd_detach(void* t);

int __wrap_mtx_init(void* m, int type) { return mesa_code(__real_mtx_init(m, type)); }
int __wrap_mtx_lock(void* m) { return mesa_code(__real_mtx_lock(m)); }
int __wrap_mtx_timedlock(void* m, const struct timespec* until) { return mesa_code(__real_mtx_timedlock(m, until)); }
int __wrap_mtx_trylock(void* m) { return mesa_code(__real_mtx_trylock(m)); }
int __wrap_mtx_unlock(void* m) { return mesa_code(__real_mtx_unlock(m)); }
int __wrap_cnd_init(void* c) { return mesa_code(__real_cnd_init(c)); }
int __wrap_cnd_wait(void* c, void* m) { return mesa_code(__real_cnd_wait(c, m)); }
int __wrap_cnd_timedwait(void* c, void* m, const struct timespec* until) { return mesa_code(__real_cnd_timedwait(c, m, until)); }
int __wrap_cnd_signal(void* c) { return mesa_code(__real_cnd_signal(c)); }
int __wrap_cnd_broadcast(void* c) { return mesa_code(__real_cnd_broadcast(c)); }
int __wrap_thrd_create(void* t, int (*start)(void*), void* arg) { return mesa_code(__real_thrd_create(t, start, arg)); }
int __wrap_thrd_join(void* t, int* result) { return mesa_code(__real_thrd_join(t, result)); }
int __wrap_thrd_detach(void* t) { return mesa_code(__real_thrd_detach(t)); }
}  // extern "C"

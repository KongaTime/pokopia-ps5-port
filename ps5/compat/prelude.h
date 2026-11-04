// SPDX-License-Identifier: GPL-3.0-or-later
// Force-included into every runtime file of the PS5 build (-include ps5/compat/prelude.h).
//
// The SDK's libc++ predates std::atomic_ref (its <version> leaves __cpp_lib_atomic_ref commented out), which the
// runtime uses on guest shared memory (services/hid.cpp: the sampling rings). This is the subset the runtime calls, on
// the same compiler builtins libc++'s own atomic_ref is built on; it is used only where the library has none.
#pragma once

// Microsoft CRT names the runtime calls without including <windows.h> (services/audio_wav.cpp opens its file with
// _wfopen), implemented in ps5/compat/win32_ps5.cpp.
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
FILE *_wfopen(const wchar_t *path, const wchar_t *mode);
#ifdef __cplusplus
FILE *_wfopen(const char *path, const wchar_t *mode);   // a std::filesystem::path's c_str(), which is char here
#endif
static inline unsigned long long _strtoui64(const char *s, char **end, int base) { return strtoull(s, end, base); }
static inline long long _strtoi64(const char *s, char **end, int base) { return strtoll(s, end, base); }

#ifdef __cplusplus
#include <atomic>
#include <type_traits>

#if !defined(__cpp_lib_atomic_ref)
namespace std {
template <class T>
class atomic_ref {
    static_assert(is_trivially_copyable_v<T>, "atomic_ref needs a trivially copyable type");
    T* ptr_;
    static constexpr int order(memory_order m) {
        return m == memory_order_relaxed ? __ATOMIC_RELAXED
             : m == memory_order_consume ? __ATOMIC_CONSUME
             : m == memory_order_acquire ? __ATOMIC_ACQUIRE
             : m == memory_order_release ? __ATOMIC_RELEASE
             : m == memory_order_acq_rel ? __ATOMIC_ACQ_REL
                                         : __ATOMIC_SEQ_CST;
    }
    // The order a failed compare-exchange may use: no release part (as the standard derives it).
    static constexpr int fail_order(memory_order m) {
        return m == memory_order_acq_rel ? __ATOMIC_ACQUIRE : m == memory_order_release ? __ATOMIC_RELAXED : order(m);
    }

public:
    using value_type = T;
    static constexpr size_t required_alignment = alignof(T);
    static constexpr bool is_always_lock_free = __atomic_always_lock_free(sizeof(T), 0);

    explicit atomic_ref(T& obj) noexcept : ptr_(&obj) {}
    atomic_ref(const atomic_ref&) noexcept = default;
    atomic_ref& operator=(const atomic_ref&) = delete;

    bool is_lock_free() const noexcept { return __atomic_is_lock_free(sizeof(T), ptr_); }
    void store(T v, memory_order m = memory_order_seq_cst) const noexcept { __atomic_store(ptr_, &v, order(m)); }
    T operator=(T v) const noexcept { store(v); return v; }
    T load(memory_order m = memory_order_seq_cst) const noexcept {
        T v;
        __atomic_load(ptr_, &v, order(m));
        return v;
    }
    operator T() const noexcept { return load(); }
    T exchange(T v, memory_order m = memory_order_seq_cst) const noexcept {
        T old;
        __atomic_exchange(ptr_, &v, &old, order(m));
        return old;
    }
    bool compare_exchange_weak(T& expected, T desired, memory_order m = memory_order_seq_cst) const noexcept {
        return __atomic_compare_exchange(ptr_, &expected, &desired, true, order(m), fail_order(m));
    }
    bool compare_exchange_strong(T& expected, T desired, memory_order m = memory_order_seq_cst) const noexcept {
        return __atomic_compare_exchange(ptr_, &expected, &desired, false, order(m), fail_order(m));
    }
    bool compare_exchange_weak(T& expected, T desired, memory_order s, memory_order f) const noexcept {
        return __atomic_compare_exchange(ptr_, &expected, &desired, true, order(s), order(f));
    }
    bool compare_exchange_strong(T& expected, T desired, memory_order s, memory_order f) const noexcept {
        return __atomic_compare_exchange(ptr_, &expected, &desired, false, order(s), order(f));
    }

    template <class U = T, enable_if_t<is_integral_v<U> && !is_same_v<U, bool>, int> = 0>
    T fetch_add(T v, memory_order m = memory_order_seq_cst) const noexcept { return __atomic_fetch_add(ptr_, v, order(m)); }
    template <class U = T, enable_if_t<is_integral_v<U> && !is_same_v<U, bool>, int> = 0>
    T fetch_sub(T v, memory_order m = memory_order_seq_cst) const noexcept { return __atomic_fetch_sub(ptr_, v, order(m)); }
    template <class U = T, enable_if_t<is_integral_v<U> && !is_same_v<U, bool>, int> = 0>
    T fetch_and(T v, memory_order m = memory_order_seq_cst) const noexcept { return __atomic_fetch_and(ptr_, v, order(m)); }
    template <class U = T, enable_if_t<is_integral_v<U> && !is_same_v<U, bool>, int> = 0>
    T fetch_or(T v, memory_order m = memory_order_seq_cst) const noexcept { return __atomic_fetch_or(ptr_, v, order(m)); }
    template <class U = T, enable_if_t<is_integral_v<U> && !is_same_v<U, bool>, int> = 0>
    T fetch_xor(T v, memory_order m = memory_order_seq_cst) const noexcept { return __atomic_fetch_xor(ptr_, v, order(m)); }
};
}  // namespace std
#endif
#endif

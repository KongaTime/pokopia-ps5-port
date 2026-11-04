// SPDX-License-Identifier: GPL-3.0-or-later
// The libc calls a title is refused or does not have, bound to the SDK platform layer's replacements (ps5platform/libc.h),
// by the title link's --wrap (ps5/CMakeLists.txt, ps5_title). The header records, per function, what the console does
// to a title: the directory streams and access() are refused, the *at calls and statfs resolve to nothing, statvfs
// faults, getcwd faults, sysconf answers 16 CPUs for 13. libc++'s std::filesystem, which the runtime's file services use
// (fs_romfs.cpp's RomFS index, fs_volume.cpp's saves), is built on them: the sixth P2 run ended in a file-system
// request (fsp-srv, svc SendSyncRequest) with SYSTEM_ILLEGAL_FUNCTION_CALL.
//
// Each __wrap_X calls ps5_X. realpath keeps title/posix_compat.cpp's wrapper (which also resolves lexically).
#include <ps5platform/libc.h>

#include <cstdarg>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>

// compat/win32_memory.cpp's, in titles that link the compat layer (weak: the others have no sections to back).
namespace ps5compat {
void touch(const void* p, size_t n) __attribute__((weak));
}
static inline void touch(const void* p, size_t n) {
    if (ps5compat::touch) ps5compat::touch(p, n);
}

extern "C" {

DIR* __wrap_opendir(const char* path) { return ps5_opendir(path); }
DIR* __wrap_fdopendir(int fd) { return ps5_fdopendir(fd); }
struct dirent* __wrap_readdir(DIR* d) { return ps5_readdir(d); }
void __wrap_rewinddir(DIR* d) { ps5_rewinddir(d); }
int __wrap_dirfd(DIR* d) { return ps5_dirfd(d); }
int __wrap_closedir(DIR* d) { return ps5_closedir(d); }

int __wrap_openat(int dir, const char* name, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list a;
        va_start(a, flags);
        mode = static_cast<mode_t>(va_arg(a, int));
        va_end(a);
    }
    return ps5_openat(dir, name, flags, mode);
}
int __wrap_unlinkat(int dir, const char* name, int flags) { return ps5_unlinkat(dir, name, flags); }
int __wrap_fchmodat(int dir, const char* name, mode_t mode, int flags) { return ps5_fchmodat(dir, name, mode, flags); }
int __wrap_fstatat(int dir, const char* name, struct stat* st, int flags) { return ps5_fstatat(dir, name, st, flags); }
int __wrap_mkdirat(int dir, const char* name, mode_t mode) { return ps5_mkdirat(dir, name, mode); }
int __wrap_renameat(int from_dir, const char* from, int to_dir, const char* to) { return ps5_renameat(from_dir, from, to_dir, to); }

int __wrap_statvfs(const char* path, struct statvfs* r) { return ps5_statvfs(path, r); }
int __wrap_fstatvfs(int fd, struct statvfs* r) { return ps5_fstatvfs(fd, r); }
int __wrap_statfs(const char* path, void* r) { return ps5_statfs(path, r); }
int __wrap_fstatfs(int fd, void* r) { return ps5_fstatfs(fd, r); }

int __wrap_access(const char* path, int mode) { return ps5_access(path, mode); }
char* __wrap_getcwd(char* buffer, size_t size) { return ps5_getcwd(buffer, size); }
int __wrap_utimensat(int dir, const char* path, const struct timespec times[2], int flags) { return ps5_utimensat(dir, path, times, flags); }
int __wrap_futimens(int fd, const struct timespec times[2]) { return ps5_futimens(fd, times); }
// ps5_sysconf answers some names itself and passes the rest to sysconf, which this same --wrap sends back here: the
// first P3 run (RADV asking for a page size) recursed until the stack ran out. The inner call goes to libc's.
long __real_sysconf(int name);
long __wrap_sysconf(int name) {
    static thread_local bool inside = false;
    if (inside) return __real_sysconf(name);
    inside = true;
    const long r = ps5_sysconf(name);
    inside = false;
    return r;
}
int __wrap_pthread_getaffinity_np(pthread_t t, size_t size, void* set) { return ps5_pthread_getaffinity_np(t, size, set); }
int __wrap_pthread_setaffinity_np(pthread_t t, size_t size, const void* set) { return ps5_pthread_setaffinity_np(t, size, set); }
int __wrap_mkstemp(char* path_template) { return ps5_mkstemp(path_template); }
FILE* __wrap_tmpfile(void) { return ps5_tmpfile(); }
ssize_t __wrap_readlink(const char* path, char* buffer, size_t size) { return ps5_readlink(path, buffer, size); }
int __wrap_link(const char* existing, const char* name) { return ps5_link(existing, name); }
int __wrap_symlink(const char* target, const char* name) { return ps5_symlink(target, name); }
mode_t __wrap_umask(mode_t mask) { return ps5_umask(mask); }

// The SDK's libc links these as raw system call stubs (mov $n, %rax; syscall), and a title may make no system call
// itself: the seventh P2 run's klog says "PPRBUG-22859: the process directly issued a syscall 244" (clock_nanosleep,
// from compat's waitable timer) before SYSTEM_ILLEGAL_FUNCTION_CALL. These three are the raw stubs anything in the title
// calls (ps5/tools/rawsys.sh scans the linked image for them).
int __wrap_clock_nanosleep(clockid_t clock, int flags, const struct timespec* request, struct timespec* remain) {
    return ps5_clock_nanosleep(clock, flags, request, remain);
}
long __wrap_pathconf(const char* path, int name) { return ps5_pathconf(path, name); }
pid_t __wrap_fork(void) { return ps5_fork(); }

// Committed section memory (the guest heap) is backed on first touch through the fault handler, which the kernel does
// not raise for a buffer handed to it: read() into an untouched page fails with EFAULT. These back the buffer first.
ssize_t __real_read(int fd, void* buffer, size_t size);
ssize_t __real_pread(int fd, void* buffer, size_t size, off_t offset);
ssize_t __real_write(int fd, const void* buffer, size_t size);
ssize_t __real_pwrite(int fd, const void* buffer, size_t size, off_t offset);
size_t __real_fread(void* buffer, size_t size, size_t count, FILE* f);
size_t __real_fwrite(const void* buffer, size_t size, size_t count, FILE* f);
ssize_t __wrap_read(int fd, void* buffer, size_t size) { touch(buffer, size); return __real_read(fd, buffer, size); }
ssize_t __wrap_pread(int fd, void* buffer, size_t size, off_t offset) {
    touch(buffer, size);
    return __real_pread(fd, buffer, size, offset);
}
ssize_t __wrap_write(int fd, const void* buffer, size_t size) { touch(buffer, size); return __real_write(fd, buffer, size); }
ssize_t __wrap_pwrite(int fd, const void* buffer, size_t size, off_t offset) {
    touch(buffer, size);
    return __real_pwrite(fd, buffer, size, offset);
}
size_t __wrap_fread(void* buffer, size_t size, size_t count, FILE* f) {
    touch(buffer, size * count);
    return __real_fread(buffer, size, count, f);
}
size_t __wrap_fwrite(const void* buffer, size_t size, size_t count, FILE* f) {
    touch(buffer, size * count);
    return __real_fwrite(buffer, size, count, f);
}

}  // extern "C"

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
#include <fcntl.h>

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
long __wrap_sysconf(int name) { return ps5_sysconf(name); }
int __wrap_pthread_getaffinity_np(pthread_t t, size_t size, void* set) { return ps5_pthread_getaffinity_np(t, size, set); }
int __wrap_pthread_setaffinity_np(pthread_t t, size_t size, const void* set) { return ps5_pthread_setaffinity_np(t, size, set); }
int __wrap_mkstemp(char* path_template) { return ps5_mkstemp(path_template); }
FILE* __wrap_tmpfile(void) { return ps5_tmpfile(); }
ssize_t __wrap_readlink(const char* path, char* buffer, size_t size) { return ps5_readlink(path, buffer, size); }
int __wrap_link(const char* existing, const char* name) { return ps5_link(existing, name); }
int __wrap_symlink(const char* target, const char* name) { return ps5_symlink(target, name); }
mode_t __wrap_umask(mode_t mask) { return ps5_umask(mask); }

}  // extern "C"

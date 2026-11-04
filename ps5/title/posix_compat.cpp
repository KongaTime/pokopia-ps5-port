// SPDX-License-Identifier: GPL-3.0-or-later
// The POSIX calls a title wraps at link time (-Wl,--wrap=...): realpath and stdout. Copied from KongaTime's
// The-Darkness-Recomp app/ps5/posix_compat.cpp (the C11-threads wrappers there are for RADV and stay out until a title
// links RADV).

// realpath: the console's sandbox refuses it (EPERM) for the title's own
// folder, and the native runtime normalizes every path it is given through
// std::filesystem::weakly_canonical, which calls it: the first console run of
// that runtime stopped at "in canonical: Operation not permitted
// [/app0/Darkness]" (2026-10-25). The title's paths contain no symbolic links,
// so when the console refuses the call the path is resolved lexically - made
// absolute, "." and ".." folded - and must exist, as realpath requires.
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" char* __real_realpath(const char* path, char* resolved);
extern "C" char* __wrap_realpath(const char* path, char* resolved) {
    if (char* result = __real_realpath(path, resolved)) return result;
    if (!path || (errno != EPERM && errno != EACCES && errno != ENOSYS)) return nullptr;
    std::string absolute;
    if (path[0] != '/') {
        char cwd[PATH_MAX];
        absolute = ::getcwd(cwd, sizeof cwd) ? cwd : "";
        absolute += '/';
    }
    absolute += path;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= absolute.size()) {
        size_t end = absolute.find('/', start);
        if (end == std::string::npos) end = absolute.size();
        const std::string part = absolute.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string normal;
    for (const auto& part : parts) normal += "/" + part;
    if (normal.empty()) normal = "/";
    struct stat st {};
    if (::stat(normal.c_str(), &st) != 0) return nullptr; // errno from stat
    if (normal.size() >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return nullptr;
    }
    if (resolved) {
        std::memcpy(resolved, normal.c_str(), normal.size() + 1);
        return resolved;
    }
    return ::strdup(normal.c_str());
}

// stdout: the platform relays a title's stderr to the kernel log, and nothing
// reads its stdout. The native runtime and the renderer report through printf
// and puts, so the link sends those to stderr as well.
#include <cstdarg>
#include <cstdio>

extern "C" int __wrap_printf(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    return written;
}

extern "C" int __wrap_vprintf(const char* format, va_list arguments) {
    return std::vfprintf(stderr, format, arguments);
}

extern "C" int __wrap_puts(const char* text) {
    const int written = std::fputs(text, stderr);
    std::fputc('\n', stderr);
    return written < 0 ? written : written + 1;
}

extern "C" int __wrap_putchar(int character) { return std::fputc(character, stderr); }

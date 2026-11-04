// SPDX-License-Identifier: GPL-3.0-or-later
// ps5/compat: files, modules, environment and paths (windows.h) on POSIX.
//
// Paths are UTF-8 with either slash; wide paths are converted. A file HANDLE is a POSIX descriptor; reads and writes are
// synchronous (an OVERLAPPED gives the offset, as the runtime uses it). Share modes have no meaning on the console and
// are ignored. The title's own program is /app0/eboot.bin.
#include "win32_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace ps5compat {
namespace {

struct File : Object {
    explicit File(int fd) : Object(Kind::File), fd(fd) {}
    ~File() override {
        if (fd >= 0) close(fd);
    }
    int fd;
};

DWORD error_of(int e) {
    switch (e) {
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
        case EEXIST: return ERROR_FILE_EXISTS;
        case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
        case EBADF: return ERROR_INVALID_HANDLE;
        case ENOSPC: return 112;   // ERROR_DISK_FULL
        default: return ERROR_INVALID_PARAMETER;
    }
}

BOOL fail_errno() {
    SetLastError(error_of(errno));
    return FALSE;
}

constexpr char kModulePath[] = "/app0/eboot.bin";
// GetModuleHandle(nullptr): zeroed memory in the title's image, large enough for the PE header reads diag/symbols.cpp makes
// before it sees there is no 'MZ'.
alignas(16) const char kModuleTag[512] = {};

uint64_t filetime_of(const timespec &ts) {
    return (uint64_t(ts.tv_sec) + 11644473600ull) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
}

void put_filetime(uint64_t v, FILETIME &ft) {
    ft.dwLowDateTime = static_cast<DWORD>(v);
    ft.dwHighDateTime = static_cast<DWORD>(v >> 32);
}

HANDLE create_file(const std::string &path, DWORD access, DWORD disposition) {
    int flags = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
        case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
        case OPEN_EXISTING: break;
        case OPEN_ALWAYS: flags |= O_CREAT; break;
        case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
    }
    struct stat st;
    const bool existed = stat(path.c_str(), &st) == 0;
    const int fd = open(path.c_str(), flags | O_CLOEXEC, 0666);
    if (fd < 0) {
        SetLastError(error_of(errno));
        return INVALID_HANDLE_VALUE;
    }
    // CREATE_ALWAYS and OPEN_ALWAYS report an existing file this way, as Windows does.
    SetLastError((disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) && existed ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return new File(fd);
}

DWORD attributes(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        SetLastError(error_of(errno));
        return INVALID_FILE_ATTRIBUTES;
    }
    DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

BOOL attributes_ex(const std::string &path, LPVOID out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return fail_errno();
    auto *d = static_cast<WIN32_FILE_ATTRIBUTE_DATA *>(out);
    *d = WIN32_FILE_ATTRIBUTE_DATA{};
    d->dwFileAttributes = attributes(path);
    put_filetime(filetime_of(st.st_birthtim), d->ftCreationTime);
    put_filetime(filetime_of(st.st_atim), d->ftLastAccessTime);
    put_filetime(filetime_of(st.st_mtim), d->ftLastWriteTime);
    d->nFileSizeHigh = static_cast<DWORD>(uint64_t(st.st_size) >> 32);
    d->nFileSizeLow = static_cast<DWORD>(st.st_size);
    return TRUE;
}

BOOL move(const std::string &from, const std::string &to, DWORD flags) {
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && stat(to.c_str(), &st) == 0) {
        SetLastError(ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    if (rename(from.c_str(), to.c_str()) != 0) return fail_errno();
    if (flags & MOVEFILE_WRITE_THROUGH) sync();
    return TRUE;
}

BOOL make_dir(const std::string &path) {
    if (mkdir(path.c_str(), 0777) == 0) return TRUE;
    if (errno == EEXIST) {
        SetLastError(ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    return fail_errno();
}

}  // namespace
}  // namespace ps5compat

using namespace ps5compat;

HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD, void *, DWORD disposition, DWORD, HANDLE) {
    return create_file(native_path(path), access, disposition);
}
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD, void *, DWORD disposition, DWORD, HANDLE) {
    return create_file(native_path(utf8_of(path).c_str()), access, disposition);
}
HANDLE CreateFileW(const char *path, DWORD access, DWORD, void *, DWORD disposition, DWORD, HANDLE) {
    return create_file(native_path(path), access, disposition);
}

BOOL ReadFile(HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD read_out, LPOVERLAPPED overlapped) {
    auto *f = as<File>(file, Object::Kind::File);
    if (!f) return FALSE;
    ssize_t n;
    if (overlapped) n = pread(f->fd, buffer, bytes, off_t((uint64_t(overlapped->OffsetHigh) << 32) | overlapped->Offset));
    else n = read(f->fd, buffer, bytes);
    if (n < 0) return fail_errno();
    if (read_out) *read_out = static_cast<DWORD>(n);
    if (overlapped) overlapped->InternalHigh = static_cast<ULONG_PTR>(n);
    // A synchronous read at the end of the file succeeds with 0 bytes; an overlapped one reports ERROR_HANDLE_EOF.
    if (overlapped && n == 0 && bytes) {
        SetLastError(ERROR_HANDLE_EOF);
        return FALSE;
    }
    return TRUE;
}

BOOL WriteFile(HANDLE file, LPCVOID buffer, DWORD bytes, LPDWORD written, LPOVERLAPPED overlapped) {
    auto *f = as<File>(file, Object::Kind::File);
    if (!f) return FALSE;
    const auto *p = static_cast<const uint8_t *>(buffer);
    uint64_t off = overlapped ? (uint64_t(overlapped->OffsetHigh) << 32) | overlapped->Offset : 0;
    DWORD done = 0;
    while (done < bytes) {
        const ssize_t n = overlapped ? pwrite(f->fd, p + done, bytes - done, off_t(off + done)) : write(f->fd, p + done, bytes - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (written) *written = done;
            return fail_errno();
        }
        done += static_cast<DWORD>(n);
    }
    if (written) *written = done;
    if (overlapped) overlapped->InternalHigh = done;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE file) {
    auto *f = as<File>(file, Object::Kind::File);
    return f && fsync(f->fd) == 0 ? TRUE : fail_errno();
}

BOOL GetFileSizeEx(HANDLE file, PLARGE_INTEGER size) {
    auto *f = as<File>(file, Object::Kind::File);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) return fail_errno();
    size->QuadPart = st.st_size;
    return TRUE;
}

DWORD GetFileSize(HANDLE file, LPDWORD high) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(file, &s)) return INVALID_FILE_SIZE;
    if (high) *high = static_cast<DWORD>(uint64_t(s.QuadPart) >> 32);
    return static_cast<DWORD>(s.QuadPart);
}

BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method) {
    auto *f = as<File>(file, Object::Kind::File);
    if (!f) return FALSE;
    const int whence = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    const off_t r = lseek(f->fd, off_t(distance.QuadPart), whence);
    if (r < 0) return fail_errno();
    if (position) position->QuadPart = r;
    return TRUE;
}

DWORD SetFilePointer(HANDLE file, LONG distance, LONG *high, DWORD method) {
    LARGE_INTEGER d, pos;
    d.QuadPart = high ? (int64_t(*high) << 32) | uint32_t(distance) : distance;
    if (!SetFilePointerEx(file, d, &pos, method)) return INVALID_SET_FILE_POINTER;
    if (high) *high = static_cast<LONG>(pos.QuadPart >> 32);
    return static_cast<DWORD>(pos.QuadPart);
}

BOOL SetEndOfFile(HANDLE file) {
    auto *f = as<File>(file, Object::Kind::File);
    if (!f) return FALSE;
    const off_t at = lseek(f->fd, 0, SEEK_CUR);
    return at >= 0 && ftruncate(f->fd, at) == 0 ? TRUE : fail_errno();
}

DWORD GetFileAttributesA(LPCSTR path) { return attributes(native_path(path)); }
DWORD GetFileAttributesW(LPCWSTR path) { return attributes(native_path(utf8_of(path).c_str())); }
DWORD GetFileAttributesW(const char *path) { return attributes(native_path(path)); }

BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS, LPVOID info) { return attributes_ex(native_path(path), info); }
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS, LPVOID info) {
    return attributes_ex(native_path(utf8_of(path).c_str()), info);
}
BOOL GetFileAttributesExW(const char *path, GET_FILEEX_INFO_LEVELS, LPVOID info) {
    return attributes_ex(native_path(path), info);
}

BOOL DeleteFileA(LPCSTR path) { return unlink(native_path(path).c_str()) == 0 ? TRUE : fail_errno(); }
BOOL DeleteFileW(LPCWSTR path) { return DeleteFileA(utf8_of(path).c_str()); }
BOOL DeleteFileW(const char *path) { return DeleteFileA(path); }

BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) { return move(native_path(from), native_path(to), flags); }
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    return move(native_path(utf8_of(from).c_str()), native_path(utf8_of(to).c_str()), flags);
}
BOOL MoveFileExW(const char *from, const char *to, DWORD flags) { return move(native_path(from), native_path(to), flags); }

BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    const std::string src = native_path(from), dst = native_path(to);
    const int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return fail_errno();
    const int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0666);
    if (out < 0) {
        const int e = errno;
        close(in);
        SetLastError(e == EEXIST ? ERROR_FILE_EXISTS : error_of(e));
        return FALSE;
    }
    char buf[1 << 16];
    ssize_t n;
    bool ok = true;
    while ((n = read(in, buf, sizeof buf)) > 0)
        for (ssize_t done = 0; done < n;) {
            const ssize_t w = write(out, buf + done, size_t(n - done));
            if (w < 0) { ok = false; break; }
            done += w;
        }
    if (n < 0) ok = false;
    close(in);
    close(out);
    if (!ok) return fail_errno();
    return TRUE;
}

BOOL CreateDirectoryA(LPCSTR path, void *) { return make_dir(native_path(path)); }
BOOL CreateDirectoryW(LPCWSTR path, void *) { return make_dir(native_path(utf8_of(path).c_str())); }
BOOL CreateDirectoryW(const char *path, void *) { return make_dir(native_path(path)); }

// ---------------------------------------------------------------------------------------------------------------------
// the title's own module

DWORD GetModuleFileNameA(HMODULE module, LPSTR out, DWORD size) {
    if (module && module != reinterpret_cast<HMODULE>(const_cast<char *>(kModuleTag))) {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    const DWORD n = sizeof kModulePath - 1;
    if (!out || size == 0) return 0;
    const DWORD copied = std::min<DWORD>(n, size - 1);
    memcpy(out, kModulePath, copied);
    out[copied] = 0;
    if (copied < n) SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return copied;
}

DWORD GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size) {
    char tmp[sizeof kModulePath];
    const DWORD n = GetModuleFileNameA(module, tmp, sizeof tmp);
    if (!n || !out || !size) return 0;
    const DWORD copied = std::min<DWORD>(n, size - 1);
    for (DWORD i = 0; i < copied; ++i) out[i] = static_cast<wchar_t>(tmp[i]);
    out[copied] = 0;
    return copied;
}

HMODULE GetModuleHandleA(LPCSTR name) {
    // The title is one module: there are no system DLLs to find (ntdll and the like answer null, as an absent DLL does).
    if (!name) return reinterpret_cast<HMODULE>(const_cast<char *>(kModuleTag));
    SetLastError(126);   // ERROR_MOD_NOT_FOUND
    return nullptr;
}
HMODULE GetModuleHandleW(LPCWSTR name) { return name ? GetModuleHandleA(utf8_of(name).c_str()) : GetModuleHandleA(nullptr); }

BOOL GetModuleHandleExA(DWORD, LPCSTR, HMODULE *module) {
    if (!module) return FALSE;
    *module = GetModuleHandleA(nullptr);
    return TRUE;
}

FARPROC GetProcAddress(HMODULE, LPCSTR) {
    SetLastError(127);   // ERROR_PROC_NOT_FOUND
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// environment, paths, ini files

DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR out, DWORD size) {
    const char *v = name ? getenv(name) : nullptr;
    if (!v) {
        SetLastError(203);   // ERROR_ENVVAR_NOT_FOUND
        return 0;
    }
    const DWORD n = static_cast<DWORD>(strlen(v));
    if (!out || size <= n) return n + 1;   // the size needed, terminator included
    memcpy(out, v, n + 1);
    return n;
}

DWORD GetCurrentDirectoryA(DWORD size, LPSTR out) {
    char buf[1024];
    if (!getcwd(buf, sizeof buf)) return fail_errno();
    const DWORD n = static_cast<DWORD>(strlen(buf));
    if (!out || size <= n) return n + 1;
    memcpy(out, buf, n + 1);
    return n;
}

DWORD GetFullPathNameA(LPCSTR path, DWORD size, LPSTR out, LPSTR *file_part) {
    if (!path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::string p = native_path(path);
    if (p.empty() || p[0] != '/') {
        char cwd[1024];
        p = std::string(getcwd(cwd, sizeof cwd) ? cwd : "") + "/" + p;
    }
    // Lexically: "." and ".." components go, as Windows' GetFullPathName does without touching the file system.
    std::string full;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string part = p.substr(i, j - i);
        if (part == "..") {
            const size_t k = full.rfind('/');
            full.erase(k == std::string::npos ? 0 : k);
        } else if (!part.empty() && part != ".") {
            full += "/" + part;
        }
        i = j + 1;
    }
    if (full.empty()) full = "/";
    const DWORD n = static_cast<DWORD>(full.size());
    if (!out || size <= n) return n + 1;
    memcpy(out, full.c_str(), n + 1);
    if (file_part) {
        char *slash = strrchr(out, '/');
        *file_part = slash && slash[1] ? slash + 1 : nullptr;
    }
    return n;
}

DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file) {
    // The form the runtime uses: one value by section and key (both given), case-insensitive, from an ini file.
    std::string value = def ? def : "";
    if (section && key && file) {
        std::ifstream in(native_path(file));
        std::string line, current;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos || line[b] == ';' || line[b] == '#') continue;
            line.erase(0, b);
            if (line[0] == '[') {
                current = line.substr(1, line.find(']') - 1);
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos || strcasecmp(current.c_str(), section) != 0) continue;
            std::string k = line.substr(0, eq);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            if (strcasecmp(k.c_str(), key) != 0) continue;
            std::string v = line.substr(eq + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
            value = v;
            break;
        }
    }
    if (!out || size == 0) return 0;
    const DWORD n = std::min<DWORD>(static_cast<DWORD>(value.size()), size - 1);
    memcpy(out, value.data(), n);
    out[n] = 0;
    return n;
}

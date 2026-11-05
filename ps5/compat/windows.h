// SPDX-License-Identifier: GPL-3.0-or-later
// The part of the Win32 API the nxrt runtime uses, for the PS5 build only (ps5/PLAN.md, "ps5/compat").
//
// Only what the runtime calls is here, with the Windows meaning: sizes are the LLP64 ones (DWORD and LONG are 32 bits),
// structures keep the field names the runtime reads, and every function's behaviour is the documented Win32 behaviour
// for the arguments the runtime passes. The implementations are in win32_ps5.cpp, on pthreads, POSIX files and the
// console's direct memory (ps5platform). What is not implemented fails the way Windows fails, with SetLastError.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

// ---------------------------------------------------------------------------------------------------------------------
// basic types

typedef int BOOL;
typedef uint8_t BYTE, UCHAR, BOOLEAN;
typedef uint16_t WORD, USHORT, LANGID;
typedef int16_t SHORT;
typedef uint32_t DWORD, ULONG, UINT, *PULONG, *LPDWORD, *PDWORD;
typedef int32_t LONG, INT, NTSTATUS, HRESULT;
typedef int64_t LONGLONG, LONG64, INT_PTR, LONG_PTR;
typedef uint64_t ULONGLONG, DWORD64, ULONG64, DWORD_PTR, ULONG_PTR, UINT_PTR, SIZE_T, *PSIZE_T, *PULONG64, *PDWORD_PTR, *PDWORD64;
typedef int64_t SSIZE_T;
typedef void VOID, *PVOID, *LPVOID, *HANDLE, *HMODULE, *HINSTANCE, *HWND, *FARPROC;
typedef const void *LPCVOID;
typedef char CHAR, *LPSTR;
typedef const char *LPCSTR;
typedef wchar_t WCHAR, *LPWSTR;
typedef const wchar_t *LPCWSTR;
typedef HANDLE *PHANDLE, *LPHANDLE;

#define WINAPI
#define CALLBACK
#define APIENTRY
#define NTAPI
#define FALSE 0
#define TRUE 1
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(~uintptr_t{0}))
#define S_OK 0
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

// ---------------------------------------------------------------------------------------------------------------------
// errors

#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_ADDRESS 487u
#define ERROR_HANDLE_EOF 38u
#define ERROR_NOT_SUPPORTED 50u
#define ERROR_FILE_EXISTS 80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_NO_UNICODE_TRANSLATION 1113u
#define ERROR_TIMEOUT 1460u
#define ERROR_COMMITMENT_LIMIT 1455u

DWORD GetLastError();
void SetLastError(DWORD error);

// ---------------------------------------------------------------------------------------------------------------------
// handles, processes, threads

#define DUPLICATE_SAME_ACCESS 0x2u
#define SYNCHRONIZE 0x00100000u
#define THREAD_ALL_ACCESS 0x001FFFFFu
#define THREAD_SUSPEND_RESUME 0x2u
#define THREAD_GET_CONTEXT 0x8u
#define THREAD_QUERY_INFORMATION 0x40u
#define CREATE_SUSPENDED 0x4u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000u

#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_ERROR_RETURN 0x7FFFFFFF

#define NORMAL_PRIORITY_CLASS 0x20u
#define ABOVE_NORMAL_PRIORITY_CLASS 0x8000u
#define HIGH_PRIORITY_CLASS 0x80u

#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED 0x80u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu

typedef DWORD(WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);

typedef struct _PROCESSOR_NUMBER {
    WORD Group;
    BYTE Number;
    BYTE Reserved;
} PROCESSOR_NUMBER, *PPROCESSOR_NUMBER;

BOOL CloseHandle(HANDLE h);
BOOL DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process, LPHANDLE dst, DWORD access, BOOL inherit,
                     DWORD options);
HANDLE GetCurrentProcess();
HANDLE GetCurrentThread();
DWORD GetCurrentProcessId();
DWORD GetCurrentThreadId();
DWORD GetThreadId(HANDLE thread);
DWORD GetProcessId(HANDLE process);
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD thread_id);
HANDLE CreateThread(void *security, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags,
                    LPDWORD thread_id);
DWORD ResumeThread(HANDLE thread);
DWORD SuspendThread(HANDLE thread);
[[noreturn]] void ExitThread(DWORD code);
[[noreturn]] void ExitProcess(UINT code);
BOOL TerminateProcess(HANDLE process, UINT code);
BOOL GetExitCodeThread(HANDLE thread, LPDWORD code);
BOOL SetThreadPriority(HANDLE thread, int priority);
int GetThreadPriority(HANDLE thread);
BOOL SetPriorityClass(HANDLE process, DWORD cls);
DWORD GetPriorityClass(HANDLE process);
DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
BOOL GetThreadTimes(HANDLE thread, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user);
BOOL GetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user);
DWORD GetCurrentProcessorNumber();
void GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER number);
HRESULT SetThreadDescription(HANDLE thread, LPCWSTR name);
BOOL SetThreadPriorityBoost(HANDLE thread, BOOL disable);
BOOL SetThreadStackGuarantee(PULONG size);
BOOL GetThreadIdealProcessorEx(HANDLE thread, PPROCESSOR_NUMBER ideal);
BOOL ReadProcessMemory(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T size, SIZE_T *read);
DWORD SetThreadIdealProcessorEx(HANDLE thread, PPROCESSOR_NUMBER ideal, PPROCESSOR_NUMBER previous);
BOOL GetProcessAffinityMask(HANDLE process, PDWORD_PTR process_mask, PDWORD_PTR system_mask);
BOOL QueryThreadCycleTime(HANDLE thread, PULONG64 cycles);
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000u
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD process_id);
void Sleep(DWORD ms);
DWORD SleepEx(DWORD ms, BOOL alertable);
BOOL SwitchToThread();
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD ms);

static inline void YieldProcessor() { __builtin_ia32_pause(); }

// events and waitable timers
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x1u
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2u
#define TIMER_ALL_ACCESS 0x1F0003u
#define EVENT_ALL_ACCESS 0x1F0003u
typedef void(CALLBACK *PTIMERAPCROUTINE)(LPVOID, DWORD, DWORD);
HANDLE CreateEventW(void *security, BOOL manual_reset, BOOL initial, LPCWSTR name);
HANDLE CreateEventA(void *security, BOOL manual_reset, BOOL initial, LPCSTR name);
#define CreateEvent CreateEventW
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);
HANDLE CreateWaitableTimerExW(void *security, LPCWSTR name, DWORD flags, DWORD access);
HANDLE CreateWaitableTimerW(void *security, BOOL manual_reset, LPCWSTR name);
#define CreateWaitableTimer CreateWaitableTimerW
#define CreateWaitableTimerEx CreateWaitableTimerExW
BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER *due, LONG period, PTIMERAPCROUTINE apc, LPVOID apc_arg,
                      BOOL resume);
BOOL CancelWaitableTimer(HANDLE timer);
typedef struct _REASON_CONTEXT *PREASON_CONTEXT;
BOOL SetWaitableTimerEx(HANDLE timer, const LARGE_INTEGER *due, LONG period, PTIMERAPCROUTINE apc, LPVOID apc_arg,
                        PREASON_CONTEXT wake_context, ULONG tolerable_delay);

// mutexes (named ones are only ever looked up inside this process: the console runs one title)
HANDLE CreateMutexA(void *security, BOOL initial_owner, LPCSTR name);
HANDLE CreateMutexW(void *security, BOOL initial_owner, LPCWSTR name);
BOOL ReleaseMutex(HANDLE mutex);

// thread-local storage
#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu
DWORD TlsAlloc();
BOOL TlsFree(DWORD index);
LPVOID TlsGetValue(DWORD index);
BOOL TlsSetValue(DWORD index, LPVOID value);

// ---------------------------------------------------------------------------------------------------------------------
// slim reader/writer locks, condition variables, critical sections, address waits
//
// SRWLOCK and CONDITION_VARIABLE are one pointer, zero when initialised (SRWLOCK_INIT), as on Windows: they hold the
// SDK's pthread_mutex_t / pthread_cond_t, which are pointers whose static initialiser is NULL (sys/_pthreadtypes.h),
// so a zeroed object is a valid unlocked lock. Shared acquisition takes the lock exclusively: SRW locks are not
// recursive in either mode, so this is never a deadlock the Windows lock would not also have, only less concurrency.

typedef struct _RTL_SRWLOCK { void *Ptr; } SRWLOCK, *PSRWLOCK;
typedef struct _RTL_CONDITION_VARIABLE { void *Ptr; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define SRWLOCK_INIT {nullptr}
#define CONDITION_VARIABLE_INIT {nullptr}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1u

void InitializeSRWLock(PSRWLOCK lock);
void AcquireSRWLockExclusive(PSRWLOCK lock);
void ReleaseSRWLockExclusive(PSRWLOCK lock);
void AcquireSRWLockShared(PSRWLOCK lock);
void ReleaseSRWLockShared(PSRWLOCK lock);
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock);
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock);
void InitializeConditionVariable(PCONDITION_VARIABLE cv);
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags);
void WakeConditionVariable(PCONDITION_VARIABLE cv);
void WakeAllConditionVariable(PCONDITION_VARIABLE cv);

typedef struct _RTL_CRITICAL_SECTION { void *Ptr; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin);
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
void EnterCriticalSection(LPCRITICAL_SECTION cs);
void LeaveCriticalSection(LPCRITICAL_SECTION cs);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs);

BOOL WaitOnAddress(volatile void *address, void *compare, SIZE_T size, DWORD ms);
void WakeByAddressSingle(void *address);
void WakeByAddressAll(void *address);

// interlocked operations the runtime names (the rest of it uses std::atomic)
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG cmp) {
    __atomic_compare_exchange_n(p, &cmp, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
static inline void MemoryBarrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// ---------------------------------------------------------------------------------------------------------------------
// time

BOOL QueryPerformanceCounter(LARGE_INTEGER *count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency);
ULONGLONG GetTickCount64();
DWORD GetTickCount();
void GetSystemTimeAsFileTime(LPFILETIME time);
void GetSystemTimePreciseAsFileTime(LPFILETIME time);
void GetSystemTime(LPSYSTEMTIME time);
void GetLocalTime(LPSYSTEMTIME time);
BOOL FileTimeToSystemTime(const FILETIME *file_time, LPSYSTEMTIME system_time);
BOOL SystemTimeToFileTime(const SYSTEMTIME *system_time, LPFILETIME file_time);

// ---------------------------------------------------------------------------------------------------------------------
// virtual memory and file mappings
//
// Pages are 4 KiB on Windows and 16 KiB on the console. Reservations and commits are rounded out to 16 KiB; a
// protection change applies to the whole 16 KiB pages it touches. File mappings are page-file-backed sections only
// (INVALID_HANDLE_VALUE): a section is backed in 64 KiB units of direct memory as its pages are first committed, and each
// unit is mapped into every view of it, so views see the same memory as on Windows.

#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_DECOMMIT 0x00004000u
#define MEM_RELEASE 0x00008000u
#define MEM_FREE 0x00010000u
#define MEM_PRIVATE 0x00020000u
#define MEM_MAPPED 0x00040000u
#define MEM_RESET 0x00080000u
#define MEM_TOP_DOWN 0x00100000u
#define MEM_IMAGE 0x01000000u

#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_WRITECOPY 0x08u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_EXECUTE_WRITECOPY 0x80u
#define PAGE_GUARD 0x100u
#define PAGE_NOCACHE 0x200u

#define SEC_COMMIT 0x08000000u
#define SEC_RESERVE 0x04000000u

#define FILE_MAP_COPY 0x1u
#define FILE_MAP_WRITE 0x2u
#define FILE_MAP_READ 0x4u
#define FILE_MAP_EXECUTE 0x20u
#define FILE_MAP_ALL_ACCESS 0xF001Fu

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct _SYSTEM_INFO {
    WORD wProcessorArchitecture;
    WORD wReserved;
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old_protect);
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length);
BOOL VirtualLock(LPVOID address, SIZE_T size);
BOOL FlushInstructionCache(HANDLE process, LPCVOID address, SIZE_T size);
BOOL IsBadReadPtr(const void *address, UINT_PTR size);
void GetSystemInfo(LPSYSTEM_INFO info);

HANDLE CreateFileMappingW(HANDLE file, void *security, DWORD protect, DWORD size_high, DWORD size_low, LPCWSTR name);
HANDLE CreateFileMappingA(HANDLE file, void *security, DWORD protect, DWORD size_high, DWORD size_low, LPCSTR name);
#define CreateFileMapping CreateFileMappingW
LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T size);
LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T size, LPVOID address);
BOOL UnmapViewOfFile(LPCVOID view);

// ---------------------------------------------------------------------------------------------------------------------
// structured exceptions: vectored handlers on the console's signals

#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u
#define EXCEPTION_BREAKPOINT 0x80000003u
#define EXCEPTION_GUARD_PAGE 0x80000001u
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDu
#define EXCEPTION_NONCONTINUABLE 0x1u
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_MAXIMUM_PARAMETERS 15

#define CONTEXT_AMD64 0x00100000u
#define CONTEXT_CONTROL (CONTEXT_AMD64 | 0x1u)
#define CONTEXT_INTEGER (CONTEXT_AMD64 | 0x2u)
#define CONTEXT_SEGMENTS (CONTEXT_AMD64 | 0x4u)
#define CONTEXT_FLOATING_POINT (CONTEXT_AMD64 | 0x8u)
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)
#define CONTEXT_ALL CONTEXT_FULL

typedef struct alignas(16) _M128A { ULONGLONG Low; LONGLONG High; } M128A;

// The fields of the x64 CONTEXT the runtime reads and writes, by name; not the Windows layout.
typedef struct alignas(16) _CONTEXT {
    DWORD ContextFlags;
    DWORD MxCsr;
    DWORD EFlags;
    WORD SegCs, SegSs;
    DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15, Rip;
    M128A Xmm[16];
} CONTEXT, *PCONTEXT;

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;

typedef LONG(NTAPI *PVECTORED_EXCEPTION_HANDLER)(PEXCEPTION_POINTERS);
typedef LONG(WINAPI *LPTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS);
typedef LPTOP_LEVEL_EXCEPTION_FILTER PTOP_LEVEL_EXCEPTION_FILTER;

// __try / __except (EXCEPTION_EXECUTE_HANDLER): the console's compiler has no SEH. A caller that reads memory which may be
// gone (the graphics shell's texture hashing reads the game's storage) runs the read through seh_guard instead: false when
// an access fault in it reached no vectored handler that resumed it (the fault handler jumps back out of the guarded call).
namespace ps5compat {
// Bring-up breadcrumbs (win32_internal.h): a line in $NXRT_PS5_BREADCRUMBS, nothing when it is unset.
void crumb(const char *format, ...) __attribute__((format(printf, 1, 2)));
void crumb_dump();        // writes the breadcrumb ring to $NXRT_PS5_BREADCRUMBS
void crumb_freeze();      // keeps the ring as it is (the first unhandled exception or fault)
void fault_uninstall();   // the process is ending: signals back to the system (win32_fault.cpp)
bool seh_guard_call(void (*fn)(void *), void *arg);
// Backs any committed section pages in [p, p + n) that have no memory yet (they are backed on first touch, which the
// kernel does not do for a buffer handed to it: read() into one fails with EFAULT instead).
void touch(const void *p, size_t n);
template <class F>
bool seh_guard(F &&f) {
    return seh_guard_call([](void *p) { (*static_cast<F *>(p))(); }, &f);
}
}  // namespace ps5compat

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
ULONG RemoveVectoredExceptionHandler(PVOID handle);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
[[noreturn]] void RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments);
void RtlCaptureContext(PCONTEXT context);
BOOL GetThreadContext(HANDLE thread, PCONTEXT context);

// ---------------------------------------------------------------------------------------------------------------------
// files

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x1u
#define FILE_SHARE_WRITE 0x2u
#define FILE_SHARE_DELETE 0x4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_READONLY 0x1u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_FILE_SIZE 0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
#define MOVEFILE_REPLACE_EXISTING 0x1u
#define MOVEFILE_WRITE_THROUGH 0x8u

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct { DWORD Offset; DWORD OffsetHigh; };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel } GET_FILEEX_INFO_LEVELS;
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags,
                   HANDLE template_file);
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags,
                   HANDLE template_file);
#define CreateFile CreateFileW
BOOL ReadFile(HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped);
BOOL WriteFile(HANDLE file, LPCVOID buffer, DWORD bytes, LPDWORD written, LPOVERLAPPED overlapped);
BOOL FlushFileBuffers(HANDLE file);
BOOL GetFileSizeEx(HANDLE file, PLARGE_INTEGER size);
DWORD GetFileSize(HANDLE file, LPDWORD high);
BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method);
DWORD SetFilePointer(HANDLE file, LONG distance, LONG *high, DWORD method);
BOOL SetEndOfFile(HANDLE file);
DWORD GetFileAttributesA(LPCSTR path);
DWORD GetFileAttributesW(LPCWSTR path);
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL DeleteFileA(LPCSTR path);
BOOL DeleteFileW(LPCWSTR path);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
BOOL CreateDirectoryA(LPCSTR path, void *security);
BOOL CreateDirectoryW(LPCWSTR path, void *security);
DWORD GetFullPathNameA(LPCSTR path, DWORD size, LPSTR out, LPSTR *file_part);
DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file);

// std::filesystem::path is char on the console, so the runtime's W calls with path.c_str() arrive as UTF-8: these take
// it as it is.
HANDLE CreateFileW(const char *path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags,
                   HANDLE template_file);
DWORD GetFileAttributesW(const char *path);
BOOL GetFileAttributesExW(const char *path, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL DeleteFileW(const char *path);
BOOL MoveFileExW(const char *from, const char *to, DWORD flags);
BOOL CreateDirectoryW(const char *path, void *security);
HANDLE CreateMutexW(void *security, BOOL initial_owner, const char *name);

DWORD GetModuleFileNameA(HMODULE module, LPSTR out, DWORD size);
DWORD GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size);
HMODULE GetModuleHandleA(LPCSTR name);
HMODULE GetModuleHandleW(LPCWSTR name);
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2u
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4u
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *module);
FARPROC GetProcAddress(HMODULE module, LPCSTR name);
HMODULE LoadLibraryA(LPCSTR name);   // the title loads no DLLs: always null (ERROR_MOD_NOT_FOUND)
HMODULE LoadLibraryW(LPCWSTR name);
BOOL FreeLibrary(HMODULE module);

// PE headers: diag/symbols.cpp looks for the exe's CodeView record through GetModuleHandle(nullptr). The title's module
// handle points at zeroed memory, so e_magic is never IMAGE_DOS_SIGNATURE and the lookup reports no record.
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_DIRECTORY_ENTRY_DEBUG 6
#define IMAGE_DEBUG_TYPE_CODEVIEW 2
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16
typedef struct _IMAGE_DOS_HEADER { WORD e_magic; WORD e_unused[29]; LONG e_lfanew; } IMAGE_DOS_HEADER;
typedef struct _IMAGE_DATA_DIRECTORY { DWORD VirtualAddress; DWORD Size; } IMAGE_DATA_DIRECTORY;
typedef struct _IMAGE_OPTIONAL_HEADER64 {
    BYTE Fields[112];   // what precedes the data directories in the PE32+ optional header
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64;
typedef struct _IMAGE_FILE_HEADER { WORD Machine, NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; WORD SizeOfOptionalHeader, Characteristics; } IMAGE_FILE_HEADER;
typedef struct _IMAGE_NT_HEADERS64 { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER64 OptionalHeader; } IMAGE_NT_HEADERS64, IMAGE_NT_HEADERS;
typedef struct _IMAGE_DEBUG_DIRECTORY {
    DWORD Characteristics, TimeDateStamp;
    WORD MajorVersion, MinorVersion;
    DWORD Type, SizeOfData, AddressOfRawData, PointerToRawData;
} IMAGE_DEBUG_DIRECTORY;
static_assert(sizeof(IMAGE_DOS_HEADER) == 64, "IMAGE_DOS_HEADER layout");

// Unwinding (diag/stackwalk.cpp): the console build keeps frame pointers (-fno-omit-frame-pointer), so the "function
// entry" here is a frame record and RtlVirtualUnwind steps one frame up the rbp chain.
#define UNW_FLAG_NHANDLER 0x0u
typedef struct _RUNTIME_FUNCTION { DWORD BeginAddress, EndAddress, UnwindData; } RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;
PRUNTIME_FUNCTION RtlLookupFunctionEntry(DWORD64 pc, PDWORD64 image_base, void *history);
void *RtlVirtualUnwind(DWORD type, DWORD64 image_base, DWORD64 pc, PRUNTIME_FUNCTION fn, PCONTEXT context,
                       PVOID *handler_data, PDWORD64 establisher, void *pointers);
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR out, DWORD size);
DWORD GetCurrentDirectoryA(DWORD size, LPSTR out);
void OutputDebugStringA(LPCSTR text);
void OutputDebugStringW(LPCWSTR text);
BOOL IsDebuggerPresent();
void DebugBreak();

// ---------------------------------------------------------------------------------------------------------------------
// strings and locale

#define CP_ACP 0u
#define CP_UTF8 65001u
#define MB_ERR_INVALID_CHARS 0x8u
#define WC_ERR_INVALID_CHARS 0x80u

// wchar_t is 4 bytes on the console. The conversions take and give wchar_t strings with one code point per element
// (UTF-32), the console's own wide encoding, so the runtime's std::wstring and L"" literals keep working; text the game
// sees as UTF-16 never passes through these (the services convert guest strings themselves).
int MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR in, int in_len, LPWSTR out, int out_len);
int WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR in, int in_len, LPSTR out, int out_len, LPCSTR def,
                        BOOL *used_def);

#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))
#define PRIMARYLANGID(l) ((WORD)(l) & 0x3ff)
#define SUBLANGID(l) ((WORD)(l) >> 10)
#define LANG_NEUTRAL 0x00
#define LANG_CHINESE 0x04
#define LANG_GERMAN 0x07
#define LANG_ENGLISH 0x09
#define LANG_SPANISH 0x0a
#define LANG_FRENCH 0x0c
#define LANG_ITALIAN 0x10
#define LANG_JAPANESE 0x11
#define LANG_KOREAN 0x12
#define LANG_DUTCH 0x13
#define LANG_PORTUGUESE 0x16
#define LANG_RUSSIAN 0x19
#define SUBLANG_DEFAULT 0x01
#define SUBLANG_CHINESE_TRADITIONAL 0x01
#define SUBLANG_CHINESE_SIMPLIFIED 0x02
#define SUBLANG_CHINESE_HONGKONG 0x03
#define SUBLANG_CHINESE_SINGAPORE 0x04
#define SUBLANG_CHINESE_MACAU 0x05
#define SUBLANG_ENGLISH_US 0x01
#define SUBLANG_ENGLISH_UK 0x02
#define SUBLANG_ENGLISH_CAN 0x04
#define SUBLANG_FRENCH_CANADIAN 0x03
#define SUBLANG_PORTUGUESE_BRAZILIAN 0x01
#define SUBLANG_SPANISH 0x01
#define SUBLANG_SPANISH_MODERN 0x03
LANGID GetUserDefaultUILanguage();
LANGID GetUserDefaultLangID();

// GDI/USER object counts (diagnostics): always zero on the console.
#define GR_GDIOBJECTS 0u
#define GR_USEROBJECTS 1u
DWORD GetGuiResources(HANDLE process, DWORD flags);

// ---------------------------------------------------------------------------------------------------------------------
// processor topology (kernel/sched.cpp counts physical cores)

typedef enum _LOGICAL_PROCESSOR_RELATIONSHIP {
    RelationProcessorCore = 0,
    RelationNumaNode = 1,
    RelationCache = 2,
    RelationProcessorPackage = 3,
    RelationGroup = 4,
    RelationAll = 0xffff
} LOGICAL_PROCESSOR_RELATIONSHIP;

typedef struct _GROUP_AFFINITY {
    ULONG_PTR Mask;
    WORD Group;
    WORD Reserved[3];
} GROUP_AFFINITY;

typedef struct _PROCESSOR_RELATIONSHIP {
    BYTE Flags;
    BYTE EfficiencyClass;
    BYTE Reserved[20];
    WORD GroupCount;
    GROUP_AFFINITY GroupMask[1];
} PROCESSOR_RELATIONSHIP;

typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX {
    LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
    DWORD Size;
    union {
        PROCESSOR_RELATIONSHIP Processor;
    };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX;

BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP relationship,
                                      PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer, PDWORD length);
DWORD GetActiveProcessorCount(WORD group);
#define ALL_PROCESSOR_GROUPS 0xffff

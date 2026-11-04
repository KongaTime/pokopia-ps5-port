// SPDX-License-Identifier: GPL-3.0-or-later
// ps5/compat: virtual memory and page-file sections (windows.h) on the console's direct memory.
//
// What the console gives (ps5platform/kernel.h, shm.h; measured by its probe): 16 KiB pages; direct memory in 64 KiB units
// that can be mapped at any number of addresses; reservations exactly at an address from 0x10_0000_0000 up to 1 TiB;
// execute only by a protection change after mapping. What the runtime expects (runtime/src/mem/memory.cpp): Windows' 4 KiB
// pages, reservations at fixed addresses committed on demand, and one SEC_RESERVE section (the guest heap) with a
// primary view and alias views of the same pages.
//
// So:
//  - every 4 KiB page has one byte in a lock-free table: its Windows state (free, reserved, committed), its protection,
//    and whether it belongs to a view. VirtualQuery answers from it;
//  - a console page takes the union of the protections of its committed 4 KiB pages, so a read-only 4 KiB page never
//    takes write access from its neighbours (it may give them more: a read-only page beside a writable one in the same
//    16 KiB page is writable). Protection changes of one 16 KiB page are serialised by a striped spinlock, which is the
//    only lock VirtualProtect takes: the runtime's fault handlers (mem/write_watch.cpp) call it from a signal handler;
//  - private allocations are ps5platform virtual ranges (ps5_vrange_*), backed as they are committed;
//  - a section is a table of 64 KiB direct-memory units allocated at their first commit, each mapped into every view of
//    the section, so all views see the same memory, as on Windows.
#include "win32_internal.h"

#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace ps5compat {
namespace {

constexpr uint64_t kWinPage = 0x1000;
constexpr uint64_t kConsolePage = PS5_KERNEL_PAGE_SIZE;           // 0x4000
constexpr uint64_t kUnit = PS5_KERNEL_DIRECT_ALIGNMENT;            // 0x10000, also Windows' allocation granularity
constexpr uint64_t kLimit = 0x10000000000ull;                       // 1 TiB: the console grants nothing above
constexpr uint64_t kLeafSpan = 0x4000000ull;                        // 64 MiB of address space per table leaf
constexpr uint64_t kLeafPages = kLeafSpan / kWinPage;               // 16384 entries

uint64_t down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// ---- the page table ----
// Entry: bits 0-1 state (0 free, 1 reserved, 2 committed), bits 2-4 protection code, bit 5 part of a view, bit 6 backed
// (memory is mapped there). Private memory is backed when committed; a view's page only once its section unit is (on
// first touch: section_fault), so a committed page can be unbacked, and then reads as committed but has nothing mapped.
enum : uint8_t { kFree = 0, kReserved = 1, kCommitted = 2 };
enum : uint8_t { kNone = 0, kR = 1, kRW = 2, kRX = 3, kRWX = 4 };
constexpr uint8_t kViewBit = 0x20;
constexpr uint8_t kBackedBit = 0x40;

uint8_t entry(uint8_t state, uint8_t prot, bool view, bool backed) {
    return uint8_t(state | prot << 2 | (view ? kViewBit : 0) | (backed ? kBackedBit : 0));
}
uint8_t state_of(uint8_t e) { return e & 3; }
uint8_t prot_of(uint8_t e) { return (e >> 2) & 7; }
bool view_of(uint8_t e) { return (e & kViewBit) != 0; }
bool backed_of(uint8_t e) { return (e & kBackedBit) != 0; }

uint8_t *g_leaves[kLimit / kLeafSpan];

uint8_t *leaf(uint64_t addr, bool create) {
    uint8_t **slot = &g_leaves[addr / kLeafSpan];
    uint8_t *l = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (l || !create) return l;
    auto *fresh = static_cast<uint8_t *>(calloc(kLeafPages, 1));
    uint8_t *expected = nullptr;
    if (__atomic_compare_exchange_n(slot, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return fresh;
    free(fresh);
    return expected;
}

uint8_t get(uint64_t addr) {
    if (addr >= kLimit) return 0;
    const uint8_t *l = leaf(addr, false);
    return l ? __atomic_load_n(&l[(addr % kLeafSpan) / kWinPage], __ATOMIC_ACQUIRE) : 0;
}

void set_range(uint64_t start, uint64_t end, uint8_t e) {
    for (uint64_t a = start; a < end; a += kWinPage)
        __atomic_store_n(&leaf(a, true)[(a % kLeafSpan) / kWinPage], e, __ATOMIC_RELEASE);
}

// ---- protections ----

uint8_t code_of(DWORD protect) {
    switch (protect & 0xFF) {
        case PAGE_READONLY: return kR;
        case PAGE_READWRITE: case PAGE_WRITECOPY: return kRW;
        case PAGE_EXECUTE: case PAGE_EXECUTE_READ: return kRX;
        case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return kRWX;
        default: return kNone;
    }
}
DWORD protect_of(uint8_t code) {
    static constexpr DWORD k[] = {PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE};
    return code <= kRWX ? k[code] : PAGE_NOACCESS;
}
int console_bits(uint8_t code) {
    static constexpr int k[] = {0, PS5_KERNEL_PROT_CPU_READ, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_EXEC,
                                PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_CPU_EXEC};
    return code <= kRWX ? k[code] : 0;
}
bool valid_protect(DWORD protect) {
    const DWORD p = protect & ~DWORD(PAGE_GUARD | PAGE_NOCACHE);
    return p == PAGE_NOACCESS || p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE ||
           p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

// One spinlock per stripe of console pages: held while a console page's protection is recomputed and set.
std::atomic<bool> g_stripes[1024];
struct StripeLock {
    std::atomic<bool> &s;
    explicit StripeLock(uint64_t page) : s(g_stripes[(page / kConsolePage) % 1024]) {
        while (s.exchange(true, std::memory_order_acquire)) __builtin_ia32_pause();
    }
    ~StripeLock() { s.store(false, std::memory_order_release); }
};

// No access is refused on some direct memory: read-only is the nearest the console gives (ps5platform does the same).
void protect_page(uint64_t page, int bits) {
    if (sceKernelMprotect(reinterpret_cast<void *>(page), kConsolePage, bits) != 0 && bits == 0)
        sceKernelMprotect(reinterpret_cast<void *>(page), kConsolePage, PS5_KERNEL_PROT_CPU_READ);
}

// Recomputes and sets the console protection of every backed console page in [start, end) that holds a committed 4 KiB
// page: the union of their protections. Pages with none committed keep what they have (reserved, or no access); pages
// with nothing mapped yet get theirs when they are backed.
void apply(uint64_t start, uint64_t end) {
    for (uint64_t p = down(start, kConsolePage); p < up(end, kConsolePage); p += kConsolePage) {
        StripeLock lk(p);
        int bits = 0;
        bool any = false;
        for (uint64_t a = p; a < p + kConsolePage; a += kWinPage) {
            const uint8_t e = get(a);
            if (state_of(e) == kCommitted && backed_of(e)) {
                any = true;
                bits |= console_bits(prot_of(e));
            }
        }
        if (any) protect_page(p, bits);
    }
}

// ---- allocations ----

struct Section;
struct Alloc {
    uint64_t base = 0, size = 0;   // the reservation (64 KiB aligned and sized)
    Section *section = nullptr;    // a view of it, or private memory
    uint64_t offset = 0;           // view: section offset of base
    uint8_t access = kRW;          // view: the protection its committed pages get
};

struct Section : Object {
    Section() : Object(Kind::Section) {}
    uint64_t size = 0;
    std::vector<int64_t> units;            // direct-memory start of each 64 KiB unit, or -1
    std::vector<uint8_t> committed;        // per 4 KiB page of the section
    std::vector<Alloc *> views;
    ~Section() override {
        for (int64_t u : units)
            if (u >= 0) sceKernelReleaseDirectMemory(u, kUnit);
    }
};

std::mutex g_mu;                           // allocations and sections (never taken by VirtualProtect)
std::map<uint64_t, Alloc> g_allocs;        // by base

Alloc *alloc_at(uint64_t addr) {
    auto it = g_allocs.upper_bound(addr);
    if (it == g_allocs.begin()) return nullptr;
    --it;
    return addr - it->second.base < it->second.size ? &it->second : nullptr;
}

bool reserve(uint64_t at, uint64_t size, uint64_t *base) {
    if (at) {
        if (at % kUnit || ps5_vrange_reserve_at(reinterpret_cast<void *>(at), size) != 0) return false;
        *base = at;
        return true;
    }
    void *b = nullptr;
    if (ps5_vrange_reserve(size, nullptr, kUnit, &b) != 0) return false;
    *base = reinterpret_cast<uint64_t>(b);
    return true;
}

// Maps a section unit into one view, read-write, then gives its pages the view's protections.
bool map_unit(Section &s, size_t unit, const Alloc &v) {
    const uint64_t unit_off = unit * kUnit;
    if (unit_off + kUnit <= v.offset || unit_off >= v.offset + v.size) return true;   // not in this view
    const uint64_t lo = std::max(unit_off, v.offset), hi = std::min(unit_off + kUnit, v.offset + v.size);
    void *at = reinterpret_cast<void *>(v.base + (lo - v.offset));
    const int r = sceKernelMapDirectMemory(&at, up(hi - lo, kConsolePage), PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                           PS5_KERNEL_MAP_FIXED, s.units[unit] + int64_t(lo - unit_off), 0);
    return r == 0;
}

// Sets or clears the backed bit of the pages [start, end), keeping their state and protection.
void mark_backed(uint64_t start, uint64_t end, bool backed) {
    for (uint64_t a = start; a < end; a += kWinPage) {
        uint8_t *slot = &leaf(a, true)[(a % kLeafSpan) / kWinPage];
        const uint8_t e = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
        __atomic_store_n(slot, uint8_t(backed ? (e | kBackedBit) : (e & ~kBackedBit)), __ATOMIC_RELEASE);
    }
}

// Gives a section unit its direct memory (g_mu held): allocated, mapped into every view, zeroed (released direct memory
// comes back with its old contents), its pages' recorded protections applied (uncommitted ones no access).
bool back_unit(Section &s, size_t u) {
    if (s.units[u] >= 0) return true;
    int64_t phys = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), kUnit, kUnit, 0, &phys) != 0) return false;
    s.units[u] = phys;
    for (Alloc *v : s.views)
        if (!map_unit(s, u, *v)) return false;
    bool zeroed = false;
    for (Alloc *v : s.views) {
        const uint64_t lo = std::max<uint64_t>(u * kUnit, v->offset), hi = std::min<uint64_t>(u * kUnit + kUnit, v->offset + v->size);
        if (lo >= hi) continue;
        const uint64_t a = v->base + (lo - v->offset), b = v->base + (hi - v->offset);
        if (!zeroed && lo == u * kUnit && hi == u * kUnit + kUnit) {
            memset(reinterpret_cast<void *>(a), 0, kUnit);
            zeroed = true;
        }
        for (uint64_t p = a; p < b; p += kConsolePage) {
            StripeLock sl(p);
            protect_page(p, 0);
        }
        mark_backed(a, b, true);
        apply(a, b);
    }
    return true;
}

// Commits section pages [off, end) seen through view `by` with `prot`; other views get their own access. As on Windows,
// a SEC_RESERVE commit charges nothing yet: each 64 KiB unit gets its memory when a page of it is first touched
// (section_fault) or handed to the kernel (touch). Committing gigabytes is then a table update, where backing them at
// once took about 40 s of the P2 boot and held the console's direct memory before the game had used any of it.
bool section_commit(Section &s, uint64_t off, uint64_t end, const Alloc &by, uint8_t prot) {
    for (uint64_t p = off / kWinPage; p < end / kWinPage; ++p) s.committed[p] = 1;
    for (Alloc *v : s.views) {
        const uint64_t lo = std::max(off, v->offset), hi = std::min(end, v->offset + v->size);
        if (lo >= hi) continue;
        for (uint64_t x = lo; x < hi;) {   // unit by unit: each run's backed bit follows its unit
            const uint64_t u = x / kUnit, y = std::min<uint64_t>(hi, (u + 1) * kUnit);
            const uint64_t a = v->base + (x - v->offset), b = v->base + (y - v->offset);
            set_range(a, b, entry(kCommitted, v == &by ? prot : v->access, true, s.units[u] >= 0));
            if (s.units[u] >= 0) apply(a, b);
            x = y;
        }
    }
    return true;
}

// The section unit behind a committed, unbacked view page at addr (g_mu held): true when it was backed now.
bool back_view_page(uint64_t addr) {
    const uint8_t e = get(addr);
    if (state_of(e) != kCommitted || !view_of(e) || backed_of(e)) return false;
    Alloc *v = alloc_at(addr);
    if (!v || !v->section) return false;
    return back_unit(*v->section, (v->offset + (addr - v->base)) / kUnit);
}

}  // namespace

bool section_fault(uint64_t addr) {
    if (state_of(get(addr)) != kCommitted || !view_of(get(addr))) return false;   // not a view page: no lock taken
    std::lock_guard lk(g_mu);
    back_view_page(addr);
    const uint8_t e = get(addr);   // backed now, or by another thread that faulted on the same unit first
    return state_of(e) == kCommitted && view_of(e) && backed_of(e);
}

void touch(const void *p, size_t n) {
    if (!n) return;
    const uint64_t a = reinterpret_cast<uint64_t>(p);
    if (a >= kLimit) return;
    const uint64_t end = std::min<uint64_t>(a + n, kLimit);
    bool locked = false;
    for (uint64_t q = down(a, kWinPage); q < end; q += kWinPage) {
        const uint8_t e = get(q);
        if (state_of(e) != kCommitted || !view_of(e) || backed_of(e)) continue;
        if (!locked) { g_mu.lock(); locked = true; }
        back_view_page(q);
    }
    if (locked) g_mu.unlock();
}

}  // namespace ps5compat

using namespace ps5compat;

// ---------------------------------------------------------------------------------------------------------------------
// VirtualAlloc / VirtualFree / VirtualProtect / VirtualQuery

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    crumb("VirtualAlloc %p %#llx type %#x prot %#x", address, static_cast<unsigned long long>(size), type, protect);
    const uint64_t addr = reinterpret_cast<uint64_t>(address);
    if (!size || !(type & (MEM_RESERVE | MEM_COMMIT)) || !valid_protect(protect) || addr >= kLimit || size > kLimit - addr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const uint8_t prot = code_of(protect);
    std::lock_guard lk(g_mu);
    uint64_t base = 0;
    if (type & MEM_RESERVE) {
        const uint64_t at = down(addr, kUnit);
        const uint64_t span = up(addr + size, kUnit) - at;
        if (addr && alloc_at(at)) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
        if (!reserve(at, span, &base)) {
            SetLastError(addr ? ERROR_INVALID_ADDRESS : ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        g_allocs[base] = Alloc{base, span, nullptr, 0, kRW};
        set_range(base, base + span, entry(kReserved, kNone, false, false));
        if (!(type & MEM_COMMIT)) return reinterpret_cast<LPVOID>(base);   // rounded down to 64 KiB, as Windows does
    }
    const uint64_t start = down(addr ? addr : base, kWinPage);
    const uint64_t end = up((addr ? addr : base) + size, kWinPage);
    Alloc *a = alloc_at(start);
    if (!a || end > a->base + a->size) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    if (a->section) {
        Section &s = *a->section;
        if (!section_commit(s, a->offset + (start - a->base), a->offset + (end - a->base), *a, prot)) return nullptr;
        return reinterpret_cast<LPVOID>(start);
    }
    // Private memory: back the console pages (ps5platform keeps what is backed already), then set the protections.
    if (ps5_vrange_commit(reinterpret_cast<void *>(down(start, kConsolePage)), up(end, kConsolePage) - down(start, kConsolePage),
                          console_bits(prot)) != 0) {
        SetLastError(ERROR_COMMITMENT_LIMIT);
        return nullptr;
    }
    set_range(start, end, entry(kCommitted, prot, false, true));
    apply(start, end);
    return reinterpret_cast<LPVOID>(start);
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
    crumb("VirtualFree %p %#llx type %#x", address, static_cast<unsigned long long>(size), type);
    const uint64_t addr = reinterpret_cast<uint64_t>(address);
    std::lock_guard lk(g_mu);
    Alloc *a = alloc_at(addr);
    if (!a || a->section) {   // views go with UnmapViewOfFile; their pages cannot be decommitted (as on Windows)
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (type & MEM_RELEASE) {
        if (size != 0 || addr != a->base) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        const uint64_t base = a->base, span = a->size;
        if (ps5_vrange_release(reinterpret_cast<void *>(base), span) != 0) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        set_range(base, base + span, entry(kFree, kNone, false, false));
        g_allocs.erase(base);
        return TRUE;
    }
    if (type & MEM_DECOMMIT) {
        const uint64_t start = down(addr, kWinPage);
        const uint64_t end = size ? up(addr + size, kWinPage) : a->base + a->size;
        if (end > a->base + a->size) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        ps5_vrange_decommit(reinterpret_cast<void *>(start), end - start);
        set_range(start, end, entry(kReserved, kNone, false, false));
        apply(start, end);   // console pages shared with still-committed 4 KiB pages get their protection back
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old_protect) {
    crumb("VirtualProtect %p %#llx prot %#x", address, static_cast<unsigned long long>(size), protect);
    const uint64_t addr = reinterpret_cast<uint64_t>(address);
    if (!size || !valid_protect(protect) || addr >= kLimit || size > kLimit - addr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const uint64_t start = down(addr, kWinPage), end = up(addr + size, kWinPage);
    // Every page must be committed (Windows fails the whole call otherwise, changing nothing).
    for (uint64_t a = start; a < end; a += kWinPage)
        if (state_of(get(a)) != kCommitted) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
    if (old_protect) *old_protect = protect_of(prot_of(get(start)));
    const uint8_t prot = code_of(protect);
    for (uint64_t a = start; a < end; a += kWinPage) set_range(a, a + kWinPage, entry(kCommitted, prot, view_of(get(a)), backed_of(get(a))));
    apply(start, end);
    return TRUE;
}

SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length) {
    if (!info || length < sizeof(MEMORY_BASIC_INFORMATION)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const uint64_t addr = reinterpret_cast<uint64_t>(address);
    if (addr >= kLimit) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint64_t page = down(addr, kWinPage);
    const uint8_t e = get(page);
    *info = MEMORY_BASIC_INFORMATION{};
    info->BaseAddress = reinterpret_cast<PVOID>(page);
    if (state_of(e) == kFree) {
        // Not ours: memory the console's libraries mapped (heap, stacks, code) answers from the kernel.
        void *s = nullptr, *t = nullptr;
        uint32_t kp = 0;
        if (sceKernelQueryMemoryProtection(reinterpret_cast<void *>(page), &s, &t, &kp) == 0 &&
            reinterpret_cast<uint64_t>(s) <= page && reinterpret_cast<uint64_t>(t) > page) {
            const bool r = kp & PS5_KERNEL_PROT_CPU_READ, w = kp & PS5_KERNEL_PROT_CPU_WRITE, x = kp & PS5_KERNEL_PROT_CPU_EXEC;
            info->AllocationBase = s;
            info->RegionSize = reinterpret_cast<uint64_t>(t) - page;
            info->State = MEM_COMMIT;
            info->Type = MEM_PRIVATE;
            info->Protect = info->AllocationProtect =
                x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : w ? PAGE_READWRITE : r ? PAGE_READONLY : PAGE_NOACCESS;
            return sizeof(MEMORY_BASIC_INFORMATION);
        }
        uint64_t end = page + kWinPage;
        while (end < kLimit && state_of(get(end)) == kFree) end = leaf(end, false) ? end + kWinPage : down(end, kLeafSpan) + kLeafSpan;
        info->AllocationBase = nullptr;
        info->RegionSize = std::min(end, kLimit) - page;
        info->State = MEM_FREE;
        info->Protect = PAGE_NOACCESS;
        return sizeof(MEMORY_BASIC_INFORMATION);
    }
    uint64_t end = page + kWinPage;
    while (end < kLimit && get(end) == e) end += kWinPage;
    // The allocation's base and size need the allocation map; a caller that faulted while another thread holds it (a
    // fault handler) gets the region's own base instead of waiting.
    uint64_t alloc_base = page, alloc_end = end;
    if (g_mu.try_lock()) {
        if (Alloc *a = alloc_at(page)) {
            alloc_base = a->base;
            alloc_end = a->base + a->size;
        }
        g_mu.unlock();
    }
    info->AllocationBase = reinterpret_cast<PVOID>(alloc_base);
    info->AllocationProtect = view_of(e) ? PAGE_READWRITE : PAGE_NOACCESS;
    info->RegionSize = std::min(end, alloc_end) - page;
    info->State = state_of(e) == kCommitted ? MEM_COMMIT : MEM_RESERVE;
    info->Protect = state_of(e) == kCommitted ? protect_of(prot_of(e)) : 0;
    info->Type = view_of(e) ? MEM_MAPPED : MEM_PRIVATE;
    return sizeof(MEMORY_BASIC_INFORMATION);
}

BOOL VirtualLock(LPVOID, SIZE_T) { return TRUE; }   // direct memory is never paged out
BOOL FlushInstructionCache(HANDLE, LPCVOID, SIZE_T) { return TRUE; }   // x86-64: coherent

BOOL IsBadReadPtr(const void *address, UINT_PTR size) {
    const uint64_t start = reinterpret_cast<uint64_t>(address);
    if (!size) return FALSE;
    for (uint64_t a = down(start, kWinPage); a < start + size; a += kWinPage) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(reinterpret_cast<void *>(a), &m, sizeof m) || m.State != MEM_COMMIT ||
            (m.Protect & 0xFF) == PAGE_NOACCESS)
            return TRUE;
    }
    return FALSE;
}

void GetSystemInfo(LPSYSTEM_INFO info) {
    // Windows' values, which the runtime's layout is built on; the console's 16 KiB pages are this layer's concern.
    *info = SYSTEM_INFO{};
    info->wProcessorArchitecture = 9;   // PROCESSOR_ARCHITECTURE_AMD64
    info->dwPageSize = kWinPage;
    info->dwAllocationGranularity = kUnit;
    info->lpMinimumApplicationAddress = reinterpret_cast<LPVOID>(0x10000);
    info->lpMaximumApplicationAddress = reinterpret_cast<LPVOID>(kLimit - 1);
    info->dwNumberOfProcessors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    GetProcessAffinityMask(GetCurrentProcess(), &info->dwActiveProcessorMask, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------------
// file mappings: page-file sections only

HANDLE CreateFileMappingW(HANDLE file, void *, DWORD protect, DWORD size_high, DWORD size_low, LPCWSTR) {
    if (file != INVALID_HANDLE_VALUE) {   // file-backed mappings: the runtime makes none
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    const uint64_t size = up((uint64_t(size_high) << 32) | size_low, kUnit);
    if (!size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    fault_install();   // section pages are backed on first touch, through the fault handler
    auto *s = new Section;
    s->size = size;
    s->units.assign(size / kUnit, -1);
    s->committed.assign(size / kWinPage, 0);
    // SEC_COMMIT (the default) commits every page; that is done at the section's first view (MapViewOfFileEx sees the
    // pages marked 2 and commits them through it), so no memory is used before anything can see it.
    if (!(protect & SEC_RESERVE)) std::fill(s->committed.begin(), s->committed.end(), 2);
    SetLastError(ERROR_SUCCESS);
    return s;
}

HANDLE CreateFileMappingA(HANDLE file, void *security, DWORD protect, DWORD size_high, DWORD size_low, LPCSTR) {
    return CreateFileMappingW(file, security, protect, size_high, size_low, nullptr);
}

LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T size, LPVOID address) {
    crumb("MapViewOfFileEx off %#llx size %#llx at %p", static_cast<unsigned long long>((uint64_t(offset_high) << 32) | offset_low), static_cast<unsigned long long>(size), address);
    auto *s = as<Section>(mapping, Object::Kind::Section);
    if (!s) return nullptr;
    const uint64_t off = (uint64_t(offset_high) << 32) | offset_low;
    if (!size) size = off < s->size ? s->size - off : 0;
    if (off % kUnit || !size || off + size > s->size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const uint64_t at = reinterpret_cast<uint64_t>(address);
    const uint64_t span = up(size, kUnit);
    std::lock_guard lk(g_mu);
    uint64_t base = 0;
    if ((at && alloc_at(at)) || !reserve(at, span, &base)) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    auto [it, fresh] = g_allocs.emplace(base, Alloc{base, span, s, off, (access & FILE_MAP_WRITE) ? kRW : kR});
    (void)fresh;
    Alloc &v = it->second;
    retain(s);   // a view keeps its section
    s->views.push_back(&v);
    set_range(base, base + span, entry(kReserved, kNone, true, false));
    for (size_t u = off / kUnit; u < up(off + size, kUnit) / kUnit; ++u)
        if (s->units[u] >= 0 && !map_unit(*s, u, v)) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
    // Pages the section has committed are committed in the new view, with its access. (A SEC_COMMIT section's pages,
    // marked 2, are committed here at their first view.)
    for (uint64_t p = off / kWinPage; p < (off + size) / kWinPage;) {
        if (!s->committed[p]) { ++p; continue; }
        uint64_t q = p;
        while (q < (off + size) / kWinPage && s->committed[q]) ++q;
        if (s->committed[p] == 2) {
            for (uint64_t k = p; k < q; ++k) s->committed[k] = 0;
            if (!section_commit(*s, p * kWinPage, q * kWinPage, v, v.access)) return nullptr;
        } else {
            for (uint64_t k = p; k < q; ++k)   // backed where the section unit already has memory
                set_range(base + (k * kWinPage - off), base + ((k + 1) * kWinPage - off),
                          entry(kCommitted, v.access, true, s->units[k * kWinPage / kUnit] >= 0));
            apply(base + (p * kWinPage - off), base + (q * kWinPage - off));
        }
        p = q;
    }
    return reinterpret_cast<LPVOID>(base);
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T size) {
    return MapViewOfFileEx(mapping, access, offset_high, offset_low, size, nullptr);
}

BOOL UnmapViewOfFile(LPCVOID view) {
    crumb("UnmapViewOfFile %p", view);
    const uint64_t base = reinterpret_cast<uint64_t>(view);
    Section *s = nullptr;
    {
        std::lock_guard lk(g_mu);
        auto it = g_allocs.find(base);
        if (it == g_allocs.end() || !it->second.section) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        s = it->second.section;
        const uint64_t span = it->second.size;
        ps5_vrange_release(reinterpret_cast<void *>(base), span);   // the direct-memory mappings in it go too
        set_range(base, base + span, entry(kFree, kNone, false, false));
        s->views.erase(std::remove(s->views.begin(), s->views.end(), &it->second), s->views.end());
        g_allocs.erase(it);
    }
    release(s);
    return TRUE;
}

#pragma once

// Whether a few bytes of this process can be read without a fault, answered by
// the page's working set entry instead of VirtualQuery when they lie in one
// resident page.
//
// ped_blood.cpp asks before it reads a ped's damage component (a ped torn down
// can give an address on a stack guard page). VirtualQuery answers by walking
// the page tables of the whole region to report its extent, and on the engine's
// ped and component pools one query takes 5.5 to 20 ms, one or two per blood
// apply. QueryWorkingSetEx reads that one page's entry. A page in the working
// set (Valid) is committed and its Win32Protection is the page's protection,
// the one VirtualQuery reports for the region holding it (a region is a run of
// pages with one state and one protection); a guard page is not Valid (its
// first touch faults); and a range inside the page lies inside that region. So
// for such a range the answer is VirtualQuery's. Any other case (two pages, a
// page out of the working set, a failed call) is left to VirtualQuery.
// tests/playback_perf_tests.cpp compares both answers on this process's own
// committed, untouched, read-only, no-access, guard, reserved, freed, code and
// stack pages.

#include <windows.h>
#include <psapi.h>

#include <cstddef>
#include <cstdint>

namespace replay::mem
{
    constexpr DWORD kReadableProtect = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    constexpr std::uintptr_t kPageBytes = 0x1000;

    // The protection test ped_blood.cpp applies to VirtualQuery's answer.
    inline bool ReadableProtect(DWORD protect)
    {
        return !(protect & (PAGE_GUARD | PAGE_NOACCESS)) && (protect & kReadableProtect) != 0;
    }

    // VirtualQuery's answer (ped_blood.cpp Readable after a cache miss): the
    // region holding `address` is committed and readable without a guard,
    // and the range ends inside it. `base` and `end` are that region (0 when
    // it is not such a region), for the caller's region cache; `queried`
    // false when VirtualQuery failed.
    inline bool ReadableByQuery(const void* address, std::size_t bytes, std::uintptr_t& base, std::uintptr_t& end,
        bool& queried)
    {
        base = 0;
        end = 0;
        MEMORY_BASIC_INFORMATION info{};
        queried = VirtualQuery(address, &info, sizeof(info)) == sizeof(info);
        if (!queried || info.State != MEM_COMMIT || !ReadableProtect(info.Protect))
            return false;
        base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        end = base + info.RegionSize;
        return reinterpret_cast<std::uintptr_t>(address) + bytes <= end;
    }

    // The working set entry's answer for a non-empty range inside one page
    // that is in the working set; `answered` false otherwise (ask
    // VirtualQuery).
    inline bool ReadableByWorkingSet(const void* address, std::size_t bytes, bool& answered)
    {
        answered = false;
        const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(address);
        if (!first || bytes == 0 || bytes > kPageBytes)
            return false;
        const std::uintptr_t last = first + bytes - 1;
        if (last < first || (first & ~(kPageBytes - 1)) != (last & ~(kPageBytes - 1)))
            return false;
        PSAPI_WORKING_SET_EX_INFORMATION entry{};
        entry.VirtualAddress = reinterpret_cast<PVOID>(first & ~(kPageBytes - 1));
        if (!K32QueryWorkingSetEx(GetCurrentProcess(), &entry, sizeof(entry)) || !entry.VirtualAttributes.Valid)
            return false;
        answered = true;
        return ReadableProtect(static_cast<DWORD>(entry.VirtualAttributes.Win32Protection));
    }
}

#pragma once

// The pure decisions of the crash handler (script.cpp CrashEvidenceHandler).
//
// - AccessName: ExceptionInformation[0] of an access violation or an in
//   page error (0 read, 1 write, 8 execute: DEP or an unmapped page reached
//   by a jump).
// - ScopeCoversPc: whether a frame of this module is inside a __try whose
//   __except will take the exception (the x64 C scope table that
//   __C_specific_handler walks). The handler unwinds the faulting thread's
//   stack: a frame of ours with such a scope means a guarded probe of the
//   mod (they fault on purpose, often), anything else a real crash.
// - DumpsToDelete: which of the old dumps go so the folder keeps the newest
//   `keep` after the new one is written.
// - DumpFileName: RDR2_<build>_<yyyymmdd-hhmmss>_pid<pid>_t<thread>.dmp

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace replay::crash
{
    // EXCEPTION_ACCESS_VIOLATION and EXCEPTION_IN_PAGE_ERROR (winnt.h).
    constexpr std::uint32_t kAccessViolation = 0xC0000005u;
    constexpr std::uint32_t kInPageError = 0xC0000006u;

    inline const char* AccessName(std::uint32_t code, std::uint32_t parameterCount, std::uint64_t info0) noexcept
    {
        if ((code != kAccessViolation && code != kInPageError) || parameterCount < 1)
            return "none";
        switch (info0)
        {
        case 0: return "read";
        case 1: return "write";
        case 8: return "execute";
        default: return "other";
        }
    }

    // One record of the x64 C scope table (UNWIND_INFO handler data of a
    // function compiled with __try): RVAs relative to the image base.
    struct ScopeRecord
    {
        std::uint32_t beginAddress;
        std::uint32_t endAddress;
        std::uint32_t handlerAddress; // filter RVA, or 1 for EXCEPTION_EXECUTE_HANDLER
        std::uint32_t jumpTarget;     // 0 for a __finally (never takes an exception)
    };

    // A plausible scope table: a small count of records inside the image.
    // C++ EH data (FuncInfo, FH4) read the same way gives a count that is an
    // RVA or a packed blob, so a wrong guess is refused, never matched.
    inline bool PlausibleScopeTable(std::uint32_t count, const ScopeRecord* records, std::uint32_t imageSize) noexcept
    {
        if (count == 0 || count > 64 || !records)
            return false;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            const ScopeRecord& r = records[n];
            if (r.beginAddress >= r.endAddress || r.endAddress > imageSize || r.jumpTarget >= imageSize)
                return false;
        }
        return true;
    }

    // The frame's pc (RVA) inside an __except scope (not a __finally).
    inline bool ScopeCoversPc(std::uint32_t count, const ScopeRecord* records, std::uint32_t imageSize, std::uint32_t pcRva) noexcept
    {
        if (!PlausibleScopeTable(count, records, imageSize))
            return false;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            const ScopeRecord& r = records[n];
            if (r.jumpTarget != 0 && pcRva >= r.beginAddress && pcRva < r.endAddress)
                return true;
        }
        return false;
    }

    // Old dumps to delete before a new one is written so that, with it, the
    // folder keeps the `keep` newest. `stamps[n]` are the files' write times
    // (any increasing unit); `toDelete` gets their indexes, oldest first.
    // Returns how many.
    inline std::size_t DumpsToDelete(const std::uint64_t* stamps, std::size_t count, std::size_t keep, std::size_t* toDelete,
        std::size_t capacity) noexcept
    {
        if (keep == 0 || count + 1 <= keep)
            return 0;
        const std::size_t excess = count + 1 - keep;
        std::size_t written = 0;
        bool taken[256] = {};
        for (std::size_t round = 0; round < excess && written < capacity && round < 256; ++round)
        {
            std::size_t oldest = count;
            for (std::size_t n = 0; n < count && n < 256; ++n)
                if (!taken[n] && (oldest == count || stamps[n] < stamps[oldest]))
                    oldest = n;
            if (oldest == count)
                break;
            taken[oldest] = true;
            toDelete[written++] = oldest;
        }
        return written;
    }

    // "RDR2_<build>_20261005-020916_pid1234_t4304.dmp" (characters a file
    // name cannot have are replaced in the build text).
    inline int DumpFileName(wchar_t* out, std::size_t capacity, const char* build, unsigned year, unsigned month, unsigned day,
        unsigned hour, unsigned minute, unsigned second, unsigned long pid, unsigned long thread) noexcept
    {
        wchar_t safe[48] = {};
        std::size_t at = 0;
        for (const char* c = build ? build : "unknown"; *c && at + 1 < sizeof(safe) / sizeof(safe[0]); ++c)
        {
            const char ch = *c;
            const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '-' || ch == '_';
            safe[at++] = ok ? static_cast<wchar_t>(ch) : L'_';
        }
        safe[at] = 0;
        return std::swprintf(out, capacity, L"RDR2_%ls_%04u%02u%02u-%02u%02u%02u_pid%lu_t%lu.dmp", safe, year, month, day, hour, minute,
            second, pid, thread);
    }
}

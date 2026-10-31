#include "engine_calls.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace replay::engine
{
    namespace
    {
        // One row per function that faulted, claimed by the name's address
        // (each Function's name is one literal). Any thread notes, the script
        // thread polls: no lock (a fault may come from a detour on any thread).
        struct Row
        {
            std::atomic<const char*> name{nullptr};
            std::atomic<std::uint32_t> count{0};
            std::atomic<std::uint32_t> firstCode{0};
            std::atomic<std::uintptr_t> firstAddress{0};
            std::atomic<std::uint32_t> loggedCount{0};
        };

        // The first fault's code and address, set by whichever thread gets
        // there first and always before its count (the poll reads the count
        // first, so a counted fault has its code).
        void Count(Row& row, std::uint32_t code, std::uintptr_t address)
        {
            std::uint32_t noCode = 0;
            if (row.firstCode.compare_exchange_strong(noCode, code ? code : 1u, std::memory_order_relaxed))
                row.firstAddress.store(address, std::memory_order_relaxed);
            row.count.fetch_add(1, std::memory_order_release);
        }
        constexpr std::size_t kRows = 128;
        Row g_rows[kRows];
        std::atomic<std::uint32_t> g_overflow{0};
        thread_local std::uint32_t t_lastFault = 0;
        thread_local std::uintptr_t t_lastFaultAddress = 0;
    }

    int NoteFault(const char* name, void* exception)
    {
        const auto* pointers = static_cast<const EXCEPTION_POINTERS*>(exception);
        const EXCEPTION_RECORD* record = pointers ? pointers->ExceptionRecord : nullptr;
        const std::uint32_t code = record ? record->ExceptionCode : 0;
        t_lastFault = code;
        t_lastFaultAddress = record ? reinterpret_cast<std::uintptr_t>(record->ExceptionAddress) : 0;
        for (Row& row : g_rows)
        {
            const char* owner = row.name.load(std::memory_order_acquire);
            if (!owner)
            {
                const char* expected = nullptr;
                if (row.name.compare_exchange_strong(expected, name, std::memory_order_acq_rel))
                {
                    Count(row, code, t_lastFaultAddress);
                    return EXCEPTION_EXECUTE_HANDLER;
                }
                owner = expected;
            }
            if (owner == name)
            {
                Count(row, code, t_lastFaultAddress);
                return EXCEPTION_EXECUTE_HANDLER;
            }
        }
        g_overflow.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    std::uint32_t LastFault()
    {
        return t_lastFault;
    }

    std::uintptr_t LastFaultAddress()
    {
        return t_lastFaultAddress;
    }

    void Raise()
    {
        RaiseException(t_lastFault, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        for (;;)
        {
        }
    }

    void PollFaults(std::ostream& log)
    {
        for (Row& row : g_rows)
        {
            const char* name = row.name.load(std::memory_order_acquire);
            if (!name)
                break;
            // The first fault, then again each time the count doubles.
            const std::uint32_t count = row.count.load(std::memory_order_acquire);
            const std::uint32_t logged = row.loggedCount.load(std::memory_order_relaxed);
            if (!count || count < 2 * logged || count == logged)
                continue;
            row.loggedCount.store(count, std::memory_order_relaxed);
            const std::uintptr_t address = row.firstAddress.load(std::memory_order_relaxed);
            const std::uintptr_t exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            char line[224];
            std::snprintf(line, sizeof(line), "ENGINE_CALL_FAULT,function=%s,code=0x%08X,count=%u,address=0x%llX,exe_rva=0x%llX\n",
                name, row.firstCode.load(std::memory_order_relaxed), count, static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(address > exe ? address - exe : 0));
            log << line;
        }
    }
}

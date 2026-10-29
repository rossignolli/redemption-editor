#pragma once

// The crash dump request's own copy of the exception (script.cpp
// ClaimCrashDump, WriteRequestedDump).
//
// The faulting thread hands the dump thread an exception to write and waits
// for it, at most kDumpWaitMs. The EXCEPTION_POINTERS the vectored handler
// receives, its EXCEPTION_RECORD chain and its CONTEXT live on the faulting
// thread's stack and belong to the dispatcher: once the handler returns
// (after a timeout, or because another handler or the game recovers from the
// fault) that stack is reused while MiniDumpWriteDump may still read it. So
// the request owns copies, made before the dump thread is signalled, in
// storage that exists for the whole load (no allocation in the handler).
//
// - The record chain is copied up to kMaxRecords; the copies link to each
//   other and the last copy ends the chain (a longer chain is cut, never
//   followed into the dispatcher's memory).
// - The CONTEXT is copied as the fixed structure. Its CONTEXT_XSTATE bit is
//   cleared: the extended state lives past the structure (CONTEXT_EX), which
//   is not copied, so the copy must not claim it.

#include <Windows.h>

#include <cstddef>
#include <cstring>

namespace replay::crash
{
    struct OwnedException
    {
        static constexpr std::size_t kMaxRecords = 8;

        CONTEXT context;                      // 16-byte aligned by its declaration
        EXCEPTION_RECORD records[kMaxRecords];
        EXCEPTION_POINTERS pointers;
        std::size_t recordCount;
    };

    // The extended-state bit of ContextFlags without the architecture bit.
    constexpr DWORD kContextXStateBit = CONTEXT_XSTATE & ~CONTEXT_AMD64;

    // Copies `source` into `out` and points out.pointers at the copies.
    // False (and out left empty) when there is nothing to copy. Plain
    // copies only: callable from the vectored exception handler.
    inline bool CopyException(const EXCEPTION_POINTERS* source, OwnedException& out) noexcept
    {
        out.pointers.ExceptionRecord = nullptr;
        out.pointers.ContextRecord = nullptr;
        out.recordCount = 0;
        if (!source || !source->ExceptionRecord || !source->ContextRecord)
            return false;
        std::memcpy(&out.context, source->ContextRecord, sizeof(CONTEXT));
        out.context.ContextFlags &= ~kContextXStateBit;
        std::size_t count = 0;
        for (const EXCEPTION_RECORD* record = source->ExceptionRecord; record && count < OwnedException::kMaxRecords;
             record = record->ExceptionRecord)
        {
            std::memcpy(&out.records[count], record, sizeof(EXCEPTION_RECORD));
            if (count > 0)
                out.records[count - 1].ExceptionRecord = &out.records[count];
            ++count;
        }
        out.records[count - 1].ExceptionRecord = nullptr;
        out.recordCount = count;
        out.pointers.ExceptionRecord = &out.records[0];
        out.pointers.ContextRecord = &out.context;
        return true;
    }
}

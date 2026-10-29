// The pure parts of the crash handler (src/crash_evidence_rules.h) and the
// thread name table (src/thread_names.h).
#include "crash_dump_copy.h"
#include "crash_evidence_rules.h"
#include "thread_names.h"

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace
{
    int g_crashFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_crashFailures;
    }

    void TestAccessName()
    {
        using namespace replay::crash;
        Check(std::strcmp(AccessName(kAccessViolation, 2, 8), "execute") == 0, "info0 = 8 is an execute fault (the 02:09 crash: a jump into the heap)");
        Check(std::strcmp(AccessName(kAccessViolation, 2, 0), "read") == 0, "info0 = 0 is a read");
        Check(std::strcmp(AccessName(kAccessViolation, 2, 1), "write") == 0, "info0 = 1 is a write");
        Check(std::strcmp(AccessName(kInPageError, 3, 0), "read") == 0, "an in page error has the same parameter");
        Check(std::strcmp(AccessName(0xC0000094u, 0, 0), "none") == 0, "a divide by zero has no access");
        Check(std::strcmp(AccessName(kAccessViolation, 0, 0), "none") == 0, "no parameter, no access");
    }

    void TestScopes()
    {
        using namespace replay::crash;
        // Two __except scopes and a __finally, image of 0x10000 bytes.
        const ScopeRecord scopes[] = {{0x1000, 0x1040, 1, 0x1050}, {0x2000, 0x2100, 0x2200, 0x2110}, {0x3000, 0x3010, 0x3020, 0}};
        Check(ScopeCoversPc(3, scopes, 0x10000, 0x1010), "inside an __except scope: guarded");
        Check(ScopeCoversPc(3, scopes, 0x10000, 0x20FF), "the last byte of a scope");
        Check(!ScopeCoversPc(3, scopes, 0x10000, 0x2100), "the end is outside");
        Check(!ScopeCoversPc(3, scopes, 0x10000, 0x3005), "a __finally never takes the exception");
        Check(!ScopeCoversPc(3, scopes, 0x10000, 0x5000), "outside every scope");
        // C++ EH data read as a scope table: an RVA where the count goes.
        Check(!ScopeCoversPc(0x00012345u, scopes, 0x10000, 0x1010), "an implausible count is refused");
        const ScopeRecord bad[] = {{0x1040, 0x1000, 1, 0x1050}};
        Check(!ScopeCoversPc(1, bad, 0x10000, 0x1010), "begin after end is refused");
        const ScopeRecord outside[] = {{0x1000, 0x20000, 1, 0x1050}};
        Check(!ScopeCoversPc(1, outside, 0x10000, 0x1010), "a scope past the image is refused");
    }

    void TestDumpRotation()
    {
        using namespace replay::crash;
        std::size_t remove[16]{};
        const std::uint64_t none[1] = {0};
        Check(DumpsToDelete(none, 0, 5, remove, 16) == 0, "an empty folder keeps everything");
        const std::uint64_t four[] = {40, 10, 30, 20};
        Check(DumpsToDelete(four, 4, 5, remove, 16) == 0, "4 old + the new one = 5: nothing goes");
        const std::uint64_t seven[] = {70, 10, 60, 20, 50, 30, 40};
        const std::size_t n = DumpsToDelete(seven, 7, 5, remove, 16);
        Check(n == 3 && remove[0] == 1 && remove[1] == 3 && remove[2] == 5, "7 old + the new one: the 3 oldest go, oldest first");
        Check(DumpsToDelete(seven, 7, 0, remove, 16) == 0, "keep 0 means no rotation");
    }

    void TestDumpName()
    {
        wchar_t name[160]{};
        replay::crash::DumpFileName(name, 160, "1.0.0-dev", 2026, 10, 5, 2, 9, 16, 1234, 4304);
        Check(std::wcscmp(name, L"RDR2_1.0.0-dev_20261005-020916_pid1234_t4304.dmp") == 0, "dump file name");
        replay::crash::DumpFileName(name, 160, "a b/c:d", 2026, 1, 2, 3, 4, 5, 6, 7);
        Check(std::wcscmp(name, L"RDR2_a_b_c_d_20260102-030405_pid6_t7.dmp") == 0, "characters a file name cannot have are replaced");
    }

    // The dump request owns its exception: the faulting thread's records can
    // be overwritten after its handler returned (the 60 s timeout) and the
    // copies stay as they were.
    void TestOwnedException()
    {
        using replay::crash::OwnedException;
        static OwnedException owned; // like the request's, outside the stack
        Check(!replay::crash::CopyException(nullptr, owned) && owned.pointers.ExceptionRecord == nullptr,
            "no exception, nothing to copy");

        EXCEPTION_RECORD chain[3]{};
        chain[0].ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        chain[0].NumberParameters = 2;
        chain[0].ExceptionInformation[1] = 0x1234;
        chain[0].ExceptionRecord = &chain[1];
        chain[1].ExceptionCode = 0xE06D7363u;
        chain[1].ExceptionRecord = &chain[2];
        chain[2].ExceptionCode = EXCEPTION_STACK_OVERFLOW;
        alignas(16) static CONTEXT context{};
        context.ContextFlags = CONTEXT_ALL | CONTEXT_XSTATE;
        context.Rip = 0x140001000;
        context.Rsp = 0x7FF000;
        EXCEPTION_POINTERS pointers{&chain[0], &context};
        Check(replay::crash::CopyException(&pointers, owned), "an exception is copied");

        // The dispatcher reuses the faulting thread's stack.
        std::memset(chain, 0xCD, sizeof(chain));
        std::memset(&context, 0xCD, sizeof(context));
        const EXCEPTION_POINTERS& copy = owned.pointers;
        const auto ownedRecord = [&](const EXCEPTION_RECORD* record) {
            return record >= owned.records && record < owned.records + OwnedException::kMaxRecords;
        };
        Check(copy.ContextRecord == &owned.context && copy.ContextRecord->Rip == 0x140001000 && copy.ContextRecord->Rsp == 0x7FF000,
            "the context is the request's copy");
        Check((copy.ContextRecord->ContextFlags & CONTEXT_ALL) == CONTEXT_ALL &&
                (copy.ContextRecord->ContextFlags & replay::crash::kContextXStateBit) == 0,
            "the copy keeps the fixed flags and drops the extended state it does not hold");
        Check(ownedRecord(copy.ExceptionRecord) && copy.ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                copy.ExceptionRecord->ExceptionInformation[1] == 0x1234,
            "the first record is the request's copy");
        const EXCEPTION_RECORD* second = copy.ExceptionRecord->ExceptionRecord;
        Check(ownedRecord(second) && second->ExceptionCode == 0xE06D7363u, "the chain links to the copies");
        const EXCEPTION_RECORD* third = second ? second->ExceptionRecord : nullptr;
        Check(ownedRecord(third) && third->ExceptionCode == EXCEPTION_STACK_OVERFLOW && third->ExceptionRecord == nullptr &&
                owned.recordCount == 3,
            "the last copy ends the chain");

        // A chain longer than the storage is cut, never followed further.
        EXCEPTION_RECORD loop[2]{};
        loop[0].ExceptionRecord = &loop[1];
        loop[1].ExceptionRecord = &loop[0]; // a cycle: only the bound stops it
        EXCEPTION_POINTERS looping{&loop[0], &context};
        Check(replay::crash::CopyException(&looping, owned) && owned.recordCount == OwnedException::kMaxRecords &&
                owned.records[OwnedException::kMaxRecords - 1].ExceptionRecord == nullptr,
            "a long chain is cut at the storage's size");
    }

    void TestThreadNames()
    {
        using namespace replay::threads;
        const DWORD self = GetCurrentThreadId();
        Check(Lookup(self)[0] == 0, "an unnamed thread has no name");
        {
            Scope scope("unit_test");
            Check(std::strcmp(Lookup(self), "unit_test") == 0, "the scope names the thread");
            Check(MapText().find("unit_test=" + std::to_string(self)) != std::string::npos, "the map lists it with its id");
            Name("renamed");
            Check(std::strcmp(Lookup(self), "renamed") == 0, "the same thread again reuses its slot");
        }
        Check(Lookup(self)[0] == 0, "the scope's end forgets it");
        NoteForeign("game_thread");
        Check(std::strcmp(Lookup(self), "game_thread") == 0, "a foreign thread is only noted");
        Forget();
        Check(Lookup(0)[0] == 0, "thread id 0 is never named");
    }
}

int RunCrashEvidenceTests()
{
    TestAccessName();
    TestScopes();
    TestDumpRotation();
    TestDumpName();
    TestOwnedException();
    TestThreadNames();
    if (g_crashFailures == 0)
        std::printf("crash evidence tests passed\n");
    return g_crashFailures;
}
